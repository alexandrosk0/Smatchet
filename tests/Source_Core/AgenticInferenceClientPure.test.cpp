#include <doctest/doctest.h>

#include "AgenticInferenceClientPure.h"

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using AgenticInferenceClientPure::ActionToString;
using AgenticInferenceClientPure::ParseInferenceResponse;
using AgenticInferenceClientPure::ProposalDraft;
using AgenticInferenceClientPure::ProposedAction;
using AgenticInferenceClientPure::StripMarkdownCodeFence;

namespace {

// Tiny helper — build a single-proposal envelope, parse, return the result
// alongside the parser verdict. Keeps the happy-path subcases compact.
struct ParseResult {
    bool ok;
    std::vector<ProposalDraft> drafts;
    std::string error;
};

ParseResult ParseEnvelope(const nlohmann::json& envelope) {
    ParseResult r;
    r.ok = ParseInferenceResponse(envelope.dump(), r.drafts, r.error);
    return r;
}

nlohmann::json MakeProposal(const std::string& action, const nlohmann::json& payload,
                            const std::string& rationale = "test rationale") {
    nlohmann::json p = nlohmann::json::object();
    p["action"] = action;
    p["rationale"] = rationale;
    p["payload"] = payload;
    return p;
}

nlohmann::json EnvelopeWith(const std::vector<nlohmann::json>& proposals) {
    nlohmann::json env = nlohmann::json::object();
    nlohmann::json arr = nlohmann::json::array();
    for (std::size_t i = 0; i < proposals.size(); ++i)
        arr.push_back(proposals[i]);
    env["proposals"] = std::move(arr);
    return env;
}

} // namespace

TEST_CASE("ParseInferenceResponse — happy path: all 7 actions") {
    std::vector<nlohmann::json> proposals;
    proposals.push_back(MakeProposal("CommentAdd", {{"body", "please provide a repro"}}));
    proposals.push_back(MakeProposal("LabelAdd", {{"label", "needs-triage"}}));
    proposals.push_back(MakeProposal("LabelRemove", {{"label", "stale"}}));
    proposals.push_back(MakeProposal("AssigneeSet", {{"user", "alexandrosk0"}}));
    proposals.push_back(MakeProposal("StateTransition", {{"state", "closed"}}));
    proposals.push_back(MakeProposal("DerivedTicketCreate", {{"targetTracker", "jira"},
                                                             {"summary", "perf regression"},
                                                             {"description", "p99 lifted from 18ms"}}));
    proposals.push_back(MakeProposal("ImplementIssue",
                                     {{"complexityHint", "medium"}, {"approachOutline", "switch to shared_mutex"}}));

    const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
    REQUIRE(r.ok);
    CHECK(r.error.empty());
    REQUIRE(r.drafts.size() == 7);

    CHECK(r.drafts.at(0).action == ProposedAction::CommentAdd);
    CHECK(r.drafts.at(1).action == ProposedAction::LabelAdd);
    CHECK(r.drafts.at(2).action == ProposedAction::LabelRemove);
    CHECK(r.drafts.at(3).action == ProposedAction::AssigneeSet);
    CHECK(r.drafts.at(4).action == ProposedAction::StateTransition);
    CHECK(r.drafts.at(5).action == ProposedAction::DerivedTicketCreate);
    CHECK(r.drafts.at(6).action == ProposedAction::ImplementIssue);

    CHECK(r.drafts.at(0).payload.at("body").get<std::string>() == "please provide a repro");
    CHECK(r.drafts.at(5).payload.at("targetTracker").get<std::string>() == "jira");
    CHECK(r.drafts.at(6).payload.at("complexityHint").get<std::string>() == "medium");
}

TEST_CASE("ParseInferenceResponse — empty proposals array is parseable") {
    nlohmann::json env = nlohmann::json::object();
    env["proposals"] = nlohmann::json::array();
    const ParseResult r = ParseEnvelope(env);
    CHECK(r.ok);
    CHECK(r.drafts.empty());
}

TEST_CASE("ParseInferenceResponse — envelope errors return false") {
    SUBCASE("not JSON") {
        std::vector<ProposalDraft> drafts;
        std::string err;
        CHECK_FALSE(ParseInferenceResponse("not json at all{", drafts, err));
        CHECK_FALSE(err.empty());
    }
    SUBCASE("missing proposals key") {
        nlohmann::json env = nlohmann::json::object();
        env["other"] = "value";
        const ParseResult r = ParseEnvelope(env);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("proposals") != std::string::npos);
    }
    SUBCASE("proposals is not an array") {
        nlohmann::json env = nlohmann::json::object();
        env["proposals"] = "should-be-array";
        const ParseResult r = ParseEnvelope(env);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("proposals") != std::string::npos);
    }
    SUBCASE("envelope is a JSON array (not object)") {
        std::vector<ProposalDraft> drafts;
        std::string err;
        CHECK_FALSE(ParseInferenceResponse("[1, 2, 3]", drafts, err));
        CHECK_FALSE(err.empty());
    }
}

TEST_CASE("ParseInferenceResponse — per-action payload validation drops invalid proposals") {
    SUBCASE("CommentAdd missing body") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("CommentAdd", nlohmann::json::object()));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("LabelAdd missing label") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("LabelAdd", {{"not-label", "x"}}));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("LabelRemove empty-string label dropped") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("LabelRemove", {{"label", "   "}}));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("AssigneeSet missing user") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("AssigneeSet", nlohmann::json::object()));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("StateTransition state=open valid") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("StateTransition", {{"state", "open"}}));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        REQUIRE(r.drafts.size() == 1);
        CHECK(r.drafts.at(0).action == ProposedAction::StateTransition);
    }
    SUBCASE("StateTransition state=merged dropped (strict enum)") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("StateTransition", {{"state", "merged"}}));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("DerivedTicketCreate targetTracker=github dropped") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("DerivedTicketCreate",
                                         {{"targetTracker", "github"}, {"summary", "x"}, {"description", "y"}}));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("DerivedTicketCreate missing description dropped") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(
            MakeProposal("DerivedTicketCreate", {{"targetTracker", "plane"}, {"summary", "x"}}));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("ImplementIssue complexityHint=critical dropped (strict enum)") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("ImplementIssue",
                                         {{"complexityHint", "critical"}, {"approachOutline", "x"}}));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("ImplementIssue missing approachOutline dropped") {
        std::vector<nlohmann::json> proposals;
        proposals.push_back(MakeProposal("ImplementIssue", {{"complexityHint", "low"}}));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
    SUBCASE("Payload is not an object — proposal dropped") {
        std::vector<nlohmann::json> proposals;
        nlohmann::json p = nlohmann::json::object();
        p["action"] = "CommentAdd";
        p["rationale"] = "x";
        p["payload"] = "should be an object";
        proposals.push_back(std::move(p));
        const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
        CHECK(r.ok);
        CHECK(r.drafts.empty());
    }
}

TEST_CASE("ParseInferenceResponse — unknown action dropped, parse still succeeds") {
    std::vector<nlohmann::json> proposals;
    proposals.push_back(MakeProposal("DoSomethingNovel", {{"foo", "bar"}}));
    proposals.push_back(MakeProposal("CommentAdd", {{"body", "still valid"}}));
    const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
    CHECK(r.ok);
    REQUIRE(r.drafts.size() == 1);
    CHECK(r.drafts.at(0).action == ProposedAction::CommentAdd);
}

TEST_CASE("ParseInferenceResponse — proposal missing action key dropped") {
    nlohmann::json p = nlohmann::json::object();
    p["rationale"] = "no action key here";
    p["payload"] = nlohmann::json::object();
    std::vector<nlohmann::json> proposals;
    proposals.push_back(std::move(p));
    const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
    CHECK(r.ok);
    CHECK(r.drafts.empty());
}

TEST_CASE("ParseInferenceResponse — unknown top-level keys accepted (extras tolerated)") {
    nlohmann::json env = nlohmann::json::object();
    env["proposals"] = nlohmann::json::array();
    env["extra-metadata"] = nlohmann::json::object();
    env["modelVersion"] = "test";
    const ParseResult r = ParseEnvelope(env);
    CHECK(r.ok);
}

TEST_CASE("ParseInferenceResponse — extra keys inside payload accepted (not dropped)") {
    std::vector<nlohmann::json> proposals;
    proposals.push_back(
        MakeProposal("CommentAdd", {{"body", "valid"}, {"futureField", "should be ignored, not dropped"}}));
    const ParseResult r = ParseEnvelope(EnvelopeWith(proposals));
    CHECK(r.ok);
    REQUIRE(r.drafts.size() == 1);
    CHECK(r.drafts.at(0).payload.at("body").get<std::string>() == "valid");
    CHECK(r.drafts.at(0).payload.at("futureField").get<std::string>() == "should be ignored, not dropped");
}

TEST_CASE("StripMarkdownCodeFence — strips ```json {...} ``` wrapping") {
    const std::string fenced = "```json\n{\"proposals\": []}\n```";
    const std::string stripped = StripMarkdownCodeFence(fenced);
    CHECK(stripped == "{\"proposals\": []}");
}

TEST_CASE("StripMarkdownCodeFence — strips bare ``` ... ``` wrapping") {
    const std::string fenced = "```\n{\"proposals\": []}\n```";
    const std::string stripped = StripMarkdownCodeFence(fenced);
    CHECK(stripped == "{\"proposals\": []}");
}

TEST_CASE("StripMarkdownCodeFence — fence-less input returned verbatim") {
    const std::string raw = "{\"proposals\": []}";
    CHECK(StripMarkdownCodeFence(raw) == raw);
}

TEST_CASE("StripMarkdownCodeFence — leading whitespace tolerated") {
    const std::string fenced = "\n  ```json\n{\"proposals\": []}\n```\n";
    const std::string stripped = StripMarkdownCodeFence(fenced);
    CHECK(stripped == "{\"proposals\": []}");
}

TEST_CASE("ParseInferenceResponse — markdown-fenced JSON still parseable") {
    nlohmann::json env = nlohmann::json::object();
    env["proposals"] = nlohmann::json::array();
    nlohmann::json p = nlohmann::json::object();
    p["action"] = "CommentAdd";
    p["rationale"] = "fence-wrapped";
    p["payload"] = {{"body", "ok"}};
    env["proposals"].push_back(p);
    const std::string fenced = "```json\n" + env.dump() + "\n```";

    std::vector<ProposalDraft> drafts;
    std::string err;
    CHECK(ParseInferenceResponse(fenced, drafts, err));
    REQUIRE(drafts.size() == 1);
    CHECK(drafts.at(0).action == ProposedAction::CommentAdd);
}

TEST_CASE("ActionToString — round-trip for all 7 actions + Unknown") {
    CHECK(std::string(ActionToString(ProposedAction::CommentAdd)) == "CommentAdd");
    CHECK(std::string(ActionToString(ProposedAction::LabelAdd)) == "LabelAdd");
    CHECK(std::string(ActionToString(ProposedAction::LabelRemove)) == "LabelRemove");
    CHECK(std::string(ActionToString(ProposedAction::AssigneeSet)) == "AssigneeSet");
    CHECK(std::string(ActionToString(ProposedAction::StateTransition)) == "StateTransition");
    CHECK(std::string(ActionToString(ProposedAction::DerivedTicketCreate)) == "DerivedTicketCreate");
    CHECK(std::string(ActionToString(ProposedAction::ImplementIssue)) == "ImplementIssue");
    CHECK(std::string(ActionToString(ProposedAction::Unknown)) == "Unknown");
}

TEST_CASE("ParseInferenceResponse — round-trip the recorded fixture") {
    // CTest under the ninja-test-msys2 preset runs SmatchetTests.exe from
    // build/ninja-test-msys2/tests/, but the fixture lives in the source
    // tree under tests/fixtures/. Probe a handful of candidate relative
    // paths plus the SMATCHET_TEST_FIXTURES env override (matches the
    // pattern in ConfigMigration.test.cpp).
    const char* envOverride = std::getenv("SMATCHET_TEST_FIXTURES");
    const std::string candidates[] = {
        envOverride ? (std::string(envOverride) + "/ollama_chat_sample.json") : std::string(),
        "tests/fixtures/ollama_chat_sample.json",
        "../tests/fixtures/ollama_chat_sample.json",
        "../../tests/fixtures/ollama_chat_sample.json",
        "../../../tests/fixtures/ollama_chat_sample.json",
    };
    std::string body;
    for (const std::string& path : candidates) {
        if (path.empty())
            continue;
        std::ifstream f(path, std::ios::binary);
        if (f.good()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            body = ss.str();
            break;
        }
    }
    if (body.empty()) {
        // Skip silently when the fixture is not reachable under the current
        // build layout — the parser is already covered by the synthetic
        // test cases above. The fixture round-trip is a smoke check, not a
        // regression gate.
        WARN("ollama_chat_sample.json fixture not found via any candidate path — skipping fixture round-trip");
        return;
    }
    std::vector<ProposalDraft> drafts;
    std::string err;
    CHECK(ParseInferenceResponse(body, drafts, err));
    CHECK(err.empty());
    // Fixture carries one of each action; all should survive validation.
    CHECK(drafts.size() == 7);
}
