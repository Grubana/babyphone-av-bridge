#include "doctest.h"
#include "monitor_replies.h"

TEST_CASE("captured monitor announce and replies were extracted") {
    CHECK(monrep::announce_len > 0);
    CHECK(monrep::kReplyCount >= 1);
    for (size_t i = 0; i < monrep::kReplyCount; ++i) {
        const unsigned char* d = monrep::kReplies[i].data;
        CHECK(d[0] == 0x56); CHECK(d[1] == 0x56); CHECK(d[2] == 0x50); CHECK(d[3] == 0x99);
    }
}
