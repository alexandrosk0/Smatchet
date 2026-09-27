// PendingActionPolicyPure doctest — the exactly-once decisions of the pending-action queue (Quality
// Pillar 6): the state a failed send leaves an action in, whether an interrupted comment already
// reached the tracker, and the comment payload round trip. Pure — no I/O.

#include "PendingActionPolicyPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::pendingaction::BuildCommentActionPayload;
using smatchet::pendingaction::BuildWorklogActionPayload;
using smatchet::pendingaction::CommentAlreadyPosted;
using smatchet::pendingaction::kCommentDedupeWindowSec;
using smatchet::pendingaction::NormalizeCommentForDedupe;
using smatchet::pendingaction::ParseCommentActionPayload;
using smatchet::pendingaction::ParseWorklogActionPayload;
using smatchet::pendingaction::StateAfterFailedSend;
using smatchet::pendingaction::WorklogActionPayload;

namespace {

const std::int64_t kQueuedAt = 1700000000;

TrackerIssueComment Comment(const std::string& body, std::int64_t createdAtSec) {
    TrackerIssueComment c;
    c.Id = "c";
    c.Author = "Ana";
    c.Body = body;
    c.CreatedAtSec = createdAtSec;
    c.UpdatedAtSec = createdAtSec;
    return c;
}

} // namespace

TEST_CASE("StateAfterFailedSend — a rejection is final, a maybe-landed send is ambiguous") {
    const std::string pending = PendingActionState::kPending;
    const std::string ambiguous = PendingActionState::kAmbiguous;
    CHECK(std::string(StateAfterFailedSend(PendingActionKind::CommentAdd, TrackerErrorInvalidRequest("bad", 400))) ==
          "");
    CHECK(std::string(StateAfterFailedSend(PendingActionKind::CommentAdd, TrackerErrorAuth("no", 401))) == "");
    CHECK(std::string(StateAfterFailedSend(PendingActionKind::CommentAdd, TrackerErrorNotFound("gone"))) == "");
    CHECK(StateAfterFailedSend(PendingActionKind::CommentAdd, TrackerErrorTransport("timeout")) == ambiguous);
    CHECK(StateAfterFailedSend(PendingActionKind::CommentAdd, TrackerErrorServer("502", 502)) == ambiguous);
    CHECK(StateAfterFailedSend(PendingActionKind::CommentAdd, TrackerErrorRateLimited("slow down")) == pending);
    CHECK(StateAfterFailedSend(PendingActionKind::WorklogAdd, TrackerErrorTransport("timeout")) == ambiguous);
    // Watching twice is harmless, so a watch is simply retried.
    CHECK(StateAfterFailedSend(PendingActionKind::WatchAdd, TrackerErrorTransport("timeout")) == pending);
    CHECK(StateAfterFailedSend(PendingActionKind::WatchAdd, TrackerErrorServer("503", 503)) == pending);
}

TEST_CASE("NormalizeCommentForDedupe — keeps letters, digits and non-ASCII; drops formatting") {
    CHECK(NormalizeCommentForDedupe("**Fixed** in `v2.1`,\r\nsee [PR](http://x)!") == "fixedinv21seeprhttpx");
    CHECK(NormalizeCommentForDedupe("Caf\xc3\xa9 OK") == "caf\xc3\xa9ok");
    CHECK(NormalizeCommentForDedupe("  --- \n").empty());
}

TEST_CASE("CommentAlreadyPosted — matches the landed comment despite a formatting round trip") {
    const std::string queued = "Fixed in *v2* - see notes\nthanks";
    std::vector<TrackerIssueComment> fetched;
    fetched.push_back(Comment("an older, unrelated comment", kQueuedAt - 3600));
    fetched.push_back(Comment("Fixed in _v2_ \\- see notes  \nthanks", kQueuedAt + 3));
    CHECK(CommentAlreadyPosted(fetched, queued, kQueuedAt));
}

TEST_CASE("CommentAlreadyPosted — an identical comment from before the window does not count") {
    std::vector<TrackerIssueComment> fetched;
    fetched.push_back(Comment("+1", kQueuedAt - kCommentDedupeWindowSec - 1));
    CHECK_FALSE(CommentAlreadyPosted(fetched, "+1", kQueuedAt));
    fetched.push_back(Comment("+1", kQueuedAt - kCommentDedupeWindowSec));
    CHECK(CommentAlreadyPosted(fetched, "+1", kQueuedAt));
}

TEST_CASE("CommentAlreadyPosted — different text, or nothing fetched, is not a match") {
    std::vector<TrackerIssueComment> fetched;
    CHECK_FALSE(CommentAlreadyPosted(fetched, "hello", kQueuedAt));
    fetched.push_back(Comment("hello there", kQueuedAt));
    CHECK_FALSE(CommentAlreadyPosted(fetched, "hello", kQueuedAt));
    CHECK_FALSE(CommentAlreadyPosted(fetched, "", kQueuedAt));
}

TEST_CASE("CommentAlreadyPosted — a body with no letters or digits compares its trimmed text") {
    std::vector<TrackerIssueComment> fetched;
    fetched.push_back(Comment(":(", kQueuedAt));
    CHECK_FALSE(CommentAlreadyPosted(fetched, ":)", kQueuedAt));
    fetched.push_back(Comment(" :)\r\n", kQueuedAt));
    CHECK(CommentAlreadyPosted(fetched, ":)", kQueuedAt));
}

TEST_CASE("Comment action payload — round trip, and malformed input is rejected") {
    std::string body;
    std::int64_t created = 0;
    REQUIRE(
        ParseCommentActionPayload(BuildCommentActionPayload("line 1\n\"quoted\" \xc3\xa9", kQueuedAt), body, created));
    CHECK(body == "line 1\n\"quoted\" \xc3\xa9");
    CHECK(created == kQueuedAt);

    CHECK_FALSE(ParseCommentActionPayload("not json", body, created));
    CHECK_FALSE(ParseCommentActionPayload("[1,2]", body, created));
    CHECK_FALSE(ParseCommentActionPayload("{\"created\":1}", body, created));
    CHECK_FALSE(ParseCommentActionPayload("{\"body\":\"\"}", body, created));
    REQUIRE(ParseCommentActionPayload("{\"body\":\"no time\"}", body, created));
    CHECK(created == 0);
}

TEST_CASE("Worklog action payload — round trip, and a missing time spent is rejected") {
    WorklogActionPayload in;
    in.TimeSpent = "2h 15m";
    in.TimeRemaining = "";
    in.AdjustEstimate = "auto";
    in.Description = "line 1\n\"quoted\" \xc3\xa9";
    in.Started = "2026-09-27T09:00:00.000+0000";
    WorklogActionPayload out;
    REQUIRE(ParseWorklogActionPayload(BuildWorklogActionPayload(in), out));
    CHECK(out.TimeSpent == in.TimeSpent);
    CHECK(out.TimeRemaining.empty());
    CHECK(out.AdjustEstimate == "auto");
    CHECK(out.Description == in.Description);
    CHECK(out.Started == in.Started);

    CHECK_FALSE(ParseWorklogActionPayload("not json", out));
    CHECK_FALSE(ParseWorklogActionPayload("[1]", out));
    CHECK_FALSE(ParseWorklogActionPayload("{\"description\":\"no time\"}", out));
    CHECK_FALSE(ParseWorklogActionPayload("{\"timeSpent\":5}", out)); // wrong type
    REQUIRE(ParseWorklogActionPayload("{\"timeSpent\":\"1h\"}", out));
    CHECK(out.TimeSpent == "1h");
    CHECK(out.Description.empty());
}
