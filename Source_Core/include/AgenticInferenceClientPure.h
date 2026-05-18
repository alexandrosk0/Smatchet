#ifndef SMATCHET_AGENTIC_INFERENCE_CLIENT_PURE_H
#define SMATCHET_AGENTIC_INFERENCE_CLIENT_PURE_H

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Pure-logic helpers for parsing the LLM inference response envelope used by
// the agentic triage flow. No HTTP / SQLite / ImGui / cpr deps — only
// nlohmann::json, <string>, <vector>, <set>, <mutex>, Logger.h. Lives behind
// `#if defined(SMATCHET_WITH_AGENTIC)` via source-list-conditional compilation
// (see root CMakeLists.txt).
//
// The schema parsed here is locked in
// `docs/design/agentic-flow-implementation.md` § "Decisions locked" → #3.
// Round-trip: LLM emits `{ "proposals": [...] }`; each proposal has an
// `action` discriminator, free-form `rationale`, and an action-specific
// `payload` object whose required keys are validated below.

namespace AgenticInferenceClientPure {

enum class ProposedAction {
    Unknown,
    CommentAdd,
    LabelAdd,
    LabelRemove,
    AssigneeSet,
    StateTransition,
    DerivedTicketCreate,
    ImplementIssue,
};

struct ProposalDraft {
    ProposedAction action;
    std::string rationale;
    nlohmann::json payload;

    ProposalDraft() : action(ProposedAction::Unknown) {}
};

// Parses the `{ "proposals": [...] }` envelope.
//
// Behaviour contract:
// - Returns false on "not JSON" / "no `proposals` key" / "`proposals` not an
//   array"; outError carries the reason.
// - Returns true on any parseable envelope, even if zero valid proposals
//   survive validation (outProposals is then empty).
// - Unknown enum values for `action` are accepted as ProposedAction::Unknown
//   and the proposal is DROPPED (not pushed to outProposals); LOG_WARN fires
//   once per unknown literal seen (de-duped via a process-local set).
// - Per-action payload validation (decision #3 — strict; ambiguous cases
//   prefer the stricter "drop" interpretation):
//     * CommentAdd:           { "body": "<string>" }
//     * LabelAdd / LabelRemove: { "label": "<string>" }
//     * AssigneeSet:          { "user": "<string>" }
//     * StateTransition:      { "state": "open" | "closed" } — strict enum.
//     * DerivedTicketCreate:  { "targetTracker": "jira" | "plane",
//                               "summary": "<string>",
//                               "description": "<string>" }
//     * ImplementIssue:       { "complexityHint": "low" | "medium" | "high",
//                               "approachOutline": "<string>" }
//   Proposals with missing-or-wrong-type required payload fields are
//   DROPPED; LOG_WARN fires with the reason + the proposal's action string
//   (when present).
// - Unknown top-level keys inside the envelope object trigger once-latched
//   LOG_WARN per literal seen.
// - Extras INSIDE per-action payload objects are accepted (forward-compat for
//   future LLM emissions); only top-level extras are warning-tracked.
bool ParseInferenceResponse(const std::string& jsonBody,
                            std::vector<ProposalDraft>& outProposals,
                            std::string& outError);

// Strips a leading ```json / ``` fence and trailing ``` fence (if present)
// from the LLM response text. Some models wrap their JSON output in markdown
// code fences despite system-prompt instructions to emit raw JSON. Idempotent
// — fence-less input is returned verbatim. The strip is conservative: only
// the literal opening triple-backtick (optionally followed by a `json`
// language tag) at the very start and the trailing triple-backtick at the
// very end are removed.
std::string StripMarkdownCodeFence(const std::string& s);

// Hard cap on the accumulated streamed LLM response body, in bytes. Pillar 3
// defense — a hostile or compromised provider can stream gigabytes of delta
// chunks before terminating; without a cap, the accumulator buffer grows
// unbounded and OOMs the process. 10 MB is well above the largest legitimate
// triage response we have observed (~12 KB JSON envelope) while still small
// enough to fail loudly before causing harm. Bundle B SH1.
constexpr std::size_t kMaxLlmResponseBytes = 10 * 1024 * 1024;

// Returns true when appending `addBytes` more bytes to a buffer that already
// holds `currentBytes` would exceed kMaxLlmResponseBytes. Pure helper so the
// streaming accumulator can short-circuit + cancel the request without
// pulling cpr / HTTP into the test surface. Saturating add — if
// currentBytes + addBytes would overflow std::size_t, the helper treats that
// as "exceeds the cap" and returns true.
bool WouldExceedResponseCap(std::size_t currentBytes, std::size_t addBytes);

// Returns true iff the scheduled-poll worker may safely advance the poll
// cursor to wall-clock after a TriageBatch iteration. Bundle B CR#232:1653 —
// if any issue in the iteration failed (LLM timeout, GitHub 5xx, db error,
// etc.), the cursor must stay put so the next poll re-fetches those issues.
// Without this gate, the cursor advanced unconditionally and silently skipped
// the failed issues, causing the triage queue to silently lose them.
// Idempotency at the LLM + insert layer means re-triaging on the next poll is
// cheap and safe. `failedCount` is the size of TriageBatch's per-issue error
// list; zero failures → safe to advance.
bool ShouldAdvancePollCursor(std::size_t failedCount);

// Returns the canonical lowercase-camel action string used in the LLM wire
// schema. Stable across versions — used by AgentProposalStore (T4) for
// SQLite serialisation as well as by the parser. Unknown returns "Unknown".
const char* ActionToString(ProposedAction a);

} // namespace AgenticInferenceClientPure

#endif // SMATCHET_AGENTIC_INFERENCE_CLIENT_PURE_H
