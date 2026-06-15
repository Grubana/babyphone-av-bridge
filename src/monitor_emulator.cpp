#include "monitor_emulator.h"
#include "monitor_replies.h"
#include "frame.h"
#include "media.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>

namespace babycam {

static const unsigned char* findReply(uint16_t cmd, size_t& len) {
    for (size_t i = 0; i < monrep::kReplyCount; ++i)
        if (monrep::kReplies[i].cmd == cmd) { len = monrep::kReplies[i].len; return monrep::kReplies[i].data; }
    return nullptr;
}

// --- diagnostic helpers (temporary instrumentation) ---
static void dumpBody(const char* tag, const std::vector<uint8_t>& b) {
    char hex[3 * 32 + 1]; size_t n = b.size() < 32 ? b.size() : 32, k = 0;
    for (size_t i = 0; i < n; ++i) k += (size_t)std::snprintf(hex + k, sizeof(hex) - k, "%02x ", b[i]);
    char asc[33]; size_t m = b.size() < 32 ? b.size() : 32;
    for (size_t i = 0; i < m; ++i) asc[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
    asc[m] = 0;
    std::fprintf(stderr, "[emu]   %s bodylen=%zu hex=%s| %s\n", tag, b.size(), hex, asc);
}

void runMonitorEmulator(int camFd, StreamHub& hub) {
    FrameReader reader;
    uint8_t buf[8192];
    std::fprintf(stderr, "[emu] session start\n");
    int nframes = 0, idle = 0;
    while (true) {
        ssize_t n = ::recv(camFd, buf, sizeof(buf), 0);
        if (n == 0) { std::fprintf(stderr, "[emu] session end: peer closed after %d frames\n", nframes); break; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::fprintf(stderr, "[emu] idle tick %d (connection open, no camera data yet)\n", ++idle);
                continue;   // read timeout is NOT a disconnect — keep waiting
            }
            std::fprintf(stderr, "[emu] session end: recv error errno=%d after %d frames\n", errno, nframes);
            break;
        }
        idle = 0;
        for (auto& fr : reader.feed(buf, (size_t)n)) {
            ++nframes;
            auto mu = extractMedia(fr);
            if (mu) { std::fprintf(stderr, "[emu] recv MEDIA op=0x%04x len=%zu -> tap\n", fr.op(), fr.body.size()); hub.publish(*mu); continue; }
            uint16_t cmd = fr.body.size() >= 2 ? (uint16_t)((fr.body[0] << 8) | fr.body[1]) : 0xffff;
            std::fprintf(stderr, "[emu] recv type=%d cmd=0x%04x flag=%d\n", fr.type, cmd, fr.flag);
            if (fr.type == 1 || fr.type == 7) dumpBody("body", fr.body);
            if (fr.type == 1) {                                   // login -> announce
                ssize_t s = ::send(camFd, monrep::announce, monrep::announce_len, 0);
                std::fprintf(stderr, "[emu]   -> announce (%zu bytes, sent=%zd)\n", monrep::announce_len, s);
            } else if (fr.type == 7 && fr.body.size() >= 2) {     // control -> captured reply or echo
                size_t rl = 0; const unsigned char* r = findReply(cmd, rl);
                if (r) { ssize_t s = ::send(camFd, r, rl, 0); std::fprintf(stderr, "[emu]   -> captured reply cmd=0x%04x (%zu bytes, sent=%zd)\n", cmd, rl, s); }
                else { auto echo = buildFrame(7, {fr.body[0], fr.body[1]}); ssize_t s = ::send(camFd, echo.data(), echo.size(), 0); std::fprintf(stderr, "[emu]   -> echo cmd=0x%04x (%zu bytes, sent=%zd) [NO captured reply]\n", cmd, echo.size(), s); }
            } else if (fr.type == 12) {                           // heartbeat -> keepalive
                auto ka = buildFrame(13, {0x00, 0x00});
                ::send(camFd, ka.data(), ka.size(), 0);
                std::fprintf(stderr, "[emu]   -> keepalive(13)\n");
            } else {
                std::fprintf(stderr, "[emu]   (no action for type=%d)\n", fr.type);
            }
        }
    }
}

} // namespace babycam
