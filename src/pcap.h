#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace babycam {
// Reassemble one TCP direction of a classic little-endian pcap into a byte stream.
// toMonitor=true selects packets with dst port 11224 (camera->monitor);
// false selects src port 11224 (monitor->camera).
std::vector<uint8_t> pcapReassemble(const std::string& path, bool toMonitor, uint16_t port = 11224);
}
