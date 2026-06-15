#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace babycam {

std::string wsAcceptKey(const std::string& clientKey);
std::string wsHandshakeResponse(const std::string& clientKey);
std::vector<uint8_t> wsEncodeBinary(const uint8_t* data, size_t len);

} // namespace babycam
