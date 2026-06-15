#pragma once
#include <atomic>
#include <cstdint>
#include "stream_hub.h"

namespace babycam {
class Server {
public:
    Server(StreamHub& hub, uint16_t port) : hub_(hub), port_(port) {}
    void run(std::atomic<bool>& stop);
private:
    void handle(int fd);
    StreamHub& hub_;
    uint16_t port_;
};
}
