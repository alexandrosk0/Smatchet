// SmatchetCommentsModalSeedPure doctest — what the comments modal shows before (or instead of) a
// live fetch: the saved structured thread, else the tooltip blob summary (Quality Pillar 6). Pure.

#include "Ui/SmatchetCommentsModalSeedPure.h"

#include "Tracker/CommentBlobFormatPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using SmatchetCommentsModalSeed::PickCommentsSeed;

namespace {

std::vector<TrackerIssueComment> OneComment(const std::string& author) {
    TrackerIssueComment c;
    c.Id = "c1";
    c.Author = author;
    c.Body = "cached comment body";
    c.CreatedAtSec = 1700000000;
    c.UpdatedAtSec = c.CreatedAtSec;
    return std::vector<TrackerIssueComment>(1, c);
}

} // namespace

TEST_CASE("PickCommentsSeed — prefers the structured thread") {
    const std::string thread = smatchet::tracker::SerializeCommentThread(OneComment("From Thread"));
    const std::string blob = smatchet::tracker::FormatCommentBlob(OneComment("From Blob"));
    std::vector<TrackerIssueComment> out;
    bool partial = true;
    REQUIRE(PickCommentsSeed(thread, blob, out, partial));
    CHECK_FALSE(partial);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author == "From Thread");
    CHECK(out[0].Id == "c1");
}

TEST_CASE("PickCommentsSeed — falls back to the blob and marks it partial") {
    const std::string blob = smatchet::tracker::FormatCommentBlob(OneComment("From Blob"));
    std::vector<TrackerIssueComment> out;
    bool partial = false;
    REQUIRE(PickCommentsSeed(std::string(), blob, out, partial));
    CHECK(partial);
    REQUIRE(out.size() == 1);
    CHECK(out[0].Author == "From Blob");
    CHECK(out[0].Body == "cached comment body");

    // An unreadable or empty thread falls back the same way.
    REQUIRE(PickCommentsSeed("not json", blob, out, partial));
    CHECK(partial);
    REQUIRE(PickCommentsSeed("[]", blob, out, partial));
    CHECK(partial);
}

TEST_CASE("PickCommentsSeed — nothing saved yields false and an empty list") {
    std::vector<TrackerIssueComment> out = OneComment("stale");
    bool partial = true;
    CHECK_FALSE(PickCommentsSeed(std::string(), std::string(), out, partial));
    CHECK(out.empty());
    CHECK_FALSE(partial);
    CHECK_FALSE(PickCommentsSeed("not json", "unrecognised blob", out, partial));
    CHECK(out.empty());
    CHECK_FALSE(partial);
}
