#ifndef SMATCHET_AGENT_TRIAGE_SCENARIO_FIXTURES_H
#define SMATCHET_AGENT_TRIAGE_SCENARIO_FIXTURES_H

// Pure-logic fixture loaders used by the T8 agent-triage-roundtrip scenario.
//
// Two helpers live here:
//   - ParseGitHubIssueCommentsFixture maps the canned GitHub-API-shape JSON
//     comments array in tests/fixtures/github_issues_sample.json onto the
//     internal TrackerIssueComment record. The wire shape (user.login, body,
//     created_at/updated_at as ISO-8601) mirrors the real GitHub REST v3
//     response so the fixture can double for HTTP traffic in scenarios + tests
//     without parser drift.
//   - ParseOllamaProposalsFixture delegates to AgenticInferenceClientPure for
//     the `{ "proposals": [...] }` envelope so the wire schema stays canonical
//     in one place.
//
// Both helpers are deliberately decoupled from cpr / httplib / SQLite / ImGui
// so the doctest rig can exercise them without dragging in the agentic-flow
// runtime stack. The fixture files are static assets (not generated at build
// time), but the loader is tolerant of malformed JSON — both functions return
// false + populate outError so the scenario can degrade to "passed=false"
// rather than crash if a fixture is edited badly.
//
// Lives behind SMATCHET_WITH_AGENTIC source-list-conditional compilation in
// the root CMakeLists.txt; the header itself is plain C++14 with no banned
// dependencies, so the doctest rig builds it unconditionally.

#include "AgenticInferenceClientPure.h"
#include "ITrackerClient.h"

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace agentic {
namespace scenario_fixtures {

// Parses a GitHub-API-shape comments array (the body of
// `GET /repos/{owner}/{repo}/issues/{n}/comments`). Each element must be a
// JSON object with `user.login`, `body`, `created_at`, `updated_at` — extras
// are ignored. ISO-8601 timestamps are parsed to unix-epoch seconds via a
// minimal `YYYY-MM-DDTHH:MM:SSZ` matcher (UTC only; the GitHub API always
// emits the `Z` suffix). Returns false on "not JSON" / "not an array" /
// "element not an object". Per-element missing/wrong-type required keys are
// reported via outError pointing at the offending index.
bool ParseGitHubIssueCommentsFixture(const std::string& jsonBody,
                                     std::vector<TrackerIssueComment>& outComments,
                                     std::string& outError);

// Parses the `{ "proposals": [...] }` envelope produced by the LLM during
// inference and exercised end-to-end in tests/fixtures/ollama_chat_sample.json.
// Wraps AgenticInferenceClientPure::ParseInferenceResponse so the canonical
// per-action payload validation rules live in one place. Returns true on any
// parseable envelope (even zero surviving proposals); returns false on
// envelope-level failures (not JSON, missing `proposals`, wrong type).
bool ParseOllamaProposalsFixture(const std::string& jsonBody,
                                 std::vector<AgenticInferenceClientPure::ProposalDraft>& outProposals,
                                 std::string& outError);

// Minimal ISO-8601 → unix-epoch-seconds parser. Accepts the strict
// `YYYY-MM-DDTHH:MM:SSZ` form GitHub emits; returns 0 on any failure (caller
// treats 0 as "unknown timestamp"). Exposed for the doctest rig — production
// code routes through ParseGitHubIssueCommentsFixture.
std::int64_t ParseIso8601Utc(const std::string& iso);

} // namespace scenario_fixtures
} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_AGENT_TRIAGE_SCENARIO_FIXTURES_H
