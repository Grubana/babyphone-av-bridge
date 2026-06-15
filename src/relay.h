#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "stream_hub.h"

namespace babycam {

class Relay {
public:
    Relay(StreamHub& hub, uint16_t listenPort, std::string monitorIp, uint16_t monitorPort)
        : hub_(hub), listenPort_(listenPort), monitorIp_(std::move(monitorIp)), monitorPort_(monitorPort) {}
    void run(std::atomic<bool>& stop);
private:
    void session(int camFd);
    StreamHub& hub_;
    uint16_t listenPort_;
    std::string monitorIp_;
    uint16_t monitorPort_;
};

// Dial the monitor with a connect timeout. Returns the fd or -1. The bridge is
// static, so the LD_PRELOAD hook never rewrites this connect. (Reused by tests.)
int dialMonitor(const std::string& ip, uint16_t port, int timeoutMs);

} // namespace babycam
