#include <doctest/doctest.h>

#include "JqlSuggestEnginePure.h"

#include <algorithm>
#include <string>
#include <vector>

// The buffer under the cursor is raw user keystrokes and the field/user catalogs are
// tracker-server-sourced — both untrusted. The engine runs on every omnibox keystroke
// (the highest-frequency untrusted-input path in the app), so these cases pin the
// tokenizer, the mode resolver, the replace-range logic, and the JQL-escape security
// contract (H3/E1: catalog text can never break out of a quoted literal).

namespace {

TrackerField MakeField(const char* id, const char* name, TrackerFieldFamily family, const char* type = "") {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    f.Family = family;
    f.Type = type;
    return f;
}

std::vector<TrackerField> DefaultFields() {
    std::vector<TrackerField> fields;

    TrackerField status = MakeField("status", "Status", TrackerFieldFamily::Status);
    status.AllowedValues = {"Open", "In Progress"};
    fields.push_back(status);

    TrackerField assignee = MakeField("assignee", "Assignee", TrackerFieldFamily::UserSingle);
    assignee.IsUserType = true;
    fields.push_back(assignee);

    fields.push_back(MakeField("created", "Created", TrackerFieldFamily::Date));
    fields.push_back(MakeField("fixVersion", "Fix Version", TrackerFieldFamily::Unknown, "version"));
    fields.push_back(MakeField("sprint", "Sprint", TrackerFieldFamily::Sprint));
    return fields;
}

std::vector<TrackerUser> DefaultUsers() {
    std::vector<TrackerUser> users;
    TrackerUser alice;
    alice.AccountId = "a1";
    alice.DisplayName = "Alice Smith";
    alice.EmailAddress = "alice@example.com";
    users.push_back(alice);

    TrackerUser app; // Connect/Forge app — must never be suggested
    app.AccountId = "app1";
    app.DisplayName = "Automation App";
    app.AccountType = "app";
    users.push_back(app);

    TrackerUser inactive;
    inactive.AccountId = "i1";
    inactive.DisplayName = "Alumni Person";
    inactive.Active = false;
    users.push_back(inactive);
    return users;
}

bool HasInsert(const QuerySuggestBuild& out, const std::string& insert) {
    return std::any_of(out.Items.begin(), out.Items.end(),
                       [&](const QuerySuggestion& s) { return s.Insert == insert; });
}

bool HasLabel(const QuerySuggestBuild& out, const std::string& label) {
    return std::any_of(out.Items.begin(), out.Items.end(), [&](const QuerySuggestion& s) { return s.Label == label; });
}

QuerySuggestBuild Run(const std::string& buf, int cursor, const std::vector<TrackerField>& fields,
                      const std::vector<TrackerUser>& users, QuerySuggestMeta* meta = nullptr) {
    QuerySuggestBuild out;
    BuildJqlSuggestionsPure(buf.c_str(), static_cast<int>(buf.size()), cursor, cursor, cursor, fields, users, out,
                            meta);
    return out;
}

} // namespace

TEST_CASE("field mode — empty buffer suggests the field catalog + NOT") {
    const auto out = Run("", 0, DefaultFields(), DefaultUsers());
    CHECK(HasInsert(out, "NOT"));
    CHECK(HasInsert(out, "status"));
    CHECK(HasLabel(out, "Assignee (assignee)"));
    CHECK(out.ReplaceStart == 0);
    CHECK(out.ReplaceEnd == 0);
    // Sorted case-insensitively by label.
    CHECK(std::is_sorted(out.Items.begin(), out.Items.end(), [](const QuerySuggestion& a, const QuerySuggestion& b) {
        const std::string la = a.Label;
        const std::string lb = b.Label;
        auto low = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        return low(la) < low(lb) || (low(la) == low(lb) && la.size() < lb.size());
    }));
}

TEST_CASE("operator mode — after a recognised field token") {
    const std::string buf = "status ";
    const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields(), DefaultUsers());
    CHECK(HasInsert(out, "="));
    CHECK(HasInsert(out, "IN"));
    CHECK(HasInsert(out, "WAS IN"));
    CHECK_FALSE(HasInsert(out, "status")); // not field mode
}

TEST_CASE("value mode — allowed values with quote-on-demand inserts") {
    const std::string buf = "status = ";
    const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields(), DefaultUsers());
    // Plain identifier value inserts bare; the spaced value round-trips through quoting
    // with the "label -> insert" annotation.
    CHECK(HasInsert(out, "Open"));
    CHECK(HasInsert(out, "\"In Progress\""));
    CHECK(HasLabel(out, "In Progress -> \"In Progress\""));
}

TEST_CASE("value mode — user field surfaces catalog humans only, with escape-proof inserts") {
    auto fields = DefaultFields();
    auto users = DefaultUsers();
    // H3/E1 regression: a display name carrying a quote + backslash must be escaped inside
    // the quoted literal, never allowed to terminate it.
    TrackerUser hostile;
    hostile.AccountId = "h1";
    hostile.DisplayName = "Eve\" OR 1=1 \\";
    users.push_back(hostile);

    QuerySuggestMeta meta;
    const std::string buf = "assignee = ";
    {
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, users, &meta);
        // Empty prefix: no org-chart dump, but the user functions still appear.
        CHECK(HasInsert(out, "currentUser()"));
        CHECK_FALSE(HasInsert(out, "\"Alice Smith\""));
        CHECK(meta.UserValueToken);
        CHECK(meta.UserSearchPrefix.empty());
    }
    {
        const std::string typed = buf + "ali";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, users, &meta);
        CHECK(HasInsert(out, "\"Alice Smith\""));
        CHECK(HasLabel(out, "Alice Smith (alice@example.com)"));
        CHECK(meta.UserValueToken);
        CHECK(meta.UserSearchPrefix == "ali");
    }
    {
        // App + inactive accounts are filtered even on a matching prefix.
        const std::string typed = buf + "a";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, users, &meta);
        CHECK_FALSE(HasInsert(out, "\"Automation App\""));
        CHECK_FALSE(HasInsert(out, "\"Alumni Person\""));
    }
    {
        const std::string typed = buf + "eve";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, users, &meta);
        CHECK(HasInsert(out, "\"Eve\\\" OR 1=1 \\\\\"")); // escaped, still one literal
    }
}

TEST_CASE("value mode — family-gated JQL functions") {
    const auto fields = DefaultFields();
    {
        const std::string buf = "created > start";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "startOfDay()"));
        CHECK(HasInsert(out, "startOfYear()"));
        CHECK_FALSE(HasInsert(out, "currentUser()")); // wrong family
    }
    {
        const std::string buf = "fixVersion = rel";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "releasedVersions()"));
    }
    {
        const std::string buf = "sprint in open";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "openSprints()"));
    }
}

TEST_CASE("IS operand + logical + ORDER BY keyword modes") {
    const auto fields = DefaultFields();
    {
        const std::string buf = "status IS ";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "EMPTY"));
        CHECK(HasInsert(out, "NULL"));
    }
    {
        // Logical mode fires only once the user types a prefix.
        const std::string buf = "status = Open a";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "AND"));
        CHECK_FALSE(HasInsert(out, "ORDER BY")); // prefix "a" filters it out
    }
    {
        const std::string buf = "status = Open order ";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "BY"));
    }
    {
        const std::string buf = "status = Open order by ";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "created")); // order-field mode = field catalog
        CHECK_FALSE(HasInsert(out, "NOT"));
    }
    {
        const std::string buf = "status = Open order by created ";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "ASC"));
        CHECK(HasInsert(out, "DESC"));
    }
}

TEST_CASE("replace range — identifier straddle, selection override, open string") {
    const auto fields = DefaultFields();
    {
        // Cursor mid-identifier: the range expands across the whole token both ways.
        const std::string buf = "status";
        const auto out = Run(buf, 3, fields, DefaultUsers());
        CHECK(out.ReplaceStart == 0);
        CHECK(out.ReplaceEnd == 6);
    }
    {
        // Active selection wins verbatim.
        QuerySuggestBuild out;
        const std::string buf = "status = Open";
        BuildJqlSuggestionsPure(buf.c_str(), static_cast<int>(buf.size()), 13, 9, 13, fields, DefaultUsers(), out);
        CHECK(out.ReplaceStart == 9);
        CHECK(out.ReplaceEnd == 13);
    }
    {
        // Open quoted string: the replace span is the string body (so an accepted
        // suggestion swaps the partial literal, not the quote). Current engine semantics:
        // the tokenizer emits an empty quoted token for the open string, which resolves to
        // Logical mode — catalog values are NOT re-suggested mid-string. Pinned as-is.
        const std::string buf = "status = \"In P";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(out.ReplaceStart == 10);
        CHECK(out.ReplaceEnd == static_cast<int>(buf.size()));
        CHECK_FALSE(HasInsert(out, "\"In Progress\""));
    }
}

TEST_CASE("robustness — null buffer, out-of-range cursor, suggestion cap") {
    {
        QuerySuggestBuild out;
        BuildJqlSuggestionsPure(nullptr, 0, 5, -3, 99, DefaultFields(), DefaultUsers(), out);
        CHECK(out.Items.empty());
    }
    {
        // Cursor/selection clamp instead of reading out of bounds.
        const std::string buf = "st";
        QuerySuggestBuild out;
        BuildJqlSuggestionsPure(buf.c_str(), static_cast<int>(buf.size()), 999, -5, 999, DefaultFields(),
                                DefaultUsers(), out);
        CHECK(out.ReplaceEnd <= static_cast<int>(buf.size()));
    }
    {
        // 200 catalog fields sharing a prefix → hard cap at 80 suggestions.
        std::vector<TrackerField> many;
        for (int i = 0; i < 200; ++i) {
            many.push_back(MakeField(("field" + std::to_string(i)).c_str(), "", TrackerFieldFamily::Text));
        }
        const std::string buf = "fie";
        const auto out = Run(buf, static_cast<int>(buf.size()), many, {});
        CHECK(out.Items.size() == 80);
    }
}
