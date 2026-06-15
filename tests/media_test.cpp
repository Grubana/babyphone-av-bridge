#include "doctest.h"
#include "media.h"
#include "pcap.h"
#include "frame.h"
using namespace babycam;

TEST_CASE("extractMedia: video from first start code, audio from body[20:]") {
    std::vector<uint8_t> vbody = {0x00,0x00, 0xAA,0xBB, 0x00,0x00,0x00,0x01, 0x67, 0x42};
    Frame v{8,false,0,vbody};
    auto mu = extractMedia(v);
    REQUIRE(mu.has_value());
    CHECK(mu->kind == MediaUnit::Video);
    CHECK(mu->keyframe);
    REQUIRE(mu->data.size() == 6);
    CHECK(mu->data[0] == 0x00); CHECK(mu->data[4] == 0x67);

    std::vector<uint8_t> abody(20 + 4, 0);
    abody[0] = 0x02; abody[1] = 0x01;
    abody[20] = 0x7f; abody[23] = 0x80;
    Frame a{8,false,0,abody};
    auto au = extractMedia(a);
    REQUIRE(au.has_value());
    CHECK(au->kind == MediaUnit::Audio);
    REQUIRE(au->data.size() == 4);
    CHECK(au->data[0] == 0x7f);
}

TEST_CASE("pcap regression: monitor capture demuxes to 15/553/1418") {
    auto stream = pcapReassemble("test-fixtures/babyphone_monitor.pcap", /*toMonitor=*/true);
    REQUIRE(stream.size() > 100000);
    FrameReader r;
    auto frames = r.feed(stream.data(), stream.size());
    int key = 0, inter = 0, audio = 0;
    for (auto& f : frames) {
        auto mu = extractMedia(f);
        if (!mu) continue;
        if (mu->kind == MediaUnit::Audio) audio++;
        else if (mu->keyframe) key++;
        else inter++;
    }
    CHECK(key == 15);
    CHECK(inter == 553);
    CHECK(audio == 1418);
}
