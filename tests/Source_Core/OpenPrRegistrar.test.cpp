// OpenPrRegistrar — phase-1 of the `coderabbit-react-loop` plan.
//
// Drives the registrar against a scripted `OpenPrLister` seam (no real
// `gh` subprocess) + a `:memory:` AgentProposalStore. Covers:
//   1. `ParseGhPrListOutput` — happy path, empty array, malformed JSON,
//      missing-field tolerance.
//   2. `Tick` — upsert idempotency over a static listing.
//   3. `Tick` — cursor-field preservation across re-upsert.
//   4. `Tick` — lister error path (LOG_WARN + 0 upserts, no crash).

#include "OpenPrRegistrar.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposalStore.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using ::smatchet::agentic::OpenPrListing;
using ::smatchet::agentic::OpenPrRegistrar;

TEST_SUITE("OpenPrRegistrar") {

TEST_CASE("ParseGhPrListOutput: happy path with 3 rows") {
    const std::string json = R"([
        {"number": 100, "headRefName": "feat/a", "headRefOid": "abc111", "url": "https://github.com/o/r/pull/100"},
        {"number": 200, "headRefName": "fix/b",  "headRefOid": "def222", "url": "https://github.com/o/r/pull/200"},
        {"number": 300, "headRefName": "chore/c","headRefOid": "ghi333", "url": "https://github.com/o/r/pull/300"}
    ])";
    std::vector<OpenPrListing> out;
    std::string err;
    REQUIRE(OpenPrRegistrar::ParseGhPrListOutput(json, out, err));
    CHECK(err.empty());
    REQUIRE(out.size() == 3);
    CHECK(out[0].number == 100);
    CHECK(out[0].headRefName == "feat/a");
    CHECK(out[0].headSha == "abc111");
    CHECK(out[0].url == "https://github.com/o/r/pull/100");
    CHECK(out[2].number == 300);
    CHECK(out[2].url == "https://github.com/o/r/pull/300");
}

TEST_CASE("ParseGhPrListOutput: empty array yields zero listings") {
    std::vector<OpenPrListing> out;
    std::string err;
    REQUIRE(OpenPrRegistrar::ParseGhPrListOutput("[]", out, err));
    CHECK(out.empty());
    CHECK(err.empty());
}

TEST_CASE("ParseGhPrListOutput: malformed JSON fails") {
    std::vector<OpenPrListing> out;
    std::string err;
    CHECK_FALSE(OpenPrRegistrar::ParseGhPrListOutput("{ not valid json", out, err));
    CHECK(!err.empty());
    CHECK(out.empty());
}

TEST_CASE("ParseGhPrListOutput: top-level non-array fails") {
    std::vector<OpenPrListing> out;
    std::string err;
    CHECK_FALSE(OpenPrRegistrar::ParseGhPrListOutput(R"({"number": 1})", out, err));
    CHECK(!err.empty());
    CHECK(out.empty());
}

TEST_CASE("ParseGhPrListOutput: entry missing a field is skipped (not fatal)") {
    // First entry is well-formed; second is missing `url`; third is well-formed.
    const std::string json = R"([
        {"number": 1, "headRefName": "a", "headRefOid": "x", "url": "https://github.com/o/r/pull/1"},
        {"number": 2, "headRefName": "b", "headRefOid": "y"},
        {"number": 3, "headRefName": "c", "headRefOid": "z", "url": "https://github.com/o/r/pull/3"}
    ])";
    std::vector<OpenPrListing> out;
    std::string err;
    REQUIRE(OpenPrRegistrar::ParseGhPrListOutput(json, out, err));
    CHECK(err.empty());
    // Two valid rows survive; the missing-`url` row is dropped.
    REQUIRE(out.size() == 2);
    CHECK(out[0].number == 1);
    CHECK(out[1].number == 3);
}

TEST_CASE("Tick: upserts every listing into agent_open_pr_watch (idempotency)") {
    AgentProposalStore store(":memory:");
    OpenPrRegistrar reg(&store, "develop");

    std::vector<OpenPrListing> canned = {
        {/*number*/ 10, /*headRefName*/ "feat/a", /*headSha*/ "sha-a", /*url*/ "https://github.com/o/r/pull/10"},
        {/*number*/ 11, /*headRefName*/ "feat/b", /*headSha*/ "sha-b", /*url*/ "https://github.com/o/r/pull/11"},
        {/*number*/ 12, /*headRefName*/ "feat/c", /*headSha*/ "sha-c", /*url*/ "https://github.com/o/r/pull/12"},
    };
    reg.SetOpenPrLister(
        [&canned](std::vector<OpenPrListing>& out, std::string& /*err*/) -> bool {
            out = canned;
            return true;
        });

    // First tick — three fresh inserts.
    CHECK(reg.Tick() == 3);
    std::vector<AgentProposalStore::OpenPrWatchRow> rows;
    std::string err;
    REQUIRE(store.ListOpenPrWatch(rows, err));
    REQUIRE(rows.size() == 3);
    // ORDER BY pr_url ASC — pull/10 < pull/11 < pull/12 lexicographically.
    CHECK(rows[0].prUrl == "https://github.com/o/r/pull/10");
    CHECK(rows[0].prNumber == 10);
    CHECK(rows[0].headRefName == "feat/a");
    CHECK(rows[0].headSha == "sha-a");
    CHECK(rows[2].prUrl == "https://github.com/o/r/pull/12");

    // Second tick with the same listings — still 3 rows, head fields unchanged.
    CHECK(reg.Tick() == 3);
    rows.clear();
    REQUIRE(store.ListOpenPrWatch(rows, err));
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].headSha == "sha-a");
    CHECK(rows[1].headSha == "sha-b");
    CHECK(rows[2].headSha == "sha-c");
}

TEST_CASE("Tick: rebinds head fields, preserves cursor fields on re-upsert") {
    AgentProposalStore store(":memory:");
    OpenPrRegistrar reg(&store, "develop");

    // Pre-populate one row with non-default cursor fields. This is what a
    // future phase-3 comment-classifier tick would have left behind.
    AgentProposalStore::OpenPrWatchRow seed;
    seed.prUrl = "https://github.com/o/r/pull/42";
    seed.prNumber = 42;
    seed.headRefName = "feat/old-name";
    seed.headSha = "old-sha";
    seed.lastSeenCommentIdStr = "42";
    seed.lastSeenCheckRunId = 7777;
    seed.iterationCount = 2;
    seed.lastPolledAtSec = 1700000000;
    std::string err;
    REQUIRE(store.SetOpenPrWatch(seed, err));

    // Registrar tick reports the SAME pr_url but with a newer head sha + a
    // renamed branch (simulating a force-push + branch rename).
    std::vector<OpenPrListing> canned = {
        {/*number*/ 42, /*headRefName*/ "feat/new-name", /*headSha*/ "new-sha",
         /*url*/ "https://github.com/o/r/pull/42"},
    };
    reg.SetOpenPrLister(
        [&canned](std::vector<OpenPrListing>& out, std::string& /*e*/) -> bool {
            out = canned;
            return true;
        });

    CHECK(reg.Tick() == 1);

    AgentProposalStore::OpenPrWatchRow got;
    REQUIRE(store.GetOpenPrWatch("https://github.com/o/r/pull/42", got, err));
    // Head fields rebound.
    CHECK(got.prNumber == 42);
    CHECK(got.headRefName == "feat/new-name");
    CHECK(got.headSha == "new-sha");
    // Cursor fields preserved.
    CHECK(got.lastSeenCommentIdStr == "42");
    CHECK(got.lastSeenCheckRunId == 7777);
    CHECK(got.iterationCount == 2);
    CHECK(got.lastPolledAtSec == 1700000000);
}

TEST_CASE("Tick: lister error returns 0 + no crash + no rows upserted") {
    AgentProposalStore store(":memory:");
    OpenPrRegistrar reg(&store, "develop");
    reg.SetOpenPrLister([](std::vector<OpenPrListing>& /*out*/, std::string& err) -> bool {
        err = "simulated gh failure";
        return false;
    });

    CHECK(reg.Tick() == 0);
    std::vector<AgentProposalStore::OpenPrWatchRow> rows;
    std::string err;
    REQUIRE(store.ListOpenPrWatch(rows, err));
    CHECK(rows.empty());
}

TEST_CASE("Tick: lister unwired returns 0 + no crash") {
    AgentProposalStore store(":memory:");
    OpenPrRegistrar reg(&store, "develop");
    // No SetOpenPrLister call.
    CHECK(reg.Tick() == 0);
}

TEST_CASE("Tick: empty listings yields 0 upserts + empty table") {
    AgentProposalStore store(":memory:");
    OpenPrRegistrar reg(&store, "develop");
    reg.SetOpenPrLister(
        [](std::vector<OpenPrListing>& out, std::string& /*e*/) -> bool {
            out.clear();
            return true;
        });
    CHECK(reg.Tick() == 0);
    std::vector<AgentProposalStore::OpenPrWatchRow> rows;
    std::string err;
    REQUIRE(store.ListOpenPrWatch(rows, err));
    CHECK(rows.empty());
}

TEST_CASE("Constructor defends empty base branch") {
    AgentProposalStore store(":memory:");
    OpenPrRegistrar reg(&store, /*watchedBaseBranch*/ "");
    // Empty string in the ctor body is coerced to "develop".
    CHECK(reg.GetWatchedBaseBranch() == "develop");

    OpenPrRegistrar reg2(&store, "main");
    CHECK(reg2.GetWatchedBaseBranch() == "main");
    reg2.SetWatchedBaseBranch("");
    // Empty arg is rejected — branch unchanged.
    CHECK(reg2.GetWatchedBaseBranch() == "main");
    reg2.SetWatchedBaseBranch("staging");
    CHECK(reg2.GetWatchedBaseBranch() == "staging");
}

} // TEST_SUITE("OpenPrRegistrar")

#endif // SMATCHET_WITH_AGENTIC
