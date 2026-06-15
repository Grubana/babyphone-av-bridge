#include "frame.h"
#include <cstring>

namespace babycam {

uint32_t additiveChecksum(const uint8_t* d, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; ++i) s += d[i];
    return s;
}

std::vector<uint8_t> buildFrame(int type, const std::vector<uint8_t>& body, int flag) {
    uint8_t t0 = ((type & 0x0F) << 4);             // encrypt bit 0 (we never emit encrypted)
    // Wire truth (validated against the pcap fixtures): total = length_field + 0x0C,
    // and total = header(8) + body + checksum(4), so length_field == body_len.
    uint32_t lengthField = (uint32_t)body.size();
    std::vector<uint8_t> out;
    out.insert(out.end(), MAGIC, MAGIC + 4);
    out.push_back(t0);
    out.push_back((uint8_t)(flag & 0xFF));
    out.push_back((uint8_t)((lengthField >> 8) & 0xFF));
    out.push_back((uint8_t)(lengthField & 0xFF));
    out.insert(out.end(), body.begin(), body.end());
    uint32_t ck = additiveChecksum(out.data(), out.size());
    out.push_back((uint8_t)((ck >> 24) & 0xFF));
    out.push_back((uint8_t)((ck >> 16) & 0xFF));
    out.push_back((uint8_t)((ck >> 8) & 0xFF));
    out.push_back((uint8_t)(ck & 0xFF));
    return out;
}

static int findMagic(const std::vector<uint8_t>& buf, size_t start) {
    if (buf.size() < 4) return -1;
    for (size_t i = start; i + 4 <= buf.size(); ++i)
        if (std::memcmp(buf.data() + i, MAGIC, 4) == 0) return (int)i;
    return -1;
}

std::optional<Frame> parseFrame(const std::vector<uint8_t>& buf, size_t start, size_t& next) {
    int s = findMagic(buf, start);
    if (s < 0) { next = buf.size() >= 3 ? buf.size() - 3 : start; return std::nullopt; }
    if (buf.size() - s < 8) { next = (size_t)s; return std::nullopt; }
    uint8_t t0 = buf[s + 4];
    int flag = buf[s + 5];
    uint32_t lengthField = (buf[s + 6] << 8) | buf[s + 7];
    size_t total = lengthField + 0x0C;
    if (buf.size() - s < total) { next = (size_t)s; return std::nullopt; }
    uint32_t stored = ((uint32_t)buf[s + total - 4] << 24) | ((uint32_t)buf[s + total - 3] << 16) |
                      ((uint32_t)buf[s + total - 2] << 8) | (uint32_t)buf[s + total - 1];
    if (additiveChecksum(buf.data() + s, total - 4) != stored) { next = (size_t)s + 1; return std::nullopt; }
    Frame fr;
    fr.type = t0 >> 4;
    fr.encrypted = (t0 & 1) != 0;
    fr.flag = flag;
    fr.body.assign(buf.begin() + s + 8, buf.begin() + s + total - 4);
    next = (size_t)s + total;
    return fr;
}

std::vector<Frame> FrameReader::feed(const uint8_t* data, size_t n) {
    buf_.insert(buf_.end(), data, data + n);
    std::vector<Frame> out;
    size_t pos = 0;
    while (true) {
        size_t next = pos;
        auto fr = parseFrame(buf_, pos, next);
        if (!fr) {
            // No frame: either resync (checksum fail, next advanced) or wait for
            // more bytes (next did not advance). Only stop when we cannot progress.
            if (next > pos) { pos = next; continue; }
            pos = next;
            break;
        }
        out.push_back(std::move(*fr));
        pos = next;
    }
    buf_.erase(buf_.begin(), buf_.begin() + pos);
    return out;
}

} // namespace babycam
