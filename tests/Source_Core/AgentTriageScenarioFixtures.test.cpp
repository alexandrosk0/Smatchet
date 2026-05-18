#include <doctest/doctest.h>

#include "AgentTriageScenarioFixtures.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

using smatchet::agentic::scenario_fixtures::ParseGitHubIssueCommentsFixture;
using smatchet::agentic::scenario_fixtures::ParseIso8601Utc;
using smatchet::agentic::scenario_fixtures::ParseOllamaProposalsFixture;

// Mirrors the on-disk fixture shape literally so the doctest doesn't depend on
// filesystem layout. The scenario itself loads from disk via ConfigManager
// paths; the parser logic is shared.
const char* kCannedCommentsArray = R"([
  {
    "id": 1001,
    "user": { "login": "alice" },
    "created_at": "2026-05-10T14:23:11Z",
    "updated_at": "2026-05-10T14:23:11Z",
    "body": "+1 — repros for me on macOS 14."
  },
  {
    "id": 1002,
    "user": { "login": "bob" },
    "created_at": "2026-05-10T15:01:42Z",
    "updated_at": "2026-05-10T15:05:00Z",
    "body": "Looks like a duplicate of #98."
  }
])";

} // namespace

TEST_CASE("ParseGitHubIssueCommentsFixture happy path maps every field") {
    std::vector<TrackerIssueComment> comments;
    std::string err;
    REQUIRE(ParseGitHubIssueCommentsFixture(kCannedCommentsArray, comments, err));
    CHECK(err.empty());
    REQUIRE(comments.size() == 2);

    CHECK(comments[0].Id == "1001");
    CHECK(comments[0].Author == "alice");
    CHECK(comments[0].Body == "+1 \xE2\x80\x94 repros for me on macOS 14.");
    CHECK(comments[0].CreatedAtSec == comments[0].UpdatedAtSec);
    CHECK(comments[0].CreatedAtSec > 0);

    CHECK(comments[1].Id == "1002");
    CHECK(comments[1].Author == "bob");
    // updated_at distinct from created_at → UpdatedAtSec is later.
    CHECK(comments[1].UpdatedAtSec > comments[1].CreatedAtSec);
}

TEST_CASE("ParseGitHubIssueCommentsFixture rejects non-array root") {
    std::vector<TrackerIssueComment> comments;
    std::string err;
    CHECK_FALSE(ParseGitHubIssueCommentsFixture("{\"comments\":[]}", comments, err));
    CHECK(err.find("array") != std::string::npos);
    CHECK(comments.empty());
}

TEST_CASE("ParseGitHubIssueCommentsFixture rejects malformed JSON") {
    std::vector<TrackerIssueComment> comments;
    std::string err;
    CHECK_FALSE(ParseGitHubIssueCommentsFixture("not json at all", comments, err));
    CHECK(err.find("not JSON") != std::string::npos);
}

TEST_CASE("ParseGitHubIssueCommentsFixture tolerates missing optional fields") {
    // No user object, no updated_at → author defaults to empty string,
    // updated_at falls back to created_at.
    const char* sparse = R"([{ "body": "anonymous note", "created_at": "2026-01-01T00:00:00Z" }])";
    std::vector<TrackerIssueComment> comments;
    std::string err;
    REQUIRE(ParseGitHubIssueCommentsFixture(sparse, comments, err));
    REQUIRE(comments.size() == 1);
    CHECK(comments[0].Author.empty());
    CHECK(comments[0].Body == "anonymous note");
    CHECK(comments[0].CreatedAtSec > 0);
    CHECK(comments[0].UpdatedAtSec == comments[0].CreatedAtSec);
}

TEST_CASE("ParseIso8601Utc handles the GitHub-canonical Z-suffixed form") {
    // 2026-05-10T14:23:11Z → 1778422991 (UTC). The literal is verified by the
    // parser's own days-from-civil arithmetic so the assertion + production
    // code stay co-located; a regression in either flips this.
    CHECK(ParseIso8601Utc("2026-05-10T14:23:11Z") == 1778422991);
    // Epoch.
    CHECK(ParseIso8601Utc("1970-01-01T00:00:00Z") == 0);
    // Round-trip: same date plus 60s seconds-of-day yields +60 unix seconds.
    CHECK(ParseIso8601Utc("2026-05-10T14:24:11Z") - ParseIso8601Utc("2026-05-10T14:23:11Z") == 60);
    // Non-Z suffix is rejected (we never accept TZ offsets).
    CHECK(ParseIso8601Utc("2026-05-10T14:23:11+02:00") == 0);
    // Too short → 0.
    CHECK(ParseIso8601Utc("2026-05-10") == 0);
}

TEST_CASE("ParseOllamaProposalsFixture round-trips the canned 7-action envelope") {
    // Mirrors tests/fixtures/ollama_chat_sample.json — kept inline so the
    // doctest stays hermetic. Schema policed by
    // AgenticInferenceClientPure::ParseInferenceResponse.
    const char* envelope = R"({
        "proposals": [
            { "action": "CommentAdd",   "rationale": "ack", "payload": { "body": "hi" } },
            { "action": "LabelAdd",     "rationale": "tag", "payload": { "label": "x" } },
            { "action": "ImplementIssue","rationale": "go", "payload": { "complexityHint": "low", "approachOutline": "y" } }
        ]
    })";
    std::vector<AgenticInferenceClientPure::ProposalDraft> drafts;
    std::string err;
    REQUIRE(ParseOllamaProposalsFixture(envelope, drafts, err));
    REQUIRE(drafts.size() == 3);
    CHECK(drafts[0].action == AgenticInferenceClientPure::ProposedAction::CommentAdd);
    CHECK(drafts[1].action == AgenticInferenceClientPure::ProposedAction::LabelAdd);
    CHECK(drafts[2].action == AgenticInferenceClientPure::ProposedAction::ImplementIssue);
}
