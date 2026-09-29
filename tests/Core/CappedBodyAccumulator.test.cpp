// CappedBodyAccumulator — the streamed-body cap shared by the attachment download (TrackerDownloadLogged)
// and the app-update check: chunks up to the cap are kept, and the chunk that would cross it is refused
// whole so no truncated body ever reaches a parser or the disk.

#include "CappedBodyAccumulator.h"

#include <doctest/doctest.h>

#include <string>

TEST_SUITE("CappedBodyAccumulator") {

    TEST_CASE("accepts chunks up to the cap and refuses the one that would cross it") {
        CappedBodyAccumulator body(16);
        CHECK(body.Append("12345678"));
        CHECK(body.Append("abcdefgh")); // exactly the cap is allowed
        CHECK(body.Body() == "12345678abcdefgh");
        CHECK_FALSE(body.Exceeded());

        CHECK_FALSE(body.Append("x"));
        CHECK(body.Exceeded());
        CHECK(body.Body() == "12345678abcdefgh"); // untouched: never a partial append
    }

    TEST_CASE("an oversized first chunk leaves the body empty") {
        CappedBodyAccumulator body(4);
        CHECK_FALSE(body.Append("overflowing-chunk"));
        CHECK(body.Exceeded());
        CHECK(body.Body().empty());
    }

    TEST_CASE("empty chunks never trip the cap, even at the limit") {
        CappedBodyAccumulator body(3);
        CHECK(body.Append("abc"));
        CHECK(body.Append(""));
        CHECK_FALSE(body.Exceeded());
    }

    TEST_CASE("a zero cap accepts nothing but empty chunks") {
        CappedBodyAccumulator body(0);
        CHECK(body.Append(""));
        CHECK_FALSE(body.Append("a"));
        CHECK(body.Exceeded());
    }

    TEST_CASE("TakeBody hands the bytes over") {
        CappedBodyAccumulator body(64, 1024);
        REQUIRE(body.Append(std::string("\0\x01binary", 8)));
        const std::string taken = body.TakeBody();
        CHECK(taken.size() == 8u);
        CHECK(taken[0] == '\0');
    }
}
