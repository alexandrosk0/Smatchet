#pragma once

// The clause editor behind the grid's facet chips (docs/plans/input-ux-w3-filter-without-jql.md): reads a view
// query into the clauses a chip can stand for, and adds, replaces or removes one clause without touching the
// bytes of any other. A view query is JQL on Jira, and a translated JQL subset on GitHub and Linear
// (GitHubQueryFromJql.h, LinearQueryFromJql.h). A Plane view query filters by key only, so it has no dialect.
// Not a JQL parser: a query it cannot read this way is "not chippable" and is kept as it is. Pure: no I/O,
// no ImGui.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace query_facets {

enum class FacetDialect : unsigned char { Jql, GitHubSubset, LinearSubset };
enum class FacetOp : unsigned char { Eq, NotEq, In, NotIn };

struct FacetClause {
    std::string FieldId; ///< the canonical facet id (CanonicalFacetField), not the typed spelling
    FacetOp Op = FacetOp::Eq;
    std::vector<std::string> Values; ///< unquoted
    bool CurrentUser = false;        ///< the value is currentUser(); in a list it stands beside Values
    std::size_t At = 0;              ///< byte span in the parsed query
    std::size_t Len = 0;
};

struct FacetParse {
    /// True when the top level is `<field> <op> <value>` clauses joined by AND only (in Jql a parenthesized
    /// group counts as one opaque clause). A trailing ORDER BY does not matter. On Linear two clauses on one
    /// field make it false: the translator keeps only the second.
    bool Chippable = false;
    /// Facet-field clauses the tracker applies as written. In Jql a clause with a bare EMPTY or NULL, or with a
    /// backslash escape other than \" \' and \\, is not one: writing it again would change it. Empty when not
    /// chippable.
    std::vector<FacetClause> Clauses;
    /// Every other clause as a (At, Len) byte span. Empty when not chippable.
    std::vector<std::pair<std::size_t, std::size_t>> OtherSpans;
    /// Subset dialects: facet fields named by a clause that is not a chip. A second clause from "+ Filter"
    /// would replace or contradict it, so the field cannot be added. Empty for Jql.
    std::vector<std::string> LockedFieldIds;
    /// Start of the top-level ORDER BY tail, npos when there is none. Set for a non-chippable query too.
    std::size_t OrderByAt = std::string::npos;
};

FacetParse ParseFacetClauses(const std::string& query, FacetDialect d, const std::vector<std::string>& facetFieldIds);

/// Adds one clause and changes no existing one: " AND <clause>" goes after the filter and before the ORDER BY
/// tail; an empty filter gets the clause alone. A Jql query that is not chippable has its filter wrapped first,
/// and only its filter: "(<filter>) AND <clause>", then the unchanged ORDER BY tail. The query is returned
/// unchanged when the clause is one the dialect cannot write (DialectSupports, ValueSupported, no value) or its
/// translator would not apply whole (a GitHub status other than Open or Closed), when a quote or a parenthesis
/// is unbalanced, and for a non-chippable query in a subset dialect (its translator
/// drops parentheses, so a wrap would change the meaning).
std::string AddFacetClause(const std::string& query, FacetDialect d, const FacetClause& clause);

/// Rewrites the one clause at [at, at + len), a FacetClause span from ParseFacetClauses on this same string,
/// and leaves every other byte as it is. A span outside the string, or a clause the dialect cannot write,
/// returns the query unchanged.
std::string ReplaceFacetClause(const std::string& query, FacetDialect d, std::size_t at, std::size_t len,
                               const FacetClause& clause);

/// Drops the clause at [at, at + len) and one adjacent AND. A span outside the string returns the query
/// unchanged.
std::string RemoveFacetClause(const std::string& query, std::size_t at, std::size_t len);

/// What a picker may write: true when the dialect's tracker takes this operator and value count on this facet
/// field. currentUser() counts as one value.
bool DialectSupports(FacetDialect d, const std::string& fieldId, FacetOp op, std::size_t valueCount);

/// The facet id a typed field name stands for in this dialect, or empty when it is not a facet field. Matches
/// without case and through the translator's own aliases: Linear state -> status, label -> labels,
/// creator -> reporter; GitHub label -> labels, author -> reporter; Jql type -> issuetype.
std::string CanonicalFacetField(FacetDialect d, const std::string& typedField);

/// True when the dialect writes one clause per value on this field, so several clauses on it are normal:
/// GitHubSubset "labels" only.
bool FieldRepeats(FacetDialect d, const std::string& fieldId);

/// False for a value the dialect cannot write safely: in a subset dialect an empty value or one holding a double
/// quote (their translators read no escapes), and a comma inside a Linear IN list (it is split there). A picker
/// draws such an option disabled, with the reason.
bool ValueSupported(FacetDialect d, const std::string& value, bool inList);

/// The facet field ids a dialect can filter by, in menu order. The ids are the names a query writes.
const std::vector<std::string>& FacetFieldIds(FacetDialect d);

/// What the dialect's translator will ignore in `query`, as its own warning text; empty for Jql or when nothing
/// is ignored. Runs TranslateJqlToGitHubSearch / TranslateJqlToLinearFilter on the query without its ORDER BY
/// tail, which both always report and which is not a filter.
std::string IgnoredClauseNote(const std::string& query, FacetDialect d);

} // namespace query_facets
