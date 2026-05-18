#include <doctest/doctest.h>

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticInferenceClientPure.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgenticInferenceClientPure::ProposedAction;

// Each test builds a fresh in-memory DB. ":memory:" gives a private SQLite handle that
// dies with the AgentProposalStore — perfect for round-trip tests without filesystem
// pollution. WAL is silently ignored on :memory: (logged as warn during ctor); the
// schema-create + CRUD path is unaffected.
AgentProposal MakeProposal(const std::string& issueKey, ProposedAction action = ProposedAction::CommentAdd) {
    AgentProposal p;
    p.sourceTracker = "github";
    p.issueKey = issueKey;
    p.action = action;
    p.rationale = "auto-generated rationale for " + issueKey;
    p.payload = nlohmann::json{{"body", "hello from t4"}};
    return p;
}

} // namespace

TEST_CASE("AgentProposalStore: construct/destruct round-trip is idempotent") {
    // Two constructors over the same in-memory path don't share state (`:memory:` is
    // per-connection) but each must CREATE TABLE IF NOT EXISTS without throwing.
    {
        AgentProposalStore s(":memory:");
        std::string err;
        std::vector<AgentProposal> rows;
        CHECK(s.Query({}, rows, err));
        CHECK(rows.empty());
        CHECK(err.empty());
    }
    {
        AgentProposalStore s(":memory:");
        std::string err;
        std::vector<AgentProposal> rows;
        CHECK(s.Query({}, rows, err));
        CHECK(rows.empty());
    }
}

TEST_CASE("AgentProposalStore: Insert stamps id, createdAtSec, lastUpdatedAtSec") {
    AgentProposalStore s(":memory:");
    AgentProposal p = MakeProposal("smatchet/example#1");
    std::string err;
    REQUIRE(s.Insert(p, err));
    CHECK(err.empty());
    CHECK(p.id > 0);
    CHECK(p.createdAtSec > 0);
    CHECK(p.lastUpdatedAtSec > 0);
    CHECK(p.createdAtSec == p.lastUpdatedAtSec);
    CHECK(p.state == AgentProposalState::Pending);
}

TEST_CASE("AgentProposalStore: Insert + Find round-trips all fields including payload json") {
    AgentProposalStore s(":memory:");
    AgentProposal p = MakeProposal("smatchet/example#2", ProposedAction::DerivedTicketCreate);
    p.payload = nlohmann::json{{"targetTracker", "jira"},
                               {"summary", "implement feature X"},
                               {"description", "long body\nwith newlines"},
                               {"nested", nlohmann::json{{"k", 42}, {"arr", {1, 2, 3}}}}};
    std::string err;
    REQUIRE(s.Insert(p, err));

    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.id == p.id);
    CHECK(got.sourceTracker == "github");
    CHECK(got.issueKey == "smatchet/example#2");
    CHECK(got.action == ProposedAction::DerivedTicketCreate);
    CHECK(got.rationale == p.rationale);
    CHECK(got.payload == p.payload);
    CHECK(got.state == AgentProposalState::Pending);
    CHECK(got.createdAtSec == p.createdAtSec);
    CHECK(got.lastUpdatedAtSec == p.lastUpdatedAtSec);
    CHECK(got.applyError.empty());
}

TEST_CASE("AgentProposalStore: Query filter by state") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal a = MakeProposal("smatchet/example#10");
    AgentProposal b = MakeProposal("smatchet/example#11");
    AgentProposal c = MakeProposal("smatchet/example#12");
    REQUIRE(s.Insert(a, err));
    REQUIRE(s.Insert(b, err));
    REQUIRE(s.Insert(c, err));
    REQUIRE(s.Transition(b.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(c.id, AgentProposalState::Rejected, "", err));

    AgentProposalStore::Filter f;
    f.states.push_back(AgentProposalState::Pending);
    std::vector<AgentProposal> rows;
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 1);
    CHECK(rows[0].id == a.id);

    f.states = {AgentProposalState::Approved, AgentProposalState::Rejected};
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 2);
}

TEST_CASE("AgentProposalStore: Query filter by sourceTracker + issueKey") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal gh1 = MakeProposal("smatchet/example#20");
    AgentProposal gh2 = MakeProposal("smatchet/example#21");
    AgentProposal jira = MakeProposal("PROJ-42");
    jira.sourceTracker = "jira";
    REQUIRE(s.Insert(gh1, err));
    REQUIRE(s.Insert(gh2, err));
    REQUIRE(s.Insert(jira, err));

    AgentProposalStore::Filter f;
    f.sourceTracker = "github";
    std::vector<AgentProposal> rows;
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 2);

    f = {};
    f.issueKey = "PROJ-42";
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 1);
    CHECK(rows[0].sourceTracker == "jira");
}

TEST_CASE("AgentProposalStore: Query limit caps result count") {
    AgentProposalStore s(":memory:");
    std::string err;
    for (int i = 0; i < 8; ++i) {
        AgentProposal p = MakeProposal("smatchet/example#" + std::to_string(30 + i));
        REQUIRE(s.Insert(p, err));
    }
    AgentProposalStore::Filter f;
    f.limit = 5;
    std::vector<AgentProposal> rows;
    REQUIRE(s.Query(f, rows, err));
    CHECK(rows.size() == 5);
}

TEST_CASE("AgentProposalStore: Transition Pending -> Approved succeeds + bumps lastUpdatedAtSec") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#40");
    REQUIRE(s.Insert(p, err));
    const int64_t origUpdated = p.lastUpdatedAtSec;
    // Sleep > 1s so the second wall-clock second-tick is observable. The store
    // stamps unix-epoch seconds, so sub-second waits can collapse to the same value.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE(s.Transition(p.id, AgentProposalState::Approved, "", err));

    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.state == AgentProposalState::Approved);
    CHECK(got.lastUpdatedAtSec > origUpdated);
    CHECK(got.createdAtSec == p.createdAtSec); // unchanged
    CHECK(got.applyError.empty());
}

TEST_CASE("AgentProposalStore: Transition Pending -> Applied is rejected (must go through Approved)") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#41");
    REQUIRE(s.Insert(p, err));
    CHECK_FALSE(s.Transition(p.id, AgentProposalState::Applied, "", err));
    CHECK(err.find("invalid transition") != std::string::npos);

    // Row should still be Pending — the failed transition is purely advisory.
    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.state == AgentProposalState::Pending);
}

TEST_CASE("AgentProposalStore: Transition Approved -> Applied succeeds") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#42");
    REQUIRE(s.Insert(p, err));
    REQUIRE(s.Transition(p.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(p.id, AgentProposalState::Applied, "", err));

    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.state == AgentProposalState::Applied);
    CHECK(got.applyError.empty());
}

TEST_CASE("AgentProposalStore: Transition Approved -> Failed stores applyError") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal p = MakeProposal("smatchet/example#43");
    REQUIRE(s.Insert(p, err));
    REQUIRE(s.Transition(p.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(p.id, AgentProposalState::Failed, "HTTP 502 from backend", err));

    AgentProposal got;
    REQUIRE(s.Find(p.id, got, err));
    CHECK(got.state == AgentProposalState::Failed);
    CHECK(got.applyError == "HTTP 502 from backend");
}

TEST_CASE("AgentProposalStore: Transition on non-existent id returns false") {
    AgentProposalStore s(":memory:");
    std::string err;
    CHECK_FALSE(s.Transition(99999, AgentProposalState::Approved, "", err));
    CHECK(err.find("row not found") != std::string::npos);
}

TEST_CASE("AgentProposalStore: Transition on terminal state returns false") {
    AgentProposalStore s(":memory:");
    std::string err;
    AgentProposal applied = MakeProposal("smatchet/example#50");
    AgentProposal rejected = MakeProposal("smatchet/example#51");
    AgentProposal failed = MakeProposal("smatchet/example#52");
    REQUIRE(s.Insert(applied, err));
    REQUIRE(s.Insert(rejected, err));
    REQUIRE(s.Insert(failed, err));
    REQUIRE(s.Transition(applied.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(applied.id, AgentProposalState::Applied, "", err));
    REQUIRE(s.Transition(rejected.id, AgentProposalState::Rejected, "", err));
    REQUIRE(s.Transition(failed.id, AgentProposalState::Approved, "", err));
    REQUIRE(s.Transition(failed.id, AgentProposalState::Failed, "boom", err));

    CHECK_FALSE(s.Transition(applied.id, AgentProposalState::Pending, "", err));
    CHECK(err.find("invalid transition") != std::string::npos);
    CHECK_FALSE(s.Transition(rejected.id, AgentProposalState::Approved, "", err));
    CHECK(err.find("invalid transition") != std::string::npos);
    CHECK_FALSE(s.Transition(failed.id, AgentProposalState::Approved, "", err));
    CHECK(err.find("invalid transition") != std::string::npos);
}

TEST_CASE("AgentProposalStore: GetPollCursor on missing key returns 0 + ok=true") {
    AgentProposalStore s(":memory:");
    std::string err;
    int64_t cursor = 12345; // sentinel; should be reset to 0 by the call.
    REQUIRE(s.GetPollCursor("github", "smatchet/example", cursor, err));
    CHECK(cursor == 0);
    CHECK(err.empty());
}

TEST_CASE("AgentProposalStore: SetPollCursor + GetPollCursor round-trip") {
    AgentProposalStore s(":memory:");
    std::string err;
    REQUIRE(s.SetPollCursor("github", "smatchet/example", 1700000000, err));
    int64_t cursor = 0;
    REQUIRE(s.GetPollCursor("github", "smatchet/example", cursor, err));
    CHECK(cursor == 1700000000);
}

TEST_CASE("AgentProposalStore: SetPollCursor twice on same key overwrites") {
    AgentProposalStore s(":memory:");
    std::string err;
    REQUIRE(s.SetPollCursor("github", "smatchet/example", 1000, err));
    REQUIRE(s.SetPollCursor("github", "smatchet/example", 2000, err));
    int64_t cursor = 0;
    REQUIRE(s.GetPollCursor("github", "smatchet/example", cursor, err));
    CHECK(cursor == 2000);

    // Different (tracker, repo) tuple stays independent.
    REQUIRE(s.SetPollCursor("github", "smatchet/other", 5000, err));
    REQUIRE(s.GetPollCursor("github", "smatchet/example", cursor, err));
    CHECK(cursor == 2000);
    REQUIRE(s.GetPollCursor("github", "smatchet/other", cursor, err));
    CHECK(cursor == 5000);
}

TEST_CASE("AgentProposalStore: state string helpers round-trip all 5 enum values") {
    const AgentProposalState values[] = {AgentProposalState::Pending, AgentProposalState::Approved,
                                         AgentProposalState::Rejected, AgentProposalState::Applied,
                                         AgentProposalState::Failed};
    for (const auto v : values) {
        const std::string s = AgentProposalStateToString(v);
        AgentProposalState out;
        CHECK(ParseAgentProposalState(s, out));
        CHECK(out == v);
    }

    // Unknown literal stays rejected — caller's `out` is untouched.
    AgentProposalState sentinel = AgentProposalState::Approved;
    CHECK_FALSE(ParseAgentProposalState("Bogus", sentinel));
    CHECK(sentinel == AgentProposalState::Approved);
}

TEST_CASE("AgentProposalStore: agentic action string helpers round-trip via proposal serialization") {
    AgentProposalStore s(":memory:");
    std::string err;
    const ProposedAction actions[] = {ProposedAction::CommentAdd,      ProposedAction::LabelAdd,
                                      ProposedAction::LabelRemove,     ProposedAction::AssigneeSet,
                                      ProposedAction::StateTransition, ProposedAction::DerivedTicketCreate,
                                      ProposedAction::ImplementIssue};
    for (const auto a : actions) {
        AgentProposal p =
            MakeProposal(std::string("smatchet/example#act-") + AgenticInferenceClientPure::ActionToString(a), a);
        REQUIRE(s.Insert(p, err));
        AgentProposal got;
        REQUIRE(s.Find(p.id, got, err));
        CHECK(got.action == a);
    }

    // Direct ParseAgenticAction smoke — including Unknown literal which the parser accepts.
    ProposedAction out = ProposedAction::Unknown;
    CHECK(ParseAgenticAction("CommentAdd", out));
    CHECK(out == ProposedAction::CommentAdd);
    CHECK(ParseAgenticAction("Unknown", out));
    CHECK(out == ProposedAction::Unknown);
    CHECK_FALSE(ParseAgenticAction("NotAnAction", out));
}
