#include <doctest/doctest.h>

// Coverage for the JQL autocomplete core extracted from JqlSuggestEngine.cpp
// (TEST_COVERAGE_GAP_MAP.md Tier 1 #5). The engine parses the filter buffer on the UI
// thread on every keystroke, so the contract under test is: correct suggest-mode
// resolution per token context, family-gated function suggestions, non-system user
// filtering, JQL-escape-safe inserts, and termination/sane output on any buffer.
// The family/function and user-catalog cases mirror the V21/V22 manual smokes in
// backlog/MANUAL_TEST_QUEUE.md so those flows are pinned without a keyboard.
#include "Tracker/JqlSuggestEnginePure.h"

#include "Tracker/TrackerFieldSchema.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {

TrackerField MakeField(const std::string& id, const std::string& name, TrackerFieldFamily family) {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    f.Family = family;
    return f;
}

std::vector<TrackerField> CatalogFields() {
    std::vector<TrackerField> fields;
    fields.push_back(MakeField("assignee", "Assignee", TrackerFieldFamily::UserSingle));
    fields.push_back(MakeField("created", "Created", TrackerFieldFamily::Date));
    fields.push_back(MakeField("summary", "Summary", TrackerFieldFamily::Text));
    TrackerField fixVersion = MakeField("fixVersion", "Fix Version", TrackerFieldFamily::SelectMulti);
    fixVersion.Type = "version";
    fields.push_back(fixVersion);
    fields.push_back(MakeField("sprint", "Sprint", TrackerFieldFamily::Sprint));
    TrackerField status = MakeField("status", "Status", TrackerFieldFamily::Status);
    TrackerFieldOption todo;
    todo.Id = "1";
    todo.Value = "To Do";
    TrackerFieldOption inProgress;
    inProgress.Id = "2";
    inProgress.Value = "In Progress";
    status.AllowedValueOptions.push_back(todo);
    status.AllowedValueOptions.push_back(inProgress);
    fields.push_back(status);
    return fields;
}

TrackerUser MakeUser(const std::string& accountId, const std::string& displayName, const std::string& email,
                     const std::string& accountType, bool active) {
    TrackerUser u;
    u.AccountId = accountId;
    u.DisplayName = displayName;
    u.EmailAddress = email;
    u.AccountType = accountType;
    u.Active = active;
    return u;
}

std::vector<TrackerUser> CatalogUsers() {
    std::vector<TrackerUser> users;
    users.push_back(MakeUser("acc-1", "Alice Smith", "alice@example.com", "atlassian", true));
    users.push_back(MakeUser("acc-2", "App Bot", "", "app", true));
    users.push_back(MakeUser("acc-3", "Carl Customer", "carl@portal.example", "customer", true));
    users.push_back(MakeUser("acc-4", "Alan Inactive", "", "atlassian", false));
    // Empty AccountType = older Jira API / non-Jira backend — must be included.
    users.push_back(MakeUser("acc-5", "Ann O'Nymous", "ann@example.com", "", true));
    // Email-prefix match: display name shares no prefix with the email local part.
    users.push_back(MakeUser("acc-6", "Zoe Zag", "zebra@example.com", "atlassian", true));
    return users;
}

// Cursor at end of `q`, no selection.
QuerySuggestBuild Run(const std::string& q, QuerySuggestMeta* meta = nullptr) {
    QuerySuggestBuild out;
    const std::vector<TrackerField> fields = CatalogFields();
    const std::vector<TrackerUser> users = CatalogUsers();
    const int len = static_cast<int>(q.size());
    JqlSuggestEnginePure::BuildJqlSuggestions(q.c_str(), len, len, len, len, fields, users, out, meta);
    return out;
}

bool HasInsert(const QuerySuggestBuild& b, const std::string& insert) {
    return std::any_of(b.Items.begin(), b.Items.end(), [&](const QuerySuggestion& s) { return s.Insert == insert; });
}

bool HasLabel(const QuerySuggestBuild& b, const std::string& label) {
    return std::any_of(b.Items.begin(), b.Items.end(), [&](const QuerySuggestion& s) { return s.Label == label; });
}

} // namespace

TEST_CASE("user-field value token surfaces user functions, gated by prefix") {
    const QuerySuggestBuild cu = Run("assignee = cu");
    CHECK(HasInsert(cu, "currentUser()"));
    CHECK(!HasInsert(cu, "now()"));

    // membersOf inserts with the caret sentinel between the quotes (post-insert caret).
    const QuerySuggestBuild memb = Run("assignee = memb");
    CHECK(HasLabel(memb, "membersOf(\"\xE2\x80\xA6\")"));
    CHECK(HasInsert(memb, "membersOf(\"\x7F\")"));
}

TEST_CASE("date-field value token surfaces date functions only") {
    const QuerySuggestBuild st = Run("created > st");
    CHECK(HasInsert(st, "startOfDay()"));
    CHECK(HasInsert(st, "startOfWeek()"));
    CHECK(HasInsert(st, "startOfMonth()"));
    CHECK(HasInsert(st, "startOfYear()"));
    CHECK(!HasInsert(st, "endOfDay()"));
    CHECK(!HasInsert(st, "currentUser()"));

    CHECK(HasInsert(Run("created < no"), "now()"));
}

TEST_CASE("version and sprint fields surface their function families") {
    CHECK(HasInsert(Run("fixVersion in un"), "unreleasedVersions()"));
    CHECK(HasInsert(Run("sprint in op"), "openSprints()"));
}

TEST_CASE("text field value token surfaces no function suggestions") { CHECK(Run("summary = cu").Items.empty()); }

TEST_CASE("user catalog: non-system users suggested by name/email prefix, quoted display-name insert") {
    const QuerySuggestBuild al = Run("assignee = Al");
    CHECK(HasLabel(al, "Alice Smith (alice@example.com)"));
    CHECK(HasInsert(al, "\"Alice Smith\""));
    // Inactive users never appear even on a name match.
    CHECK(!HasLabel(al, "Alan Inactive"));

    // app + customer account types are suppressed; empty account type is included.
    const QuerySuggestBuild a = Run("assignee = A");
    CHECK(HasInsert(a, "\"Ann O'Nymous\""));
    CHECK(!HasInsert(a, "\"App Bot\""));
    CHECK(!HasInsert(a, "\"Carl Customer\""));

    // Email prefix matches too ('@' ends the identifier run, so the local part is the prefix).
    const QuerySuggestBuild zeb = Run("assignee = zeb");
    CHECK(HasInsert(zeb, "\"Zoe Zag\""));
    CHECK(HasLabel(zeb, "Zoe Zag (zebra@example.com)"));
}

TEST_CASE("empty value prefix skips the user-catalog dump but keeps functions and meta") {
    QuerySuggestMeta meta;
    const QuerySuggestBuild b = Run("assignee = ", &meta);
    CHECK(!HasInsert(b, "\"Alice Smith\""));
    CHECK(HasInsert(b, "currentUser()"));
    CHECK(meta.UserValueToken);
    CHECK(meta.UserSearchPrefix.empty());

    QuerySuggestMeta metaTyped;
    Run("assignee = Al", &metaTyped);
    CHECK(metaTyped.UserValueToken);
    CHECK(metaTyped.UserSearchPrefix == "Al");

    QuerySuggestMeta metaNonUser;
    Run("created > st", &metaNonUser);
    CHECK(!metaNonUser.UserValueToken);
}

TEST_CASE("field mode at buffer start lists the catalog plus clause keywords") {
    const QuerySuggestBuild b = Run("");
    CHECK(HasInsert(b, "assignee"));
    CHECK(HasInsert(b, "status"));
    CHECK(HasInsert(b, "NOT"));
    CHECK(HasLabel(b, "Assignee (assignee)"));
}

TEST_CASE("operator mode after a recognized field name") {
    const QuerySuggestBuild b = Run("assignee ");
    CHECK(HasInsert(b, "="));
    CHECK(HasInsert(b, "!="));
    CHECK(HasInsert(b, "IN"));
    CHECK(HasInsert(b, "WAS"));
    CHECK(!HasInsert(b, "assignee"));
}

TEST_CASE("allowed-value options quote values that need it") {
    const QuerySuggestBuild b = Run("status = ");
    CHECK(HasInsert(b, "\"To Do\""));
    CHECK(HasInsert(b, "\"In Progress\""));
    // Option ids are offered alongside, labelled with the display value.
    CHECK(HasInsert(b, "1"));
    CHECK(HasLabel(b, "1 (To Do)"));
}

TEST_CASE("ORDER BY chain: keyword, order-field, then sort direction") {
    CHECK(HasInsert(Run("order "), "BY"));
    const QuerySuggestBuild orderField = Run("summary ~ x order by ");
    CHECK(HasInsert(orderField, "created"));
    const QuerySuggestBuild dir = Run("summary ~ x order by created ");
    CHECK(HasInsert(dir, "ASC"));
    CHECK(HasInsert(dir, "DESC"));
}

TEST_CASE("IS operand and logical connectors") {
    const QuerySuggestBuild is = Run("assignee is ");
    CHECK(HasInsert(is, "EMPTY"));
    CHECK(HasInsert(is, "NULL"));

    const QuerySuggestBuild logical = Run("assignee = currentUser() a");
    CHECK(HasInsert(logical, "AND"));
    CHECK(!HasInsert(logical, "OR"));
}

TEST_CASE("open string literal: replace span covers the in-string prefix") {
    const std::string q = "summary ~ \"hel";
    const QuerySuggestBuild b = Run(q);
    CHECK(b.ReplaceStart == 11);
    CHECK(b.ReplaceEnd == static_cast<int>(q.size()));
}

TEST_CASE("active selection wins the replace span") {
    const std::string q = "assignee = foo";
    QuerySuggestBuild out;
    const std::vector<TrackerField> fields = CatalogFields();
    const std::vector<TrackerUser> users = CatalogUsers();
    const int len = static_cast<int>(q.size());
    JqlSuggestEnginePure::BuildJqlSuggestions(q.c_str(), len, len, 11, 14, fields, users, out, nullptr);
    CHECK(out.ReplaceStart == 11);
    CHECK(out.ReplaceEnd == 14);
}

TEST_CASE("null buffer and out-of-range cursor degrade to empty output") {
    QuerySuggestBuild out;
    const std::vector<TrackerField> fields = CatalogFields();
    const std::vector<TrackerUser> users = CatalogUsers();
    JqlSuggestEnginePure::BuildJqlSuggestions(nullptr, 0, 5, -3, 99, fields, users, out, nullptr);
    CHECK(out.Items.empty());
    const std::string q = "assignee";
    JqlSuggestEnginePure::BuildJqlSuggestions(q.c_str(), static_cast<int>(q.size()), 999, -1, 999, fields, users, out,
                                              nullptr);
    CHECK(out.ReplaceEnd <= static_cast<int>(q.size()));
}

TEST_CASE("tracker-supplied display names are JQL-escaped in user inserts") {
    std::vector<TrackerUser> users;
    users.push_back(MakeUser("acc-9", "Evil \"Quote\\Back", "", "atlassian", true));
    const std::vector<TrackerField> fields = CatalogFields();
    const std::string q = "assignee = Ev";
    const int len = static_cast<int>(q.size());
    QuerySuggestBuild out;
    JqlSuggestEnginePure::BuildJqlSuggestions(q.c_str(), len, len, len, len, fields, users, out, nullptr);
    // A `"` or `\` in the display name is escaped, never allowed to break out of the literal.
    CHECK(HasInsert(out, "\"Evil \\\"Quote\\\\Back\""));
}

TEST_CASE("output is label-sorted and capped at 80 suggestions") {
    std::vector<TrackerField> many;
    for (int i = 0; i < 100; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "field%03d", i);
        many.push_back(MakeField(id, "", TrackerFieldFamily::Text));
    }
    const std::vector<TrackerUser> users;
    QuerySuggestBuild out;
    JqlSuggestEnginePure::BuildJqlSuggestions("", 0, 0, 0, 0, many, users, out, nullptr);
    CHECK(out.Items.size() == 80);
    const bool sorted =
        std::is_sorted(out.Items.begin(), out.Items.end(), [](const QuerySuggestion& a, const QuerySuggestion& b) {
            std::string la = a.Label;
            std::string lb = b.Label;
            std::transform(la.begin(), la.end(), la.begin(), ::tolower);
            std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
            return la < lb || (la == lb && a.Label.size() < b.Label.size());
        });
    CHECK(sorted);
}
