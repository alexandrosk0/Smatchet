#pragma once

// OmnibarInputClassifier — pure, header-only classifier for the global Chrome-
// omnibox search bar (jql-omnibox plan, Stream B slice 2c). Given what the user
// typed plus the focused pane's backend, it decides which of three actions Enter
// should drive:
//   * TicketKey    — a bare issue key for the active backend ("PROJ-123",
//                    "owner/repo#42"); Enter jumps to that ticket.
//   * Jql          — a structured filter query (operators / grouping / field:value
//                    / ORDER BY); Enter replaces the focused pane's view query.
//   * TitleSearch  — plain words; Enter filters the focused pane's loaded rows.
//
// No ImGui / no session state / no I/O — bucket-A testable in isolation
// (tests/Core/OmnibarInputClassifier.test.cpp). Ticket-key shape detection reuses
// the single-source backend helpers (ExtractIssueKeyPrefix for Jira keys,
// ParseGitHubIssueKey for GitHub keys) so the omnibar never re-implements key
// validation. Plane ids are project-scoped UUIDs with no typeable key shape, so
// the Plane backend never yields TicketKey — it degrades to Jql / TitleSearch.

#include "GitHubClientHelpers.h"
#include "ProjectResolver.h"
#include "StringUtil.h"

#include <cctype>
#include <string>

namespace smatchet {
namespace omnibar {

/// The three omnibar actions Enter can drive. Defaults to TitleSearch — the
/// safest fallback (a plain substring filter never mutates a saved view or opens
/// a browser tab).
enum class OmnibarInputKind { Jql, TicketKey, TitleSearch };

/// Which backend the focused pane is bound to. Mirrors the three shipped
/// backends (DefaultTrackerBackendFactory); only TicketKey detection branches on
/// it. Unknown / empty keys default to Jira to match the factory's Jira-default.
enum class OmnibarBackend { Jira, Plane, GitHub };

/// Map a `GridPane::backendKey` (ConfigManager::NormalizeViewsBackendKey output:
/// "Jira" / "Plane" / "GitHub") to the enum, case-insensitively. Unknown / empty
/// → Jira (matches DefaultTrackerBackendFactory's fallback so a stale key still
/// classifies sensibly). Pure.
inline OmnibarBackend OmnibarBackendFromKey(const std::string& backendKey) {
    const std::string lower = ToLowerAsciiCopy(backendKey);
    if (lower == "plane") {
        return OmnibarBackend::Plane;
    }
    if (lower == "github") {
        return OmnibarBackend::GitHub;
    }
    return OmnibarBackend::Jira;
}

/// True when `input` is a bare, whole-string issue key for `backend`. Reuses the
/// backend's own validator (no re-implementation): Jira via ExtractIssueKeyPrefix
/// (which enforces `^[A-Za-z][A-Za-z0-9_]*-[0-9]+$` and keeps UUIDs out), GitHub
/// via ParseGitHubIssueKey (strict `owner/repo#N`). Plane has no typeable key
/// shape → always false. A multi-token query like "PROJ-123 AND status = Open"
/// is NOT a bare key (the whole-string validators reject the spaces), so it falls
/// through to the structured-query / title heuristics. Pure.
inline bool LooksLikeTicketKey(const std::string& input, OmnibarBackend backend) {
    switch (backend) {
    case OmnibarBackend::Jira:
        return !smatchet::ExtractIssueKeyPrefix(input).empty();
    case OmnibarBackend::GitHub: {
        smatchet::github::ParsedIssueKey parsed;
        return smatchet::github::ParseGitHubIssueKey(input, parsed);
    }
    case OmnibarBackend::Plane:
    default:
        return false;
    }
}

/// True when `input` looks like a structured filter query rather than a plain
/// title phrase. Conservative heuristic (favours TitleSearch on ambiguity):
///   1. any comparison / grouping char — `= ~ < > ! ( )` — unambiguously marks a
///      filter clause (every JQL / Plane comparison carries one);
///   2. a `field:value` colon token (GitHub `is:open`, Plane `state:done`):
///      a colon with an alnum immediately before and a non-space immediately
///      after — excludes free-text colons like "fix: crash on startup";
///   3. a case-insensitive "order by" ordering clause (no operator char of its
///      own, but never appears in a real ticket title).
/// A reserved-word-only query (e.g. bare "project IS EMPTY") carries no operator
/// char and degrades to TitleSearch by design — accepted in v1; refine later.
/// Pure.
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
    return ToLowerAsciiCopy(input).find("order by") != std::string::npos;
}

/// Classify trimmed `raw` for `backend`. Precedence: bare ticket key → structured
/// query → title search. Empty / whitespace-only input → TitleSearch (a no-op
/// filter, never a destructive view rewrite). Pure — the omnibar calls this both
/// every frame (to pick the leading mode glyph) and on Enter (to route the
/// action).
inline OmnibarInputKind ClassifyOmnibarInput(const std::string& raw, OmnibarBackend backend) {
    const std::string input = TrimCopyAsciiWhitespace(raw);
    if (input.empty()) {
        return OmnibarInputKind::TitleSearch;
    }
    if (LooksLikeTicketKey(input, backend)) {
        return OmnibarInputKind::TicketKey;
    }
    if (LooksLikeStructuredQuery(input)) {
        return OmnibarInputKind::Jql;
    }
    return OmnibarInputKind::TitleSearch;
}

} // namespace omnibar
} // namespace smatchet
