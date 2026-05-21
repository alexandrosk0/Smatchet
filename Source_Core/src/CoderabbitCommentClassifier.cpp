// CoderabbitCommentClassifier.cpp — phase-3 of the `coderabbit-react-loop`
// plan. Concrete `PrCommentClassifier` impl. The 18-rule descriptor table +
// detector functions live in this TU's file-scope anonymous namespace so
// the public header stays minimal (just the policy class + the small set
// of static helpers needed by the doctest rig).
//
// Detector contract: each detector returns true when the comment body's
// suggestion shape matches the rule (i.e. the rule SHOULD fire = reject).
// Detectors run on the comment body verbatim. `PostedComment` carries
// `body` + `author` only (no cited file path on the struct as of phase 3),
// so path-conditional rules (#2, #14) degrade to body-only substring
// heuristics — false-negative-prefer is the documented bias: a missed
// reject routes to coderabbit-triage which performs the full inspection;
// a false-positive reject is annoying noise on the PR thread.

#if defined(SMATCHET_WITH_AGENTIC)

#include "CoderabbitCommentClassifier.h"

#include "CoderabbitCommentClassifierPure.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace smatchet {
namespace agentic {

namespace {

// Local helpers — pure ASCII case-folding + substring search. We do NOT
// pull <regex>: locale-flaky + slow on MinGW UCRT. Two-pass scans against
// a lowered copy of the body are O(N * M) but N is tiny (typical
// CodeRabbit body < 8 KB) and the call frequency is low (one per posted
// comment).

char AsciiToLower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

std::string AsciiLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    // Raw loop kept; std::transform with back_inserter + lambda binding
    // AsciiToLower is less readable in C++14.
    for (char c : s) {
        // cppcheck-suppress useStlAlgorithm
        out.push_back(AsciiToLower(c));
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    return haystack.find(needle) != std::string::npos;
}

// Detector signature — runs on the raw body. Detectors that benefit from
// case-folding lower the body once locally; we do not pre-lower at the
// top of Classify() because some detectors look at suggestion-block
// content (case-sensitive token matches like `std::string_view`).
using OverrideDetector = bool (*)(const std::string& body);

struct OverrideRule {
    int number;
    const char* shortReason; // matches `agents/coderabbit-triage.md` "Reject because" column
    OverrideDetector detector;
};

// Helper — concat all suggestion blocks into one searchable string. Most
// detectors care equally about prose + suggestion-block content; the
// concatenation simplifies substring scans.
std::string CollectBodyForScan(const std::string& body) {
    std::vector<std::string> blocks = coderabbit_pure::ExtractSuggestionBlocks(body);
    if (blocks.empty()) {
        return body;
    }
    std::string out;
    out.reserve(body.size() + 32);
    out.append(body);
    for (const std::string& b : blocks) {
        out.push_back('\n');
        out.append(b);
    }
    return out;
}

// Rule 1 — C++17+ tokens banned by AGENTS.md § Project rules (C++14 hard).
// We scan body + suggestion blocks for the canonical token forms. The
// `std::` prefix is required to avoid false positives on prose like "the
// optional comma" / "string view of the data".
bool DetectCpp17PlusTokens(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    if (Contains(scan, "std::string_view")) {
        return true;
    }
    if (Contains(scan, "std::optional")) {
        return true;
    }
    if (Contains(scan, "std::variant")) {
        return true;
    }
    if (Contains(scan, "if constexpr")) {
        return true;
    }
    // Structured-binding heuristic — `auto [name`. Conservative; matches
    // only the canonical opener, not `auto& [`. The bug we care about is
    // CodeRabbit literally writing `auto [x, y] = pair;` in a suggestion.
    if (Contains(scan, "auto [")) {
        return true;
    }
    if (Contains(scan, "auto& [")) {
        return true;
    }
    if (Contains(scan, "auto&& [")) {
        return true;
    }
    if (Contains(scan, "const auto [")) {
        return true;
    }
    if (Contains(scan, "const auto& [")) {
        return true;
    }
    return false;
}

// Rule 2 — header-only OpenGL include. Path-conditional in the markdown
// table; on phase 3 we can only inspect the comment body, so we look for
// the literal `#include <GLFW/` / `<glad/` / `<GL/` token combined with a
// header-file path mention (`.h` substring nearby). Conservative — a
// stray prose mention of "GLFW headers" is not enough.
bool DetectGlInHeader(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    const bool hasGlInclude =
        Contains(scan, "#include <GLFW/") || Contains(scan, "#include <glad/") || Contains(scan, "#include <GL/");
    if (!hasGlInclude) {
        return false;
    }
    // Heuristic — the body cites a header file under Source_Core/include/.
    // Future phases that propagate `cited_path` should tighten this.
    const std::string lower = AsciiLower(scan);
    return Contains(lower, "source_core/include/") || Contains(lower, ".h");
}

// Rule 3 — local redefinition of `IMGUI_USE_WCHAR32`. Match on the
// literal `#define IMGUI_USE_WCHAR32` token in a suggestion.
bool DetectImguiWchar32Redef(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    return Contains(scan, "#define IMGUI_USE_WCHAR32");
}

// Rule 4 — replace `LOG_*` with `printf` / `std::cerr` / `std::cout` /
// `fprintf(stderr,…)`. We look for the printf/cerr/cout/fprintf
// suggestion forms (case-sensitive) in body or suggestion blocks. False
// positive on test code is fine — CodeRabbit doesn't typically suggest
// printf in tests.
bool DetectLogToPrintf(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    return Contains(scan, "std::cerr") || Contains(scan, "std::cout") || Contains(scan, "fprintf(stderr") ||
           Contains(scan, "fprintf(stdout") || Contains(scan, "printf(\"") || Contains(scan, "printf(R\"");
}

// Rule 5 — nlohmann::json brace-list reassignment. CodeRabbit's typical
// shape is `obj = {{"k", v}, ...}` or `j = {...}` against a previously-
// assigned json. Conservative match — require either the literal token
// `json::object(` (cast away to object) or the pattern `= {{` (brace
// init of an existing json).
bool DetectJsonBraceListReassign(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    // Token forms that indicate the antipattern. Conservative.
    if (Contains(scan, "json::object()") && Contains(scan, "= {")) {
        return true;
    }
    if (Contains(scan, "= json{") || Contains(scan, "= nlohmann::json{")) {
        return true;
    }
    // The literal `obj = {` shape requires a contextual `obj["` use elsewhere
    // in the body to avoid matching every brace literal in the file.
    if (Contains(scan, "nlohmann::json") && Contains(scan, "= {{")) {
        return true;
    }
    return false;
}

// Rule 6 — raw new/delete outside the sol2 / ImGui callback edge cases.
// We match on the literal `new ` / `delete ` opener in a suggestion
// fence. False positives on `new line` / `delete the comment` are
// excluded by requiring a trailing identifier-like character (letter or
// `[`).
bool DetectRawNewDelete(const std::string& body) {
    std::vector<std::string> blocks = coderabbit_pure::ExtractSuggestionBlocks(body);
    if (blocks.empty()) {
        return false;
    }
    // cppcheck-suppress useStlAlgorithm
    for (const std::string& b : blocks) {
        // Raw loop kept; std::any_of with a nested-loop lambda over `find`
        // positions is harder to read than the explicit double-loop body below.
        // Look for `new <Type>(`. Requires a capital letter or `std::`
        // immediately after the keyword.
        std::size_t pos = 0;
        while ((pos = b.find("new ", pos)) != std::string::npos) {
            std::size_t after = pos + 4;
            if (after < b.size()) {
                char c = b[after];
                if ((c >= 'A' && c <= 'Z') || c == '_' || c == ':') {
                    return true;
                }
            }
            pos = after;
        }
        pos = 0;
        while ((pos = b.find("delete ", pos)) != std::string::npos) {
            std::size_t after = pos + 7;
            if (after < b.size()) {
                char c = b[after];
                if ((c >= 'a' && c <= 'z') || (c == '[')) {
                    return true;
                }
            }
            pos = after;
        }
    }
    return false;
}

// Rule 7 — bypass `TrackerHttpClient` and call `cpr::` directly from a
// feature file. We detect bare `cpr::Get(` / `cpr::Post(` / etc in a
// suggestion. Rule does NOT fire when `TrackerHttpClient` is also
// mentioned in the body (= legitimate context for the suggestion).
bool DetectCprBypass(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    const bool hasBareCpr = Contains(scan, "cpr::Get(") || Contains(scan, "cpr::Post(") ||
                            Contains(scan, "cpr::Put(") || Contains(scan, "cpr::Delete(") ||
                            Contains(scan, "cpr::Patch(");
    if (!hasBareCpr) {
        return false;
    }
    // CodeRabbit naming the TrackerHttpClient class explicitly in the same
    // body usually means "use this wrapper instead" — that's an OK
    // suggestion, not a reject candidate. Skip when both appear together.
    if (Contains(scan, "TrackerHttpClient")) {
        return false;
    }
    return true;
}

// Rule 8 — sync IO inlined into an `ImGui::*`-reachable frame. Strongest
// signal is the combination of a blocking call (`cpr::`, `SQLite::`,
// `p4 `, `std::ifstream`, `std::mutex::lock`) and an ImGui-frame token
// (`ImGui::Begin`, `ImGui::Render`, `Draw()`, `Frame()`) in the same body.
bool DetectSyncOnUiFrame(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    // Detect lock-acquisition forms specifically — a bare `std::mutex`
    // mention covers declarations and comments and produces false
    // positives. The pillar-2 critical condition is the act of acquiring
    // the lock, not the type's existence.
    const bool hasBlocking = Contains(scan, "cpr::") || Contains(scan, "SQLite::Database") ||
                             Contains(scan, "SQLite::Statement") || Contains(scan, "std::ifstream") ||
                             Contains(scan, ".lock(") || Contains(scan, "std::lock_guard") ||
                             Contains(scan, "std::unique_lock") || Contains(scan, "p4 ");
    const bool hasUiFrame = Contains(scan, "ImGui::Begin") || Contains(scan, "ImGui::Render") ||
                            Contains(scan, "ImGui::SameLine") || Contains(scan, "RenderFrame(") ||
                            Contains(scan, "DrawFrame(");
    return hasBlocking && hasUiFrame;
}

// Rule 9 — "drop the LOG_TRACE / LOG_DEBUG from this non-trivial branch
// for cleanliness". Detect the canonical reject-this-log prose.
bool DetectLogDrop(const std::string& body) {
    const std::string lower = AsciiLower(body);
    const bool hasLogToken =
        Contains(lower, "log_trace") || Contains(lower, "log_debug") || Contains(lower, "log_info");
    if (!hasLogToken) {
        return false;
    }
    // Either of these signals "delete this log" intent.
    return Contains(lower, "remove this") || Contains(lower, "remove the") || Contains(lower, "drop this") ||
           Contains(lower, "delete this") || Contains(lower, "unused log") || Contains(lower, "for cleanliness") ||
           Contains(lower, "noisy log") || Contains(lower, "verbose log");
}

// Rule 10 — backward-compat shim / re-export / `// removed` comment.
// Detect the literal phrases CodeRabbit uses when suggesting we keep
// removed code as a stub.
bool DetectBackcompatShim(const std::string& body) {
    const std::string lower = AsciiLower(body);
    return Contains(lower, "backward compat") || Contains(lower, "backwards compat") ||
           Contains(lower, "back-compat") || Contains(lower, "// removed") || Contains(lower, "compatibility shim") ||
           Contains(lower, "for backward") || Contains(lower, "for backwards") || Contains(lower, "re-export") ||
           Contains(lower, "deprecation shim");
}

// Rule 11 — WHAT-the-code-does / PR-cite comment. CodeRabbit tags these
// with "add a comment" + a WHAT-style description. Conservative: require
// both the suggestion form AND a PR/task/fix mention.
bool DetectWhatComment(const std::string& body) {
    const std::string lower = AsciiLower(body);
    const bool suggestsComment = Contains(lower, "add a comment") || Contains(lower, "add a // comment") ||
                                 Contains(lower, "explanatory comment") || Contains(lower, "explain what") ||
                                 Contains(lower, "documentation comment");
    if (!suggestsComment) {
        return false;
    }
    // PR / task / fix citation that AGENTS.md § Comment discipline bans.
    return Contains(lower, "this pr") || Contains(lower, "this fix") || Contains(lower, "this task") ||
           Contains(lower, "what this") || Contains(lower, "what the code");
}

// Rule 12 — try/catch around code with no thrown exception, or validate
// internal-caller parameters. Detect the suggestion-block shape that wraps
// existing code in `try { ... } catch (...)`.
bool DetectDefensiveTryCatch(const std::string& body) {
    const std::vector<std::string> blocks = coderabbit_pure::ExtractSuggestionBlocks(body);
    const bool blockMatch = std::any_of(blocks.begin(), blocks.end(), [](const std::string& b) {
        return (Contains(b, "try {") && Contains(b, "catch (")) || (Contains(b, "try\n") && Contains(b, "catch"));
    });
    if (blockMatch) {
        return true;
    }
    // Or the prose form: "add a try/catch", "validate the parameter".
    const std::string lower = AsciiLower(body);
    if (Contains(lower, "add a try") || Contains(lower, "wrap in try") || Contains(lower, "add error handling")) {
        return true;
    }
    return false;
}

// Rule 13 — suggest splitting trivial-visual-only diff into separate PRs
// or running bucket-E on a `SmatchetTheme.cpp` / `Locales/*.json` swap.
bool DetectSplitTrivialVisualPr(const std::string& body) {
    const std::string lower = AsciiLower(body);
    const bool mentionsSplit = Contains(lower, "split this pr") || Contains(lower, "separate commit") ||
                               Contains(lower, "separate pr") || Contains(lower, "split into") ||
                               Contains(lower, "consider splitting");
    if (!mentionsSplit) {
        // bucket-E suggestion on visual-only files is also rule 13.
        const bool mentionsBucketE = Contains(lower, "bucket-e") || Contains(lower, "bucket e ") ||
                                     Contains(lower, "imgui test engine") || Contains(lower, "imgui-test-engine");
        if (!mentionsBucketE) {
            return false;
        }
    }
    return Contains(lower, "smatchettheme") || Contains(lower, "smatchet_theme") || Contains(lower, "locales/") ||
           Contains(lower, "locales\\");
}

// Rule 14 — touch a `*_DX12` CMake target / Unreal ThirdParty file.
// Path-conditional in the spec; we match on the body's path mentions.
bool DetectDx12OrUnrealThirdparty(const std::string& body) {
    const std::string lower = AsciiLower(body);
    if (Contains(lower, "_dx12")) {
        return true;
    }
    if (Contains(lower, "unrealplugins/smatchetimguiplugin/thirdparty/") ||
        Contains(lower, "unrealplugins\\smatchetimguiplugin\\thirdparty\\")) {
        return true;
    }
    return false;
}

// Rule 15 — new third-party dependency outside the FetchContent budget.
// Detect `FetchContent_Declare` mentions OR `find_package` of a name not
// in the documented set.
bool DetectNewThirdpartyDep(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    if (Contains(scan, "FetchContent_Declare")) {
        return true;
    }
    if (!Contains(scan, "find_package(")) {
        return false;
    }
    // Allowed dep names — case-insensitive. Each entry is a `find_package(`
    // prefix; an occurrence matches when its lowercased text begins with one
    // of these strings.
    const std::string lower = AsciiLower(scan);
    const std::vector<std::string> allowedDeps = {
        "find_package(nlohmann", "find_package(cpr",   "find_package(sqlitecpp", "find_package(httplib",
        "find_package(md4c",     "find_package(imgui", "find_package(glfw",      "find_package(lua",
        "find_package(sol2",     "find_package(ghc"};
    // Walk every `find_package(...)` occurrence and reject when any single
    // one is non-allowlisted. A short-circuit on the first allowed match
    // would let a mixed body (allowed + disallowed) slip through.
    const std::string opener = "find_package(";
    std::size_t pos = 0;
    while ((pos = lower.find(opener, pos)) != std::string::npos) {
        const std::size_t end = lower.find(')', pos);
        const std::string call = (end == std::string::npos) ? lower.substr(pos) : lower.substr(pos, end - pos + 1);
        const bool allowed = std::any_of(allowedDeps.begin(), allowedDeps.end(),
                                         [&call](const std::string& a) { return call.compare(0, a.size(), a) == 0; });
        if (!allowed) {
            return true;
        }
        pos = (end == std::string::npos) ? (pos + opener.size()) : (end + 1);
    }
    return false;
}

// Rule 16 — "drop const&, pass by value". The classic suggestion shape
// is `const std::string&` → `std::string`. Detect prose forms.
bool DetectDropConstRef(const std::string& body) {
    const std::string lower = AsciiLower(body);
    const bool mentionsPassByValue =
        Contains(lower, "pass by value") || Contains(lower, "pass-by-value") || Contains(lower, "compiler will elide");
    if (!mentionsPassByValue) {
        return false;
    }
    return Contains(lower, "const&") || Contains(lower, "const &") || Contains(lower, "const reference");
}

// Rule 17 — "use std::map for deterministic order" when insertion order
// does not matter (i.e. the suggestion is to migrate from
// std::unordered_map to std::map).
bool DetectStdMapForDeterminism(const std::string& body) {
    const std::string scan = CollectBodyForScan(body);
    const std::string lower = AsciiLower(scan);
    const bool suggestsMap = Contains(scan, "std::map<") && Contains(scan, "std::unordered_map");
    const bool determinismProse = Contains(lower, "deterministic") || Contains(lower, "ordered iteration") ||
                                  Contains(lower, "predictable order");
    return suggestsMap && determinismProse;
}

// Rule 18 — rename a public symbol. Detect "rename" + a symbol-like
// token (CamelCase or snake_case identifier with at least two segments).
// Very conservative — easy to miss; routes to mechanic + user sign-off
// anyway.
bool DetectUnauthorisedRename(const std::string& body) {
    const std::string lower = AsciiLower(body);
    if (!Contains(lower, "rename")) {
        return false;
    }
    // Look for `Rename A to B` / `rename FooBar to FooBaz` shape.
    if (Contains(lower, "rename ") && Contains(lower, " to ")) {
        return true;
    }
    if (Contains(lower, "renamed to ") || Contains(lower, "should be renamed")) {
        return true;
    }
    return false;
}

const OverrideRule kOverrideRules[] = {
    {1, "C++14 hard", &DetectCpp17PlusTokens},
    {2, "Dual-target — DX12 compiles those headers too", &DetectGlInHeader},
    {3, "Already PUBLIC on ImGuiLib", &DetectImguiWchar32Redef},
    {4, "Logger contract", &DetectLogToPrintf},
    {5, "Won't compile — must be obj[\"k\"] = v", &DetectJsonBraceListReassign},
    {6, "RAII rule (pillar 3 — Never crash)", &DetectRawNewDelete},
    {7, "Tracker invariant", &DetectCprBypass},
    {8, "Pillar 2 — UI never freezes (>100 ms ops must move to worker)", &DetectSyncOnUiFrame},
    {9, "Required by AGENTS.md § Project rules", &DetectLogDrop},
    {10, "Banned by CLAUDE.md — delete completely", &DetectBackcompatShim},
    {11, "Comment discipline (AGENTS.md § Comment discipline)", &DetectWhatComment},
    {12, "Don't add error handling for scenarios that can't happen", &DetectDefensiveTryCatch},
    {13, "AGENTS.md § Trivial-visual-only change envelope", &DetectSplitTrivialVisualPr},
    {14, "Unreal-only — EXCLUDE_FROM_ALL", &DetectDx12OrUnrealThirdparty},
    {15, "Out of dependency budget — escalate to architect", &DetectNewThirdpartyDep},
    {16, "Convention — explicit const& for non-trivial params", &DetectDropConstRef},
    {17, "Prefer std::unordered_map on hot paths", &DetectStdMapForDeterminism},
    {18, "Mechanical scope creep — flag for mechanic with explicit user sign-off", &DetectUnauthorisedRename},
};
constexpr std::size_t kOverrideRuleCount = sizeof(kOverrideRules) / sizeof(kOverrideRules[0]);
static_assert(kOverrideRuleCount == 18, "expected exactly 18 override rules");

// Marker prefix for reject-replies. The watcher's bot-filter should
// treat both `<!-- smatchet-handoff -->` and this marker as Smatchet-
// authored (avoid re-classifying our own replies).
const char* const kRejectReplyMarker = "<!-- smatchet-coderabbit-react -->";

bool HasRejectReplyMarker(const std::string& body) {
    if (body.empty()) {
        return false;
    }
    return body.find(kRejectReplyMarker) != std::string::npos;
}

} // namespace

CoderabbitCommentClassifier::CoderabbitCommentClassifier() : botLoginAllowList_({"coderabbitai[bot]"}) {
    LOG_TRACE("CoderabbitCommentClassifier::CoderabbitCommentClassifier enter (default allow-list)");
}

CoderabbitCommentClassifier::~CoderabbitCommentClassifier() = default;

void CoderabbitCommentClassifier::SetBotLoginAllowList(std::vector<std::string> allowList) {
    LOG_TRACE("CoderabbitCommentClassifier::SetBotLoginAllowList enter size=%zu", allowList.size());
    botLoginAllowList_ = std::move(allowList);
}

const std::vector<std::string>& CoderabbitCommentClassifier::GetBotLoginAllowList() const {
    LOG_TRACE("CoderabbitCommentClassifier::GetBotLoginAllowList enter size=%zu", botLoginAllowList_.size());
    return botLoginAllowList_;
}

void CoderabbitCommentClassifier::SetShortCircuitRejectEnabled(bool enabled) {
    LOG_TRACE("CoderabbitCommentClassifier::SetShortCircuitRejectEnabled enter enabled=%d", static_cast<int>(enabled));
    shortCircuitRejectEnabled_ = enabled;
}

bool CoderabbitCommentClassifier::GetShortCircuitRejectEnabled() const {
    LOG_TRACE("CoderabbitCommentClassifier::GetShortCircuitRejectEnabled enter value=%d",
              static_cast<int>(shortCircuitRejectEnabled_));
    return shortCircuitRejectEnabled_;
}

ClassificationResult CoderabbitCommentClassifier::Classify(const PostedComment& comment) const {
    LOG_TRACE("CoderabbitCommentClassifier::Classify enter id=%s author=%s body_bytes=%zu", comment.id.c_str(),
              comment.author.c_str(), comment.body.size());
    ClassificationResult out;

    // Step 1 — Smatchet's own markers (handoff or react-reply) skip
    // unconditionally. We never re-classify our own replies.
    if (coderabbit_pure::HasSmatchetHandoffMarker(comment.body) || HasRejectReplyMarker(comment.body)) {
        LOG_DEBUG("classifier verdict Skip(smatchet-marker) comment=%s comment_bytes=%zu", comment.id.c_str(),
                  comment.body.size());
        out.verdict = ClassifierVerdict::Skip;
        out.skipReason = "smatchet marker";
        return out;
    }

    // Step 2 — author allow-list. Non-bot authors (humans, other bots)
    // are not in scope for the classifier; the watcher's default path
    // handles them.
    if (!coderabbit_pure::IsBotLoginAllowed(comment.author, botLoginAllowList_)) {
        LOG_DEBUG("classifier verdict Skip(non-bot-author) comment=%s author=%s", comment.id.c_str(),
                  comment.author.c_str());
        out.verdict = ClassifierVerdict::Skip;
        out.skipReason = "non-bot author";
        return out;
    }

    // Step 3 — short-circuit-reject debugging knob. With it off, every
    // recognised bot comment dispatches to coderabbit-triage.
    if (!shortCircuitRejectEnabled_) {
        LOG_DEBUG("classifier verdict Dispatch(short-circuit-disabled) comment=%s", comment.id.c_str());
        out.verdict = ClassifierVerdict::Dispatch;
        out.dispatchTargetAgent = "coderabbit-triage";
        return out;
    }

    // Step 4 — walk the 18 override rules in numeric order. First match
    // wins.
    for (std::size_t i = 0; i < kOverrideRuleCount; ++i) {
        const OverrideRule& rule = kOverrideRules[i];
        if (rule.detector(comment.body)) {
            LOG_DEBUG("override rule %d matched: %s", rule.number, rule.shortReason);
            LOG_DEBUG("classifier verdict RejectShortCircuit comment_bytes=%zu rule=%d comment=%s", comment.body.size(),
                      rule.number, comment.id.c_str());
            LOG_DEBUG("CoderabbitCommentClassifier: rule #%d fired (%s) on comment %s", rule.number, rule.shortReason,
                      comment.id.c_str());
            out.verdict = ClassifierVerdict::RejectShortCircuit;
            out.rejectReplyBody = BuildRejectReplyBody(rule.number, rule.shortReason);
            std::ostringstream cite;
            cite << "override #" << rule.number << " — " << rule.shortReason;
            out.rejectRuleCitation = cite.str();
            return out;
        }
    }

    // Step 5 — no rule fired. Dispatch to coderabbit-triage for full
    // triage prose review.
    LOG_DEBUG("classifier verdict Dispatch(no-rule-fired) comment=%s comment_bytes=%zu", comment.id.c_str(),
              comment.body.size());
    LOG_TRACE("CoderabbitCommentClassifier: no override rule fired on comment %s; dispatching to coderabbit-triage",
              comment.id.c_str());
    out.verdict = ClassifierVerdict::Dispatch;
    out.dispatchTargetAgent = "coderabbit-triage";
    return out;
}

std::string CoderabbitCommentClassifier::BuildRejectReplyBody(int ruleNumber, const std::string& ruleShortReason) {
    LOG_TRACE("CoderabbitCommentClassifier::BuildRejectReplyBody enter rule=%d", ruleNumber);
    std::ostringstream body;
    body << kRejectReplyMarker << "\n\n";
    body << "Smatchet's CodeRabbit override classifier rejected this suggestion automatically.\n\n";
    body << "**Override rule #" << ruleNumber << "** — " << ruleShortReason << ".\n\n";
    body << "Rationale + the full 18-rule table live in `agents/coderabbit-triage.md` § \"Override rules — reject "
            "CodeRabbit suggestions that violate these\". If you believe this rejection is incorrect, reply with "
            "`/smatchet override` and a human reviewer will take a second look.\n";
    return body.str();
}

const char* CoderabbitCommentClassifier::RejectReplyMarker() {
    LOG_TRACE("CoderabbitCommentClassifier::RejectReplyMarker enter");
    return kRejectReplyMarker;
}

std::size_t CoderabbitCommentClassifier::OverrideRuleCount() {
    LOG_TRACE("CoderabbitCommentClassifier::OverrideRuleCount enter count=%zu", kOverrideRuleCount);
    return kOverrideRuleCount;
}

std::string CoderabbitCommentClassifier::LookupOverrideShortReason(int ruleNumber) {
    LOG_TRACE("CoderabbitCommentClassifier::LookupOverrideShortReason enter rule=%d", ruleNumber);
    for (std::size_t i = 0; i < kOverrideRuleCount; ++i) {
        if (kOverrideRules[i].number == ruleNumber) {
            return std::string(kOverrideRules[i].shortReason);
        }
    }
    LOG_WARN("CoderabbitCommentClassifier::LookupOverrideShortReason: unknown rule number %d", ruleNumber);
    return std::string();
}

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
