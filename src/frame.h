#pragma once
#include <cstdint>
#include <optional>
#include <vector>

namespace babycam {

constexpr uint8_t MAGIC[4] = {0x56, 0x56, 0x50, 0x99};

struct Frame {
    int type = 0;
    bool encrypted = false;
    int flag = 0;
    std::vector<uint8_t> body;
    int op() const { return body.size() >= 2 ? (body[0] << 8) | body[1] : -1; }
};

uint32_t additiveChecksum(const uint8_t* d, size_t n);
std::vector<uint8_t> buildFrame(int type, const std::vector<uint8_t>& body, int flag = 0);

std::optional<Frame> parseFrame(const std::vector<uint8_t>& buf, size_t start, size_t& next);

class FrameReader {
public:
    std::vector<Frame> feed(const uint8_t* data, size_t n);
private:
    std::vector<uint8_t> buf_;
};

} // namespace babycam
