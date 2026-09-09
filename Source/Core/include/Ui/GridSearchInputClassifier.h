#pragma once

// GridSearchInputClassifier — pure, header-only classifier for the grid header's
// search box (formerly the global omnibox; the two search inputs were merged into
// the per-pane box, see docs/plans). Given what the user typed plus the pane's
// backend, it decides which of three actions Enter should drive:
//   * TicketKey    — a bare issue key for the active backend ("PROJ-123",
//                    "owner/repo#42"); Enter jumps to that ticket.
//   * Jql          — a structured filter query (operators / grouping / field:value
//                    / ORDER BY); Enter replaces the pane's view query.
//   * TitleSearch  — plain words; Enter (and every keystroke) filters the pane's
//                    loaded rows.

// The classification ALSO gates the live row filter (see AppliesAsRowFilter): plain
// words and a ticket key are matched against the loaded rows as they are typed, a
// structured query never is. A half-typed query — or a query still sitting in the
// box after its Enter re-ran the view — must not be matched as a substring against
// the rows it just fetched, which would blank the grid.

// No ImGui / no session state / no I/O — bucket-A testable in isolation
// (tests/Core/GridSearchInputClassifier.test.cpp). Ticket-key shape detection reuses
// the single-source backend helpers (ExtractIssueKeyPrefix for Jira keys,
// ParseGitHubIssueKey for GitHub keys) so the search box never re-implements key
// validation. Plane ids are project-scoped UUIDs with no typeable key shape, so
// the Plane backend never yields TicketKey — it degrades to Jql / TitleSearch.

#include "GitHubClientHelpers.h"
#include "ProjectResolver.h"
#include "StringUtil.h"

#include <cctype>
#include <string>

namespace smatchet {
namespace gridsearch {

/// The three actions the grid search box's Enter can drive. Defaults to TitleSearch
/// — the safest fallback (a plain substring filter never mutates a saved view or
/// opens a browser tab).
enum class GridSearchInputKind { Jql, TicketKey, TitleSearch };

/// Which backend the pane is bound to. Mirrors the three shipped backends
/// (DefaultTrackerBackendFactory); only TicketKey detection branches on it. Unknown
/// / empty keys default to Jira to match the factory's Jira-default.
enum class GridSearchBackend { Jira, Plane, GitHub };

/// Map a `GridPane::backendKey` (ConfigManager::NormalizeViewsBackendKey output:
/// "Jira" / "Plane" / "GitHub") to the enum, case-insensitively. Unknown / empty
/// → Jira (matches DefaultTrackerBackendFactory's fallback so a stale key still
/// classifies sensibly). Pure.
inline GridSearchBackend GridSearchBackendFromKey(const std::string& backendKey) {
    const std::string lower = ToLowerAsciiCopy(backendKey);
    if (lower == "plane") {
        return GridSearchBackend::Plane;
    }
    if (lower == "github") {
        return GridSearchBackend::GitHub;
    }
    return GridSearchBackend::Jira;
}

/// True when `input` is a bare, whole-string issue key for `backend`. Reuses the
/// backend's own validator (no re-implementation): Jira via ExtractIssueKeyPrefix
/// (which enforces `^[A-Za-z][A-Za-z0-9_]*-[0-9]+$` and keeps UUIDs out), GitHub
/// via ParseGitHubIssueKey (strict `owner/repo#N`). Plane has no typeable key
/// shape → always false. A multi-token query like "PROJ-123 AND status = Open"
/// is NOT a bare key (the whole-string validators reject the spaces), so it falls
/// through to the structured-query / title heuristics. Pure.
inline bool LooksLikeTicketKey(const std::string& input, GridSearchBackend backend) {
    switch (backend) {
    case GridSearchBackend::Jira:
        return !smatchet::ExtractIssueKeyPrefix(input).empty();
    case GridSearchBackend::GitHub: {
        smatchet::github::ParsedIssueKey parsed;
        return smatchet::github::ParseGitHubIssueKey(input, parsed);
    }
    case GridSearchBackend::Plane:
    default:
        return false;
    }
}

/// True when `haystack` contains `phrase` as a whole-word span: matched
/// case-SENSITIVELY, with a non-alphanumeric boundary (or the string edge) on
/// both sides. The boundary check is what keeps "THIS EMPTY grid" from matching
/// "IS EMPTY" on the "TH|IS EMPTY" split. Pure.
inline bool ContainsKeywordSpan(const std::string& haystack, const std::string& phrase) {
    if (phrase.empty() || haystack.size() < phrase.size()) {
        return false;
    }
    for (std::size_t at = haystack.find(phrase); at != std::string::npos; at = haystack.find(phrase, at + 1)) {
        const bool leftOk = at == 0 || std::isalnum(static_cast<unsigned char>(haystack[at - 1])) == 0;
        const std::size_t after = at + phrase.size();
        const bool rightOk = after >= haystack.size() || std::isalnum(static_cast<unsigned char>(haystack[after])) == 0;
        if (leftOk && rightOk) {
            return true;
        }
    }
    return false;
}

/// True when `input` looks like a structured filter query rather than a plain
/// title phrase. Conservative heuristic (favours TitleSearch on ambiguity):
///   1. any comparison / grouping char — `= ~ < > ! ( )` — unambiguously marks a
///      filter clause (every JQL / Plane comparison carries one);
///   2. a `field:value` colon token (GitHub `is:open`, Plane `state:done`):
///      a colon with an alnum immediately before and a non-space immediately
///      after — excludes free-text colons like "fix: crash on startup";
///   3. a case-insensitive "order by" ordering clause (no operator char of its
///      own, but never appears in a real ticket title);
///   4. JQL's empty/null predicates — `IS EMPTY` / `IS NOT EMPTY` / `IS NULL` /
///      `IS NOT NULL` — which carry no operator char of their own.
/// Rule 4 is matched UPPERCASE-only, deliberately. Lowercase "is empty" is a
/// perfectly ordinary ticket title ("cart is empty after reload"), and no
/// heuristic can separate that from a lowercase `project is empty` without
/// knowing which words are field names. Case is the one signal that does
/// separate them — JQL keywords are written uppercase by convention and in
/// every Jira example — so the uppercase form is caught and the lowercase form
/// keeps this classifier's documented TitleSearch bias. Pure.
inline bool LooksLikeStructuredQuery(const std::string& input) {
    for (char ch : input) {
        if (ch == '=' || ch == '~' || ch == '<' || ch == '>' || ch == '!' || ch == '(' || ch == ')') {
            return true;
        }
    }
    for (std::size_t i = 1; i + 1 < input.size(); ++i) {
        if (input[i] == ':' && std::isalnum(static_cast<unsigned char>(input[i - 1])) != 0 &&
            std::isspace(static_cast<unsigned char>(input[i + 1])) == 0) {
            return true;
        }
    }
    if (ContainsKeywordSpan(input, "IS EMPTY") || ContainsKeywordSpan(input, "IS NOT EMPTY") ||
        ContainsKeywordSpan(input, "IS NULL") || ContainsKeywordSpan(input, "IS NOT NULL")) {
        return true;
    }
    return ToLowerAsciiCopy(input).find("order by") != std::string::npos;
}

/// Classify trimmed `raw` for `backend`. Precedence: bare ticket key → structured
/// query → title search. Empty / whitespace-only input → TitleSearch (a no-op
/// filter, never a destructive view rewrite). Pure — the search box calls this to
/// pick its leading mode glyph, to gate the live row filter, and on Enter to route
/// the action.
inline GridSearchInputKind ClassifyGridSearchInput(const std::string& raw, GridSearchBackend backend) {
    const std::string input = TrimCopyAsciiWhitespace(raw);
    if (input.empty()) {
        return GridSearchInputKind::TitleSearch;
    }
    if (LooksLikeTicketKey(input, backend)) {
        return GridSearchInputKind::TicketKey;
    }
    if (LooksLikeStructuredQuery(input)) {
        return GridSearchInputKind::Jql;
    }
    return GridSearchInputKind::TitleSearch;
}

/// True when text of this kind filters the pane's LOADED rows as it is typed. Plain title
/// text does, and so does a ticket key — typing one narrows the grid to that row, which is
/// what the box did before the key/query modes arrived, and its Enter additionally jumps.
/// A structured query does NOT: its text addresses the backend, not row content, and it
/// stays in the box after its own Enter re-runs the view — matching it as a substring would
/// hide every row that Enter just fetched. Pure.
inline bool AppliesAsRowFilter(GridSearchInputKind kind) { return kind != GridSearchInputKind::Jql; }

} // namespace gridsearch
} // namespace smatchet
