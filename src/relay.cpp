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
#include <thread>
#include <stdexcept>

namespace babycam {

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

static void teeCamToMon(int camFd, int monFd, StreamHub& hub) {
    FrameReader reader;
    uint8_t buf[8192];
    while (true) {
        ssize_t n = ::recv(camFd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (monFd >= 0 && ::send(monFd, buf, n, 0) != n) break;
        for (auto& fr : reader.feed(buf, (size_t)n)) {
            auto mu = extractMedia(fr);
            if (mu) hub.publish(*mu);
        }
    }
}

static void pump(int from, int to) {
    uint8_t buf[8192];
    while (true) {
        ssize_t n = ::recv(from, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (::send(to, buf, n, 0) != n) break;
    }
}

void Relay::session(int camFd) {
    int monFd = dialMonitor(monitorIp_, monitorPort_, 500);
    if (monFd >= 0) {
        std::fprintf(stderr, "[relay] Mode A (tee): monitor up\n");
        std::thread back([monFd, camFd]{ pump(monFd, camFd); });  // monitor -> camera
        teeCamToMon(camFd, monFd, hub_);                          // camera -> monitor (+tap)
        ::shutdown(monFd, SHUT_RDWR); ::shutdown(camFd, SHUT_RDWR);
        back.join();
        ::close(monFd);
    } else {
        std::fprintf(stderr, "[relay] Mode B (emulate): monitor down\n");
        runMonitorEmulator(camFd, hub_);                          // emulate + tap
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
