#include "server.h"
#include "http.h"
#include "websocket.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <thread>
#include <vector>
#include <string>
#include <stdexcept>

namespace babycam {

static bool sendAll(int fd, const uint8_t* d, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::send(fd, d + off, n - off, 0);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

void Server::handle(int fd) {
    std::string raw; char buf[2048]; HttpRequest req; bool have = false;
    for (int i = 0; i < 64; ++i) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { ::close(fd); return; }
        raw.append(buf, n);
        if (raw.find("\r\n\r\n") != std::string::npos) { have = parseHttpRequest(raw, req); break; }
    }
    if (!have) { ::close(fd); return; }

    bool isWs = req.path == "/ws" &&
                req.header("upgrade").find("websocket") != std::string::npos &&
                !req.header("sec-websocket-key").empty();
    if (!isWs) {
        auto resp = buildAssetResponse(req.path);
        sendAll(fd, (const uint8_t*)resp.data(), resp.size());
        ::close(fd);
        return;
    }

    auto hs = wsHandshakeResponse(req.header("sec-websocket-key"));
    if (!sendAll(fd, (const uint8_t*)hs.data(), hs.size())) { ::close(fd); return; }

    auto sub = hub_.subscribe();
    std::fprintf(stderr, "[ws] client up\n");
    while (true) {
        Packet p = sub->next(1000);
        if (!p) continue;                         // keepalive tick; loop
        auto frame = wsEncodeBinary(p->data(), p->size());
        if (!sendAll(fd, frame.data(), frame.size())) break;   // client gone
    }
    hub_.unsubscribe(sub);
    ::close(fd);
    std::fprintf(stderr, "[ws] client down\n");
}

void Server::run(std::atomic<bool>& stop) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("http socket()");
    int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(port_);
    if (::bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { ::close(fd); throw std::runtime_error("http bind()"); }
    if (::listen(fd, 8) < 0) { ::close(fd); throw std::runtime_error("http listen()"); }
    timeval tv{1, 0}; ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (!stop.load()) {
        int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) continue;
        std::thread([this, c]{ handle(c); }).detach();
    }
    ::close(fd);
}

} // namespace babycam
