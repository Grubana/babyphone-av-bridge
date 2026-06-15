#include "pcap.h"
#include <cstring>
#include <map>
#include <fstream>

namespace babycam {

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

std::vector<uint8_t> pcapReassemble(const std::string& path, bool toMonitor, uint16_t port) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> out;
    if (d.size() < 24 || !(d[0]==0xd4 && d[1]==0xc3 && d[2]==0xb2 && d[3]==0xa1)) return out;
    std::map<uint32_t, std::vector<uint8_t>> segs;   // seq -> payload
    size_t off = 24;
    while (off + 16 <= d.size()) {
        uint32_t incl = le32(&d[off + 8]);
        off += 16;
        if (off + incl > d.size()) break;
        const uint8_t* raw = &d[off]; size_t rlen = incl; off += incl;
        if (rlen < 14 || raw[12] != 0x08 || raw[13] != 0x00) continue;     // IPv4
        const uint8_t* ip = raw + 14; size_t iplen = rlen - 14;
        if (iplen < 20) continue;
        size_t ihl = (ip[0] & 0x0F) * 4;
        if (ip[9] != 6 || iplen < ihl + 20) continue;                       // TCP
        const uint8_t* tcp = ip + ihl;
        uint16_t sp = be16(tcp), dp = be16(tcp + 2);
        uint32_t seq = be32(tcp + 4);
        size_t doff = ((tcp[12] >> 4) & 0x0F) * 4;
        if (ihl + doff > iplen) continue;
        const uint8_t* pl = tcp + doff; size_t pllen = iplen - ihl - doff;
        bool match = toMonitor ? (dp == port) : (sp == port);
        if (match && pllen) segs.emplace(seq, std::vector<uint8_t>(pl, pl + pllen));
    }
    uint32_t nxt = 0; bool first = true;
    for (auto& kv : segs) {
        if (first || kv.first >= nxt) { out.insert(out.end(), kv.second.begin(), kv.second.end()); nxt = kv.first + (uint32_t)kv.second.size(); first = false; }
    }
    return out;
}

} // namespace babycam
