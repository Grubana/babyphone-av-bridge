#include "doctest.h"
#include "stream_hub.h"
using namespace babycam;

static MediaUnit vid(bool key, uint8_t marker) {
    MediaUnit m; m.kind = MediaUnit::Video; m.keyframe = key; m.data = {marker}; return m;
}

TEST_CASE("subscriber starts at the first keyframe, tagging video as 0") {
    StreamHub hub;
    auto s = hub.subscribe();
    hub.publish(vid(false, 1));        // inter before any keyframe -> dropped for this sub
    hub.publish(vid(true, 2));         // keyframe -> starts here
    hub.publish(vid(false, 3));        // inter after start -> delivered
    auto p1 = s->next(100); REQUIRE(bool(p1));
    CHECK((*p1)[0] == 0);              // video tag
    CHECK((*p1)[1] == 2);              // the keyframe marker, not the pre-keyframe inter
    auto p2 = s->next(100); REQUIRE(bool(p2));
    CHECK((*p2)[1] == 3);
    CHECK_FALSE(bool(s->next(50)));    // nothing more
}

TEST_CASE("audio is tagged 1 and only after start") {
    StreamHub hub;
    auto s = hub.subscribe();
    MediaUnit a; a.kind = MediaUnit::Audio; a.data = {9};
    hub.publish(a);                    // before keyframe -> dropped
    hub.publish(vid(true, 5));         // start
    hub.publish(a);                    // delivered
    auto p1 = s->next(100); REQUIRE(bool(p1)); CHECK((*p1)[0] == 0);   // keyframe video
    auto p2 = s->next(100); REQUIRE(bool(p2)); CHECK((*p2)[0] == 1);   // audio
}
