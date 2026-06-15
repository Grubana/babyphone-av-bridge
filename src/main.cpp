#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include "stream_hub.h"
#include "relay.h"
#include "server.h"

using namespace babycam;
static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop.store(true); }

template <typename F>
static void supervise(const char* name, F fn) {
    while (!g_stop.load()) {
        try { fn(g_stop); return; }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[%s] crashed: %s - restart in 3s\n", name, e.what());
            for (int i = 0; i < 30 && !g_stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

int main(int argc, char** argv) {
    std::string monIp = "10.10.10.1";
    uint16_t monPort = 11224, listenPort = 11224, webPort = 8080;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* d){ return std::string(i + 1 < argc ? argv[++i] : d); };
        if (a == "--monitor-ip") monIp = val("10.10.10.1");
        else if (a == "--monitor-port") monPort = (uint16_t)std::stoi(val("11224"));
        else if (a == "--listen-port") listenPort = (uint16_t)std::stoi(val("11224"));
        else if (a == "--web-port") webPort = (uint16_t)std::stoi(val("8080"));
    }
    std::printf("av-bridge: listen :%u  monitor %s:%u  web :%u\n", listenPort, monIp.c_str(), monPort, webPort);
    std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal); std::signal(SIGPIPE, SIG_IGN);

    StreamHub hub;
    Relay relay(hub, listenPort, monIp, monPort);
    Server server(hub, webPort);

    std::thread t1([&]{ supervise("relay", [&](std::atomic<bool>& s){ relay.run(s); }); });
    std::thread t2([&]{ supervise("web", [&](std::atomic<bool>& s){ server.run(s); }); });
    t1.join(); t2.join();
    std::printf("av-bridge stopped\n");
    return 0;
}
