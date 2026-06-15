#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "frame.h"
using namespace babycam;

TEST_CASE("build then parse round-trips type/flag/body") {
    std::vector<uint8_t> body = {0x00, 0x00, 0xde, 0xad};
    auto wire = buildFrame(8, body, 0x00);
    size_t next = 0;
    auto fr = parseFrame(wire, 0, next);
    REQUIRE(fr.has_value());
    CHECK(fr->type == 8);
    CHECK_FALSE(fr->encrypted);
    CHECK(fr->body == body);
    CHECK(fr->op() == 0x0000);
    CHECK(next == wire.size());
}

TEST_CASE("FrameReader handles two concatenated frames and a partial tail") {
    auto a = buildFrame(7, {0x00, 0x05});
    auto b = buildFrame(12, {0x00, 0x01});
    std::vector<uint8_t> all = a;
    all.insert(all.end(), b.begin(), b.end());
    all.push_back(0x56); all.push_back(0x56);  // partial magic of a 3rd frame
    FrameReader r;
    auto frames = r.feed(all.data(), all.size());
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].type == 7);
    CHECK(frames[1].type == 12);
    auto c = buildFrame(13, {0x00, 0x00});
    std::vector<uint8_t> rest(c.begin() + 2, c.end());  // we already fed 2 magic bytes
    auto more = r.feed(rest.data(), rest.size());
    REQUIRE(more.size() == 1);
    CHECK(more[0].type == 13);
}

TEST_CASE("bad checksum resyncs to the next frame") {
    auto a = buildFrame(7, {0x00, 0x05});
    a[a.size() - 1] ^= 0xFF;  // corrupt checksum
    auto b = buildFrame(8, {0x00, 0x00, 0x01});
    std::vector<uint8_t> all = a;
    all.insert(all.end(), b.begin(), b.end());
    FrameReader r;
    auto frames = r.feed(all.data(), all.size());
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].type == 8);
}

TEST_CASE("parses a frame whose checksum has the high bit set (no shift UB)") {
    // body chosen so the additive checksum's top byte is >= 0x80
    std::vector<uint8_t> body(64, 0xFF);   // large additive sum -> high bytes set
    auto wire = buildFrame(8, body);
    size_t next = 0;
    auto fr = parseFrame(wire, 0, next);
    REQUIRE(fr.has_value());
    CHECK(fr->body == body);
    CHECK(next == wire.size());
}
