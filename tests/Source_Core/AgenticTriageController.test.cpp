#include <doctest/doctest.h>

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticInferenceClientPure.h"
#include "AgenticTriageController.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

using AgenticInferenceClientPure::ProposalDraft;
using AgenticInferenceClientPure::ProposedAction;

// Fake IGitHubReadClient — canned responses controlled per-test. Tracks which
// methods were called for ordering / count assertions.
class FakeGitHub : public smatchet::agentic::IGitHubReadClient {
  public:
    bool bodyOk = true;
    bool commentsOk = true;
    bool listOk = true;
    std::string bodyValue = "Default issue body.";
    std::vector<TrackerIssueComment> commentsValue;
    std::vector<std::string> listValue; // keys
    std::string bodyError = "fake: body fetch failed";
    std::string commentsError = "fake: comments fetch failed";
    std::string listError = "fake: list failed";

    // Per-issue overrides; if an entry exists for issueKey, FetchIssueBody returns false with that error.
    std::vector<std::string> bodyFailFor;

    int bodyCalls = 0;
    int commentCalls = 0;
    int listCalls = 0;

    bool FetchIssueBody(const std::string& issueKey, std::string& outBody, std::string& outError) override {
        ++bodyCalls;
        for (const auto& k : bodyFailFor) {
            if (k == issueKey) {
                outError = "forced failure on " + issueKey;
                return false;
            }
        }
        if (!bodyOk) {
            outError = bodyError;
            return false;
        }
        outBody = bodyValue;
        return true;
    }
    bool FetchIssueComments(const std::string& /*issueKey*/, std::vector<TrackerIssueComment>& outComments,
                            std::string& outError) override {
        ++commentCalls;
        if (!commentsOk) {
            outError = commentsError;
            return false;
        }
        outComments = commentsValue;
        return true;
    }
    bool ListOpenIssuesForRepo(const std::string& /*owner*/, const std::string& /*repo*/,
                               std::vector<std::string>& outKeys, std::string& outError) override {
        ++listCalls;
        if (!listOk) {
            outError = listError;
            return false;
        }
        outKeys = listValue;
        return true;
    }
};

class FakeInference : public smatchet::agentic::IInferenceClient {
  public:
    bool ok = true;
    std::vector<ProposalDraft> draftsValue;
    std::string errorValue = "fake: inference failed";
    int calls = 0;

    bool RequestProposals(const std::string& /*issueBody*/, const std::vector<TrackerIssueComment>& /*comments*/,
                          std::vector<ProposalDraft>& outDrafts, std::string& outError) override {
        ++calls;
        if (!ok) {
            outError = errorValue;
            return false;
        }
        outDrafts = draftsValue;
        return true;
    }
};

ProposalDraft MakeDraft(ProposedAction action, const char* rationaleText) {
    ProposalDraft d;
    d.action = action;
    d.rationale = rationaleText;
    // Action-specific payload — match the parser's per-action shape so the test
    // doubles look realistic. The controller doesn't re-validate; that ran upstream.
    switch (action) {
    case ProposedAction::CommentAdd:
        d.payload = nlohmann::json{{"body", "auto-comment"}};
        break;
    case ProposedAction::LabelAdd:
    case ProposedAction::LabelRemove:
        d.payload = nlohmann::json{{"label", "needs-triage"}};
        break;
    case ProposedAction::AssigneeSet:
        d.payload = nlohmann::json{{"user", "alice"}};
        break;
    case ProposedAction::StateTransition:
        d.payload = nlohmann::json{{"state", "closed"}};
        break;
    default:
        d.payload = nlohmann::json::object();
        break;
    }
    return d;
}

} // namespace

TEST_CASE("AgenticTriageController::TriageIssue happy path inserts every draft as Pending") {
    FakeGitHub gh;
    gh.bodyValue = "Tracker issue body for triage";
    TrackerIssueComment c;
    c.Author = "alice";
    c.Body = "Repro: this happens on launch.";
    c.CreatedAtSec = 1700000000;
    gh.commentsValue.push_back(c);

    FakeInference inf;
    inf.draftsValue.push_back(MakeDraft(ProposedAction::CommentAdd, "ack the reporter"));
    inf.draftsValue.push_back(MakeDraft(ProposedAction::LabelAdd, "tag for grooming"));

    AgentProposalStore store(":memory:");

    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);

    int inserted = 0;
    std::string err;
    REQUIRE(ctrl.TriageIssue("smatchet/example#42", inserted, err));
    CHECK(err.empty());
    CHECK(inserted == 2);
    CHECK(gh.bodyCalls == 1);
    CHECK(gh.commentCalls == 1);
    CHECK(inf.calls == 1);

    // Both proposals should be persisted Pending under the right issue key.
    AgentProposalStore::Filter f;
    f.states = {AgentProposalState::Pending};
    std::vector<AgentProposal> rows;
    REQUIRE(store.Query(f, rows, err));
    CHECK(rows.size() == 2);
    for (const auto& r : rows) {
        CHECK(r.sourceTracker == "github");
        CHECK(r.issueKey == "smatchet/example#42");
        CHECK(r.state == AgentProposalState::Pending);
        CHECK(r.id > 0);
    }
}

TEST_CASE("AgenticTriageController::TriageIssue surfaces GitHub body fetch failure") {
    FakeGitHub gh;
    gh.bodyOk = false;
    gh.bodyError = "401 unauthorized";
    FakeInference inf;
    AgentProposalStore store(":memory:");

    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);
    int inserted = 0;
    std::string err;
    CHECK_FALSE(ctrl.TriageIssue("smatchet/example#1", inserted, err));
    CHECK(inserted == 0);
    CHECK(err.find("FetchIssueBody failed") != std::string::npos);
    CHECK(err.find("401 unauthorized") != std::string::npos);
    // No comments fetched + no inference invoked on body failure.
    CHECK(gh.commentCalls == 0);
    CHECK(inf.calls == 0);
}

TEST_CASE("AgenticTriageController::TriageIssue surfaces inference failure") {
    FakeGitHub gh;
    FakeInference inf;
    inf.ok = false;
    inf.errorValue = "LLM stream error: read timeout";
    AgentProposalStore store(":memory:");

    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);
    int inserted = 0;
    std::string err;
    CHECK_FALSE(ctrl.TriageIssue("smatchet/example#2", inserted, err));
    CHECK(inserted == 0);
    CHECK(err.find("RequestProposals failed") != std::string::npos);
    CHECK(err.find("read timeout") != std::string::npos);
}

TEST_CASE("AgenticTriageController::TriageIssue rejects empty issueKey") {
    FakeGitHub gh;
    FakeInference inf;
    AgentProposalStore store(":memory:");
    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);
    int inserted = 0;
    std::string err;
    CHECK_FALSE(ctrl.TriageIssue(std::string(), inserted, err));
    CHECK(err == "issueKey is empty.");
    CHECK(gh.bodyCalls == 0);
}

TEST_CASE("AgenticTriageController::TriageBatch happy path scans all and aggregates") {
    FakeGitHub gh;
    gh.listValue = {"acme/repo#1", "acme/repo#2", "acme/repo#3"};
    FakeInference inf;
    inf.draftsValue.push_back(MakeDraft(ProposedAction::CommentAdd, "ack"));
    AgentProposalStore store(":memory:");

    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);
    smatchet::agentic::AgenticTriageController::BatchResult result;
    std::string err;
    REQUIRE(ctrl.TriageBatch("github", "acme/repo", result, err));
    CHECK(err.empty());
    CHECK(result.totalIssuesScanned == 3);
    CHECK(result.proposalsInserted == 3); // 1 per issue
    CHECK(result.perIssueErrors.empty());
    CHECK(gh.listCalls == 1);
    CHECK(gh.bodyCalls == 3);
}

TEST_CASE("AgenticTriageController::TriageBatch partial failure leaves successful proposals committed") {
    FakeGitHub gh;
    gh.listValue = {"acme/repo#1", "acme/repo#2", "acme/repo#3"};
    gh.bodyFailFor = {"acme/repo#2"}; // middle issue blows up
    FakeInference inf;
    inf.draftsValue.push_back(MakeDraft(ProposedAction::CommentAdd, "ack"));
    AgentProposalStore store(":memory:");

    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);
    smatchet::agentic::AgenticTriageController::BatchResult result;
    std::string err;
    REQUIRE(ctrl.TriageBatch("github", "acme/repo", result, err));
    CHECK(err.empty());
    CHECK(result.totalIssuesScanned == 3);
    CHECK(result.proposalsInserted == 2); // #1 + #3 succeed
    REQUIRE(result.perIssueErrors.size() == 1);
    CHECK(result.perIssueErrors[0].first == "acme/repo#2");
    CHECK(result.perIssueErrors[0].second.find("forced failure") != std::string::npos);

    // Two pending proposals should land in the store — for #1 and #3 only.
    AgentProposalStore::Filter f;
    std::vector<AgentProposal> rows;
    REQUIRE(store.Query(f, rows, err));
    CHECK(rows.size() == 2);
    bool has1 = false, has3 = false;
    for (const auto& r : rows) {
        if (r.issueKey == "acme/repo#1")
            has1 = true;
        if (r.issueKey == "acme/repo#3")
            has3 = true;
    }
    CHECK(has1);
    CHECK(has3);
}

TEST_CASE("AgenticTriageController::TriageBatch rejects non-github source in T5") {
    FakeGitHub gh;
    FakeInference inf;
    AgentProposalStore store(":memory:");
    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);
    smatchet::agentic::AgenticTriageController::BatchResult result;
    std::string err;
    CHECK_FALSE(ctrl.TriageBatch("jira", "PROJ", result, err));
    CHECK(err.find("sourceTracker must be 'github'") != std::string::npos);
    CHECK(gh.listCalls == 0);
}

TEST_CASE("AgenticTriageController::TriageBatch rejects malformed OWNER/REPO query") {
    FakeGitHub gh;
    FakeInference inf;
    AgentProposalStore store(":memory:");
    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);
    smatchet::agentic::AgenticTriageController::BatchResult result;
    std::string err;

    // No slash.
    CHECK_FALSE(ctrl.TriageBatch("github", "acmerepo", result, err));
    CHECK(err.find("must contain '/'") != std::string::npos);

    // Empty owner.
    CHECK_FALSE(ctrl.TriageBatch("github", "/repo", result, err));
    CHECK(err.find("non-empty") != std::string::npos);

    // Empty repo.
    CHECK_FALSE(ctrl.TriageBatch("github", "acme/", result, err));
    CHECK(err.find("non-empty") != std::string::npos);

    // Too many slashes.
    CHECK_FALSE(ctrl.TriageBatch("github", "acme/team/repo", result, err));
    CHECK(err.find("exactly one") != std::string::npos);

    CHECK(gh.listCalls == 0);
}

TEST_CASE("AgenticTriageController::TriageBatch surfaces discovery error") {
    FakeGitHub gh;
    gh.listOk = false;
    gh.listError = "403 rate-limited";
    FakeInference inf;
    AgentProposalStore store(":memory:");
    smatchet::agentic::AgenticTriageController ctrl(&gh, &inf, &store);
    smatchet::agentic::AgenticTriageController::BatchResult result;
    std::string err;
    CHECK_FALSE(ctrl.TriageBatch("github", "acme/repo", result, err));
    CHECK(err.find("ListOpenIssuesForRepo failed") != std::string::npos);
    CHECK(err.find("403 rate-limited") != std::string::npos);
}

TEST_CASE("AgenticTriageController nullptr deps surface a coherent error") {
    AgentProposalStore store(":memory:");
    smatchet::agentic::AgenticTriageController ctrl(nullptr, nullptr, &store);
    int inserted = 0;
    std::string err;
    CHECK_FALSE(ctrl.TriageIssue("smatchet/example#1", inserted, err));
    CHECK(err.find("not wired") != std::string::npos);
}
