// CoderabbitCommentClassifierPure.h — phase-2 of the `coderabbit-react-loop`
// plan (docs/design/coderabbit-react-loop.md § Phase 2). Pure C++14 helpers
// used by the phase-3 concrete `CoderabbitCommentClassifier` and exercised
// directly by the doctest rig. No third-party / banned-dep includes (no
// cpr / SQLite / ImGui / httplib / sol2). Built into both `SmatchetStandalone`
// and `SmatchetCore_DX12` and into the doctest rig.
//
// Per AGENTS.md § "Pure-helper TU-split recipe" — the parsers live in a
// dependency-light TU so future agentic-flow consumers (the phase-3 concrete
// classifier, the spawned-harness `coderabbit-triage` orchestrator mode in
// phase 8) can call them without dragging in the GitHub HTTP stack.

#ifndef SMATCHET_CODERABBIT_COMMENT_CLASSIFIER_PURE_H
#define SMATCHET_CODERABBIT_COMMENT_CLASSIFIER_PURE_H

#include <cstddef>
#include <string>
#include <vector>

namespace smatchet {
namespace agentic {
namespace coderabbit_pure {

enum class SeverityIcon {
    None,       // No CodeRabbit icon detected
    Actionable, // wrench-and-screwdriver U+1F6E0 + VS-16 (variation selector)
    Caution,    // warning U+26A0 + VS-16
    Nit,        // light-bulb U+1F4A1
    Chore,      // broom U+1F9F9
};

/// Scan the comment body for the first CodeRabbit severity icon. Returns
/// `None` when no icon is found. The icons are 4-byte UTF-8 sequences on
/// the emoji plane (Caution is 3-byte BMP + a 3-byte VS-16) — scan for
/// the canonical encoded byte sequences; we never depend on `std::regex`
/// (slow + locale-flaky on MinGW UCRT).
SeverityIcon ExtractSeverityIcon(const std::string& body);

/// Parse the `_Actionable comments posted: N_` summary header CodeRabbit
/// posts at the top of a triage round. Returns `-1` when the header is not
/// present (so callers can distinguish "0 found in body" from "no header").
/// Tolerates trailing whitespace before the closing `_`.
int ExtractActionableHeaderCount(const std::string& body);

/// Parse the `_Nitpick comments (N)_` summary header (note the bracket
/// shape `(` `)` rather than `:`). Returns `-1` when the header is absent.
int ExtractNitpickHeaderCount(const std::string& body);

/// Extract every triple-backtick `suggestion ... ` fenced block from the
/// body. Returns the suggestion-block bodies (the prose inside the fences,
/// not the fence markers themselves). Empty input or no suggestion blocks
/// → empty vector. Malformed fences (opener with no closer) are ignored.
std::vector<std::string> ExtractSuggestionBlocks(const std::string& body);

/// Test the comment author's login against an allow-list. Case-insensitive
/// substring match — pulled in the bot login `coderabbitai[bot]` matches a
/// configured `"coderabbitai[bot]"` entry regardless of case differences.
/// Empty allow-list → false. Empty login → false. Used by the phase-3
/// concrete classifier; exposed here so the pure-test rig can lock the
/// behaviour without depending on the classifier impl.
bool IsBotLoginAllowed(const std::string& login, const std::vector<std::string>& allowList);

/// Detect the `<!-- smatchet-handoff -->` marker that Smatchet posts on its
/// own PR comments (budget-exhausted notices, reject-replies, etc). The
/// watcher uses this to skip comments it authored. Marker text is canonical
/// — callers do not parameterise it.
bool HasSmatchetHandoffMarker(const std::string& body);

} // namespace coderabbit_pure
} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_CODERABBIT_COMMENT_CLASSIFIER_PURE_H
