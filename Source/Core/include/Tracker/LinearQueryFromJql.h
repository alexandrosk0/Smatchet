#ifndef SMATCHET_LINEAR_QUERY_FROM_JQL_H
#define SMATCHET_LINEAR_QUERY_FROM_JQL_H

// Pure-helper JQL -> Linear GraphQL `IssueFilter` translator (mirrors the
// GitHubQueryFromJql convention: pure logic, no I/O, its own TU so the doctest
// rig links it without cpr/SQLite/ImGui). Users keep authoring view queries in
// JQL syntax; this converts the SAFE subset to a Linear filter at fetch time.
// It is deliberately conservative — only the clauses below translate; anything
// else (OR, parentheses, unknown fields) drops with a Warning rather than
// silently broadening the result set.

#include <nlohmann/json.hpp>

#include <string>

namespace smatchet {
namespace linear {

struct JqlToLinearResult {
    /// The Linear `IssueFilter` object passed as the `filter` GraphQL variable.
    /// An empty object means "no filter" (the caller lists the team's issues).
    nlohmann::json Filter = nlohmann::json::object();
    /// Free-text term for Linear's `issueSearch` query, set by a `text ~ "..."`
    /// clause. Empty when the query carried no text term.
    std::string SearchTerm;
    /// Non-fatal: "; "-joined description of unsupported / dropped clauses.
    std::string Warning;
    /// True on successful translation (including empty input).
    bool Ok = false;
    /// Populated only when Ok == false.
    std::string Error;

    bool HasFilter() const { return Filter.is_object() && !Filter.empty(); }
};

/// Translate a Smatchet JQL view query into a Linear `IssueFilter`.
/// Field names + operators are case-insensitive. Supported clauses (joined by
/// AND; the filter keys are implicitly AND-ed):
///   assignee = currentUser()      -> { assignee: { isMe: { eq: true } } }
///   assignee = "<name>"           -> { assignee: { name: { containsIgnoreCase } } }
///   status = "X" / status in (..) -> { state: { name: { eq | in } } }
///   status != "X"                 -> { state: { name: { neq } } }
///   labels = "X" / labels in (..) -> { labels: { name: { eq | in } } }
///   priority = High|Urgent|Medium|Low|"No priority" -> { priority: { eq: <0-4> } }
///   project = "X"                 -> { project: { name: { eq } } }
///   team = "X"                    -> { team: { key: { eq } } }
///   text ~ "phrase"               -> result.SearchTerm = "phrase"
/// Unsupported (warned + dropped): OR / parentheses grouping, ORDER BY, unknown
/// fields, and any operator a field doesn't support. Empty input -> Ok=true,
/// empty Filter.
JqlToLinearResult TranslateJqlToLinearFilter(const std::string& jql);

} // namespace linear
} // namespace smatchet

#endif // SMATCHET_LINEAR_QUERY_FROM_JQL_H
