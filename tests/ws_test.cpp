#include "doctest.h"
#include "websocket.h"
using namespace babycam;

TEST_CASE("RFC6455 accept-key vector") {
    CHECK(wsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("binary frame headers for small and large payloads") {
    std::vector<uint8_t> small(5, 0xAB);
    auto f = wsEncodeBinary(small.data(), small.size());
    CHECK(f[0] == 0x82);          // FIN + binary
    CHECK(f[1] == 5);             // unmasked, len in 1 byte
    CHECK(f.size() == 2 + 5);

    std::vector<uint8_t> big(200, 0x01);
    auto g = wsEncodeBinary(big.data(), big.size());
    CHECK(g[0] == 0x82);
    CHECK(g[1] == 126);           // 16-bit length form
    CHECK(((g[2] << 8) | g[3]) == 200);
    CHECK(g.size() == 4 + 200);
}
