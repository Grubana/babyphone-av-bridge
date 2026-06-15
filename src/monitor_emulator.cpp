#include "monitor_emulator.h"
#include "monitor_replies.h"
#include "frame.h"
#include "media.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>

namespace babycam {

static const unsigned char* findReply(uint16_t cmd, size_t& len) {
    for (size_t i = 0; i < monrep::kReplyCount; ++i)
        if (monrep::kReplies[i].cmd == cmd) { len = monrep::kReplies[i].len; return monrep::kReplies[i].data; }
    return nullptr;
}

void runMonitorEmulator(int camFd, StreamHub& hub) {
    FrameReader reader;
    uint8_t buf[8192];
    while (true) {
        ssize_t n = ::recv(camFd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (auto& fr : reader.feed(buf, (size_t)n)) {
            auto mu = extractMedia(fr);
            if (mu) { hub.publish(*mu); continue; }
            if (fr.type == 1) {                                   // login -> announce
                ::send(camFd, monrep::announce, monrep::announce_len, 0);
            } else if (fr.type == 7 && fr.body.size() >= 2) {     // control -> captured reply or echo
                uint16_t cmd = (fr.body[0] << 8) | fr.body[1];
                size_t rl = 0; const unsigned char* r = findReply(cmd, rl);
                if (r) ::send(camFd, r, rl, 0);
                else { auto echo = buildFrame(7, {fr.body[0], fr.body[1]}); ::send(camFd, echo.data(), echo.size(), 0); }
            } else if (fr.type == 12) {                           // heartbeat -> keepalive
                auto ka = buildFrame(13, {0x00, 0x00});
                ::send(camFd, ka.data(), ka.size(), 0);
            }
        }
    }
}

} // namespace babycam
