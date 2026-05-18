#ifndef SMATCHET_CODING_HARNESS_TYPES_H
#define SMATCHET_CODING_HARNESS_TYPES_H

// Pure value types exchanged between AgenticHandoffController, ICodingHarnessRunner
// implementations, and the spawned harness's first delegate. Header is always-compiled
// — the declarations are inert (no AGENTIC-dependent symbols), so call sites still
// gate via `#if defined(SMATCHET_WITH_AGENTIC)` when they reference fields that only
// make sense in an agentic build.
//
// Contract surface lives here so all three sides — runner (writer), seed-builder
// (writer), and spawned-harness side (reader) — share one source of truth. The
// SEED.json shape lives in this struct + CodingHarnessSeedBuilder; the JSON
// snapshot test in tests/Source_Core/CodingHarnessSeedBuilder.test.cpp is the
// wire spec.

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>

namespace CodingHarness {

// Seed payload — the contract between the runner (writer) + the spawned harness's
// first delegate (reader). Lives at the harness worktree's SEED.json. payloadExtra
// preserves forward-compat keys — runner round-trips unknown fields so the spawned
// harness side can read them without coordination on every schema bump.
struct Seed {
    int64_t proposalId = 0;          // AgentProposalStore row id
    std::string sourceTracker;       // "github" / "jira" / "plane"
    std::string issueKey;            // e.g. "owner/repo#42"
    std::string issueTitle;          // best-effort from FetchIssueBody
    std::string issueBodyMarkdown;   // raw markdown
    std::string approachOutline;     // from ImplementIssue payload
    std::string complexityHint;      // "low" | "medium" | "high"
    std::string targetBranch;        // e.g. "agent/<id>/<slug>"
    std::string workingDirectory;    // absolute path to worktree root
    int64_t timestampUnixSec = 0;
    nlohmann::json payloadExtra;     // forward-compat — unknown keys round-trip
};

// Stream-json delta event emitted by the harness on stdout. The runner parses
// claude-code's stream-json line by line and emits one StreamEvent per parsed
// JSON object via the IRunner::DeltaCallback.
struct StreamEvent {
    std::string type;                // "message" / "result" / "tool_use" / "error"
    std::string subtype;             // type-specific discriminator
    nlohmann::json data;             // type-specific payload
};

// Clarification request payload — written to CLARIFICATION_NEEDED.json by the
// harness when it needs the user's input mid-run.
struct ClarificationRequest {
    std::string question;            // free-form
    int64_t timestampUnixSec = 0;
};

// Clarification response — written to USER_RESPONSE.json by the runner/UI when
// the user answers the question. Whichever of {worktree file, GitHub-comment
// reply} arrives first wins; the dual-channel design is documented in
// docs/design/agentic-coding-handoff.md § Decisions locked.
struct ClarificationResponse {
    std::string answer;              // free-form
    int64_t timestampUnixSec = 0;
};

// Final run result — written to RUN_RESULT.json by the harness on exit. The
// runner reads it on subprocess exit and uses the fields to drive the next
// FSM transition (Complete vs Failed).
struct RunResult {
    bool ok = false;
    std::string errorMessage;
    std::string prUrl;               // empty if no PR opened
    int filesChanged = 0;
    int linesAdded = 0;
    int linesRemoved = 0;
    nlohmann::json toolUseSummary;   // free-form, optional
};

} // namespace CodingHarness

#endif // SMATCHET_CODING_HARNESS_TYPES_H
