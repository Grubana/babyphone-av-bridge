#include "monitor_emulator.h"
#include "monitor_replies.h"
#include "frame.h"
#include "media.h"
#include "relay.h"          // dialMonitor (reclaim probe)
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <ctime>
#include <cstdio>
#include <cerrno>

namespace babycam {

static const unsigned char* findReply(uint16_t cmd, size_t& len) {
    for (size_t i = 0; i < monrep::kReplyCount; ++i)
        if (monrep::kReplies[i].cmd == cmd) { len = monrep::kReplies[i].len; return monrep::kReplies[i].data; }
    return nullptr;
}

static long nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec) * 1000L + ts.tv_nsec / 1000000L;
}

// The real monitor is the ACTIVE side: after the announce it PROACTIVELY polls the
// camera with this exact command sequence (captured identically across sessions),
// and the camera answers each. Being reactive (only replying to camera frames) is
// why the emulator stalled after 0x0017 -- the camera was waiting for these polls.
// 0x0024 is the ONE camera-initiated request (camera asks for the monitor's
// firmware info) and is answered reactively, so it is NOT in this proactive list.
static const uint16_t kScript[] = {
    0x000d,  // START/enable stream (kicks the camera into sending A/V)
    0x001b, 0x0027, 0x0008, 0x000b, 0x0017, 0x0006,
    0x001f, 0x0005, 0x000a, 0x001c, 0x0011,
};
static const size_t kScriptN = sizeof(kScript) / sizeof(kScript[0]);

// After the handshake the monitor loops this poll cycle + a periodic heartbeat.
static const uint16_t kSteady[] = { 0x0006, 0x0006, 0x0005 };

void runMonitorEmulator(int camFd, StreamHub& hub, const std::string& monitorIp, uint16_t monitorPort) {
    FrameReader reader;
    uint8_t buf[8192];

    // Reclaim probe: on a background thread (so it never hiccups the stream),
    // check every ~5s whether the real monitor is back. If it is, flag it and the
    // main loop returns -> the session ends -> the camera reconnects -> the bridge
    // dials the monitor and tees (Mode A). Both monitor and web then work.
    std::atomic<bool> monitorBack{false}, stopProbe{false};
    std::thread probe([&]{
        while (!stopProbe.load()) {
            for (int i = 0; i < 50 && !stopProbe.load(); ++i) ::usleep(100 * 1000);  // ~5s, interruptible
            if (stopProbe.load()) break;
            int fd = dialMonitor(monitorIp, monitorPort, 400);
            if (fd >= 0) { ::close(fd); monitorBack.store(true); break; }
        }
    });

    bool announced = false;    // announce has been sent (drives the poll script)
    size_t scriptPos = 0;      // next proactive handshake poll to send
    size_t steadyPos = 0;
    long nextSendAt = 0;       // monotonic ms gate for the next proactive send
    long lastHeartbeat = 0;
    long media = 0;
    long sessionStart = nowMs();
    long connectAt = sessionStart;
    long lastReportAt = sessionStart;
    long lastReportMedia = 0;
    long videoFrames = 0, audioFrames = 0;

    auto sendCmd = [&](uint16_t cmd) -> bool {
        size_t len = 0; const unsigned char* r = findReply(cmd, len);
        if (!r) { std::fprintf(stderr, "[emu] !! no canned frame for 0x%04x\n", cmd); return false; }
        return ::send(camFd, r, len, 0) == (ssize_t)len;
    };
    auto beginDriving = [&](const char* why) {
        long now = nowMs();
        ::send(camFd, monrep::announce, monrep::announce_len, 0);
        announced = true;
        nextSendAt = now;                 // start the proactive script now
        lastHeartbeat = now;
        std::fprintf(stderr, "[emu] announce sent (%s); starting proactive poll script\n", why);
    };

    std::fprintf(stderr, "[emu] session start (proactive monitor driver)\n");
    while (true) {
        long now = nowMs();

        // Real monitor is back -> end the emulated session so the camera reconnects
        // and the bridge hands it to the monitor (Mode A tee).
        if (monitorBack.load()) {
            std::fprintf(stderr, "[emu] real monitor is back -> ending Mode B to hand back (media=%ld)\n", media);
            break;
        }

        // Cold-start fallback: if the camera connects but stays silent (never
        // sends LOGIN), announce anyway after a short wait so it isn't a deadlock
        // (the real monitor announces right after login; here we bootstrap it).
        if (!announced && now - connectAt > 1500) beginDriving("no login within 1.5s");

        // ---- drive the monitor's proactive schedule ----
        if (announced && now >= nextSendAt) {
            if (scriptPos < kScriptN) {
                sendCmd(kScript[scriptPos]);
                std::fprintf(stderr, "[emu] -> poll 0x%04x (%zu/%zu)\n",
                             kScript[scriptPos], scriptPos + 1, kScriptN);
                ++scriptPos;
                nextSendAt = now + 60;                 // ~60ms between handshake polls
            } else {
                // steady state: heartbeat ~1/s + a rolling 0x0006/0x0006/0x0005 poll
                if (now - lastHeartbeat >= 1000) {
                    auto hb = buildFrame(12, {0x00, 0x01});
                    ::send(camFd, hb.data(), hb.size(), 0);
                    lastHeartbeat = now;
                }
                sendCmd(kSteady[steadyPos % 3]);
                ++steadyPos;
                nextSendAt = now + 300;                // steady poll cadence
            }
        }

        // ---- periodic streaming health (so a long capture proves sustained flow) ----
        if (announced && now - lastReportAt >= 5000) {
            long dt = now - lastReportAt, dm = media - lastReportMedia;
            std::fprintf(stderr, "[emu] alive t=%lds media=%ld (+%ld, ~%ld/s) video=%ld audio=%ld\n",
                         (now - sessionStart) / 1000, media, dm, dm * 1000 / (dt ? dt : 1),
                         videoFrames, audioFrames);
            lastReportAt = now;
            lastReportMedia = media;
        }

        // ---- wait for readability with a RELIABLE timeout ----
        // Do NOT rely on SO_RCVTIMEO here: on this kernel it is not honored, so a
        // blocking recv() would freeze the proactive driver whenever the camera
        // goes silent (the 45s startup stall + cold-boot deadlock). poll() gives us
        // a dependable ~50ms wake so the schedule advances regardless of silence.
        struct pollfd pfd; pfd.fd = camFd; pfd.events = POLLIN; pfd.revents = 0;
        int pr = ::poll(&pfd, 1, 50);
        if (pr == 0) continue;                         // timeout -> loop to drive sends
        if (pr < 0) { if (errno == EINTR) continue; std::fprintf(stderr, "[emu] poll err errno=%d\n", errno); break; }

        ssize_t n = ::recv(camFd, buf, sizeof(buf), 0);
        if (n == 0) { std::fprintf(stderr, "[emu] session end: peer closed (media=%ld)\n", media); break; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            std::fprintf(stderr, "[emu] session end: recv error errno=%d (media=%ld)\n", errno, media);
            break;
        }
        for (auto& fr : reader.feed(buf, (size_t)n)) {
            auto mu = extractMedia(fr);
            if (mu) {
                ++media;
                int op = fr.op();
                if (op == 0x0201) ++audioFrames; else ++videoFrames;  // 0x0000 key / 0x0100 inter
                if (media <= 3 || media % 200 == 0)
                    std::fprintf(stderr, "[emu] tap MEDIA op=0x%04x len=%zu (media#%ld)\n",
                                 op, fr.body.size(), media);
                hub.publish(*mu);
                continue;
            }
            uint16_t cmd = fr.body.size() >= 2 ? (uint16_t)((fr.body[0] << 8) | fr.body[1]) : 0xffff;
            if (fr.type == 1 && !announced) {          // LOGIN -> announce, then start the script
                beginDriving("login");
            } else if (fr.type == 7 && cmd == 0x0024) { // camera asks for monitor info -> answer
                size_t rl = 0; const unsigned char* r = findReply(0x0024, rl);
                if (r) { ::send(camFd, r, rl, 0); std::fprintf(stderr, "[emu] -> 0x0024 info reply (reactive)\n"); }
            }
            // All other camera frames (its answers to our polls, keepalives) need no
            // reply -- the monitor drives on its own clock. Drained by the recv above.
        }
    }
    stopProbe.store(true);
    probe.join();
}

} // namespace babycam
