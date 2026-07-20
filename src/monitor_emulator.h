#pragma once
#include <string>
#include <cstdint>
#include "stream_hub.h"
namespace babycam {
// Emulate the monitor for `camFd`, tapping media into `hub`. While emulating, it
// periodically probes monitorIp:monitorPort; if the real monitor comes back, it
// returns so the caller can reconnect the camera to it (hand back to Mode A tee).
void runMonitorEmulator(int camFd, StreamHub& hub, const std::string& monitorIp, uint16_t monitorPort);
}
