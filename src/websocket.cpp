#include "websocket.h"
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>

namespace babycam {

std::string wsAcceptKey(const std::string& clientKey) {
    std::string s = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[20];
    mbedtls_sha1((const unsigned char*)s.data(), s.size(), sha);
    unsigned char b64[64]; size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, sha, 20);
    return std::string((char*)b64, olen);
}

std::string wsHandshakeResponse(const std::string& clientKey) {
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + wsAcceptKey(clientKey) + "\r\n\r\n";
}

std::vector<uint8_t> wsEncodeBinary(const uint8_t* data, size_t len) {
    std::vector<uint8_t> f;
    f.push_back(0x82);
    if (len < 126) {
        f.push_back((uint8_t)len);
    } else if (len < 65536) {
        f.push_back(126);
        f.push_back((uint8_t)((len >> 8) & 0xFF));
        f.push_back((uint8_t)(len & 0xFF));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back((uint8_t)((len >> (8 * i)) & 0xFF));
    }
    f.insert(f.end(), data, data + len);
    return f;
}

} // namespace babycam
