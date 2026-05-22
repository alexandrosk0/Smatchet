#ifndef SMATCHET_GITHUB_QUERY_FROM_JQL_H
#define SMATCHET_GITHUB_QUERY_FROM_JQL_H

#include <string>

// PR5 of docs/design/github-tracker-backend.md — pure-helper JQL → GitHub
// /search/issues `q=` translator. Pure logic, no I/O, no globals. Lives in
// its own TU so the doctest rig links it without pulling cpr/SQLite/ImGui
// (mirrors the LabelEditDiffPure / GitHubClientHelpers convention).
//
// Users keep authoring view queries in JQL syntax; this helper converts the
// supported subset to GitHub search-qualifier syntax at fetch time. Unknown
// terms are dropped with a Warning rather than failing translation — the
// caller decides whether to surface the warning to the UI.

namespace smatchet {
namespace github {

struct JqlToGitHubResult {
    /// The translated GitHub search query string (the value passed to `?q=`).
    /// Empty when input was empty or every supported term was dropped; in that
    /// case the caller typically falls back to a repo-scoped is:open listing.
    std::string Query;
    /// Non-fatal: human-readable description of unsupported / dropped JQL
    /// terms. Empty when the full input translated cleanly. Multiple warnings
    /// are concatenated with "; " separators.
    std::string Warning;
    /// True on successful translation (including the empty-input case).
    /// False only on genuine parse failure where Query is unusable.
    bool Ok = false;
    /// Populated when Ok == false.
    std::string Error;
};

/// Translate a Smatchet JQL view query to a GitHub /search/issues `q=` value.
///
/// `owner` / `repo` are optional context: when both are non-empty the result
/// query is prefixed with `repo:<owner>/<repo>`. The cross-repo path passes
/// empty strings and relies on JQL anchors to scope the search.
///
/// Supported JQL terms (operator + keyword are case-insensitive):
///   project = <KEY>            → ignored with warning
///   assignee = currentUser()   → assignee:@me
///   assignee = "<login>"       → assignee:<login>
///   status = "Open"            → is:open
///   status = "Closed"          → is:closed
///   status != "Closed"         → is:open
///   labels = "<label>"         → label:"<label>" (chained via space = AND)
///   text ~ "<phrase>"          → "<phrase>"
///   reporter = currentUser()   → author:@me
///   ORDER BY <field> <dir>     → ignored with warning
///   AND / OR connectors        → AND becomes space (GitHub default);
///                                OR is unsupported and emits a warning
///
/// Unsupported / unknown terms drop with a warning. Empty input → Ok=true,
/// Query empty (or just `repo:o/r` when owner+repo provided).
JqlToGitHubResult TranslateJqlToGitHubSearch(const std::string& jql, const std::string& owner,
                                             const std::string& repo);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_GITHUB_QUERY_FROM_JQL_H
