#include <doctest/doctest.h>

#include "ProjectResolver.h"
#include "CachedTicketTypes.h"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// Per-project Jira component dropdowns (PR #672, additive on the multiselect branch).
// Pure, curl-free coverage of the three building blocks the feature relies on:
//   1. issue-key-prefix extraction (the project key for a row),
//   2. distinct-project extraction over an active-ticket vector,
//   3. per-project option-map lookup semantics.

TEST_CASE("ExtractIssueKeyPrefix returns the project key for a Jira key") {
    using smatchet::ExtractIssueKeyPrefix;

    CHECK(ExtractIssueKeyPrefix("PROJ-123") == "PROJ");
    CHECK(ExtractIssueKeyPrefix("ABC-1") == "ABC");
    CHECK(ExtractIssueKeyPrefix("A1B2-9") == "A1B2");
    CHECK(ExtractIssueKeyPrefix("WITH_UNDERSCORE-5") == "WITH_UNDERSCORE");
}

TEST_CASE("ExtractIssueKeyPrefix returns empty for non-key strings") {
    using smatchet::ExtractIssueKeyPrefix;

    CHECK(ExtractIssueKeyPrefix("-1") == "");    // leading dash
    CHECK(ExtractIssueKeyPrefix("x y-1") == ""); // space before dash
    CHECK(ExtractIssueKeyPrefix("PROJ") == "");  // no dash at all
    CHECK(ExtractIssueKeyPrefix("") == "");      // empty
    CHECK(ExtractIssueKeyPrefix("550e8400-e29b-41d4-a716-446655440000") ==
          ""); // UUID: leading-digit prefix is not a Jira key (must start with a letter)
    CHECK(ExtractIssueKeyPrefix("123-456") == "");  // digit-leading prefix rejected
    CHECK(ExtractIssueKeyPrefix("PROJ-abc") == ""); // non-numeric issue suffix rejected
}

TEST_CASE("ExtractIssueKeyPrefix rejects punctuation before the dash") {
    using smatchet::ExtractIssueKeyPrefix;

    // A UUID segment that contains a non-alphanumeric char before the first dash yields "".
    CHECK(ExtractIssueKeyPrefix("a.b-1") == "");
    CHECK(ExtractIssueKeyPrefix("foo/bar-2") == "");
}

namespace {

// Mirrors the distinct-prefix collection the warm path performs over ActiveTickets.
std::vector<std::string> DistinctProjectKeys(const std::vector<CachedTicket>& tickets) {
    std::vector<std::string> keys;
    std::set<std::string> seen;
    for (const auto& ticket : tickets) {
        if (ticket.id.empty()) {
            continue;
        }
        const std::string key = smatchet::ExtractIssueKeyPrefix(ticket.id);
        if (!key.empty() && seen.insert(key).second) {
            keys.push_back(key);
        }
    }
    return keys;
}

} // namespace

TEST_CASE("Distinct project-key extraction over a ticket vector yields the unique prefix set") {
    std::vector<CachedTicket> tickets;
    CachedTicket a;
    a.id = "PROJ-1";
    CachedTicket b;
    b.id = "PROJ-2"; // duplicate project
    CachedTicket c;
    c.id = "OTHER-7";
    CachedTicket d;
    d.id = "-9"; // non-key — contributes nothing
    CachedTicket e;
    e.id = ""; // empty — skipped
    tickets.push_back(a);
    tickets.push_back(b);
    tickets.push_back(c);
    tickets.push_back(d);
    tickets.push_back(e);

    const std::vector<std::string> keys = DistinctProjectKeys(tickets);
    CHECK(keys.size() == 2);
    const std::set<std::string> asSet(keys.begin(), keys.end());
    CHECK(asSet.count("PROJ") == 1);
    CHECK(asSet.count("OTHER") == 1);
}

TEST_CASE("Per-project option map returns the right list and empty for unknown keys") {
    // Models AppController::projectComponentOptions_ lookup (GetComponentOptionsForProject):
    // a hit returns the project's own list; a miss returns an empty vector.
    std::unordered_map<std::string, std::vector<std::string>> projectComponentOptions;
    projectComponentOptions["PROJ"] = {"Backend", "Frontend"};
    projectComponentOptions["OTHER"] = {"Infra"};

    const auto lookup = [&](const std::string& key) -> std::vector<std::string> {
        const auto it = projectComponentOptions.find(key);
        if (it == projectComponentOptions.end()) {
            return std::vector<std::string>();
        }
        return it->second;
    };

    CHECK(lookup("PROJ") == std::vector<std::string>({"Backend", "Frontend"}));
    CHECK(lookup("OTHER") == std::vector<std::string>({"Infra"}));
    CHECK(lookup("MISSING").empty());
}
