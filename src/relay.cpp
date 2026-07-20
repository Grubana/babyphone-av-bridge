#include "relay.h"
#include "frame.h"
#include "media.h"
#include "monitor_emulator.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <atomic>
#include <thread>
#include <stdexcept>

namespace babycam {

// ---- protocol logging (for reverse-engineering the monitor handshake) --------
// Logs every frame in BOTH directions with a session-relative timestamp so the
// monitor's proactive frames + request/response pairing can be read off the log.
// Media (type 8) is rate-limited so it doesn't drown the control traffic.

static std::atomic<int> g_sessionSeq{0};

static long nowMs() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long>(ts.tv_sec) * 1000L + ts.tv_nsec / 1000000L;
}

static const char* typeName(int t) {
    switch (t) {
        case 1:  return "LOGIN";
        case 2:  return "ANNOUNCE";
        case 7:  return "CONTROL";
        case 8:  return "MEDIA";
        case 12: return "HEARTBEAT";
        case 13: return "KEEPALIVE";
        default: return "?";
    }
}

struct DirCounters { long media = 0; };

// dir is "C->M" (camera->monitor) or "M->C" (monitor->camera).
static void logFrame(int sess, const char* dir, const Frame& fr, long t0, DirCounters& dc) {
    long rel = nowMs() - t0;
    int cmd = fr.op();
    if (fr.type == 8) {
        // Media: log the first few + every 200th so we see it flowing without spam.
        if (++dc.media <= 3 || dc.media % 200 == 0)
            std::fprintf(stderr, "[proto s%d %+7ldms] %s MEDIA op=0x%04x flag=%d len=%zu (media#%ld)\n",
                         sess, rel, dir, cmd, fr.flag, fr.body.size(), dc.media);
        return;
    }
    // Control/login/announce/heartbeat/keepalive: full body hex (this is what
    // Mode B must replay). Cap at 128 bytes.
    char hex[3 * 128 + 1]; size_t m = fr.body.size() < 128 ? fr.body.size() : 128, k = 0;
    for (size_t i = 0; i < m; ++i) k += (size_t)std::snprintf(hex + k, sizeof(hex) - k, "%02x ", fr.body[i]);
    std::fprintf(stderr, "[proto s%d %+7ldms] %s type=%d(%s) cmd=0x%04x flag=%d len=%zu body=%s%s\n",
                 sess, rel, dir, fr.type, typeName(fr.type), cmd, fr.flag, fr.body.size(),
                 hex, fr.body.size() > 128 ? "..." : "");
}

// Read frames from `from`, log them, forward raw bytes to `to`, and (when hub is
// set — the camera->monitor direction) tap media into the stream hub.
static void relayDirection(int from, int to, const char* dir, int sess, long t0, StreamHub* hub) {
    FrameReader reader;
    uint8_t buf[8192];
    DirCounters dc;
    while (true) {
        ssize_t n = ::recv(from, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (to >= 0 && ::send(to, buf, n, 0) != n) break;
        for (auto& fr : reader.feed(buf, (size_t)n)) {
            logFrame(sess, dir, fr, t0, dc);
            if (hub) { auto mu = extractMedia(fr); if (mu) hub->publish(*mu); }
        }
    }
}

int dialMonitor(const std::string& ip, uint16_t port, int timeoutMs) {
    // The bridge is statically linked, so the LD_PRELOAD hook does NOT apply to
    // this process — this connect reaches the real monitor unmodified (no loop).
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, (sockaddr*)&a, sizeof(a));
    if (rc == 0) { fcntl(fd, F_SETFL, fl); return fd; }
    if (errno != EINPROGRESS) { ::close(fd); return -1; }
    fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (::select(fd + 1, nullptr, &w, nullptr, &tv) <= 0) { ::close(fd); return -1; }
    int err = 0; socklen_t el = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err != 0) { ::close(fd); return -1; }
    fcntl(fd, F_SETFL, fl);
    return fd;
}

void Relay::session(int camFd) {
    int sess = ++g_sessionSeq;
    long t0 = nowMs();
    int monFd = dialMonitor(monitorIp_, monitorPort_, 500);
    if (monFd >= 0) {
        std::fprintf(stderr, "[proto s%d %+7ldms] ==== SESSION START mode=A (tee, monitor UP) ====\n", sess, 0L);
        // monitor -> camera (control/handshake; no media tap)
        std::thread back([monFd, camFd, sess, t0]{ relayDirection(monFd, camFd, "M->C", sess, t0, nullptr); });
        // camera -> monitor (+ tap media into the hub)
        relayDirection(camFd, monFd, "C->M", sess, t0, &hub_);
        ::shutdown(monFd, SHUT_RDWR); ::shutdown(camFd, SHUT_RDWR);
        back.join();
        ::close(monFd);
        std::fprintf(stderr, "[proto s%d %+7ldms] ==== SESSION END mode=A ====\n", sess, nowMs() - t0);
    } else {
        std::fprintf(stderr, "[proto s%d %+7ldms] ==== SESSION START mode=B (emulate, monitor DOWN) ====\n", sess, 0L);
        runMonitorEmulator(camFd, hub_, monitorIp_, monitorPort_);  // emulate + tap + reclaim
        std::fprintf(stderr, "[proto s%d %+7ldms] ==== SESSION END mode=B ====\n", sess, nowMs() - t0);
    }
    ::close(camFd);
}

void Relay::run(std::atomic<bool>& stop) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("relay socket()");
    int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(listenPort_);
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { ::close(fd); throw std::runtime_error("relay bind()"); }
    if (::listen(fd, 4) < 0) { ::close(fd); throw std::runtime_error("relay listen()"); }
    timeval tv{1, 0}; ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (!stop.load()) {
        int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        std::thread([this, c]{ session(c); }).detach();
    }
    ::close(fd);
}

} // namespace babycam
