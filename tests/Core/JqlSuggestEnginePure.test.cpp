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
    {
        // Email-prefix fallback: a user whose DISPLAY name does not prefix-match but whose
        // EMAIL does must still surface. Every other user case here matches by display name,
        // so without this the emailMatch branch (AppendJqlUserCatalogSuggestions) is unasserted.
        auto emailUsers = DefaultUsers();
        TrackerUser byEmail;
        byEmail.AccountId = "z1";
        byEmail.DisplayName = "Bob Jones";        // does NOT start with "zoe"
        byEmail.EmailAddress = "zoe@example.com"; // DOES start with "zoe"
        emailUsers.push_back(byEmail);
        const std::string typed = buf + "zoe";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, emailUsers, &meta);
        CHECK(HasInsert(out, "\"Bob Jones\""));
        CHECK(HasLabel(out, "Bob Jones (zoe@example.com)"));
    }
    {
        // Mid-name match: a surname (or any inner run) finds the account — the reason the
        // matcher is substring, not prefix-anchored.
        const std::string typed = buf + "smit";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, users, &meta);
        CHECK(HasInsert(out, "\"Alice Smith\""));
    }
    {
        // Substring matching does not resurrect the filtered account types.
        const std::string typed = buf + "utomation";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, users, &meta);
        CHECK_FALSE(HasInsert(out, "\"Automation App\""));
    }
    {
        // Mid-email match (domain / local-part tail) surfaces the account too.
        const std::string typed = buf + "example.com";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, users, &meta);
        CHECK(HasInsert(out, "\"Alice Smith\""));
    }
    {
        // Cap contention: 50 users match only mid-name, one matches by prefix. The prefix
        // match must survive the kMaxUsers cut — that is what the two-pass order buys.
        std::vector<TrackerUser> capUsers;
        for (int i = 0; i < 50; ++i) {
            TrackerUser u;
            u.AccountId = "mid" + std::to_string(i);
            u.DisplayName = "Person Zeta" + std::to_string(i); // contains "zeta", starts with "Person"
            capUsers.push_back(u);
        }
        TrackerUser prefixed;
        prefixed.AccountId = "pre1";
        prefixed.DisplayName = "Zeta Prefix"; // starts with "zeta"
        capUsers.push_back(prefixed);         // last in catalog order, so only ordering saves it
        const std::string typed = buf + "zeta";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, capUsers, &meta);
        CHECK(HasInsert(out, "\"Zeta Prefix\""));
        const auto userCount = std::count_if(out.Items.begin(), out.Items.end(), [](const QuerySuggestion& s) {
            return s.Label.rfind("Person Zeta", 0) == 0 || s.Label.rfind("Zeta Prefix", 0) == 0;
        });
        CHECK(userCount == 50);
    }
    {
        // Held-back mid-name matches that duplicate an already-emitted insert must not burn
        // hold capacity: 50 same-named duplicates ahead of one distinct mid-name match, and
        // the distinct one still lands.
        std::vector<TrackerUser> dupUsers;
        TrackerUser prefixed;
        prefixed.AccountId = "p1";
        prefixed.DisplayName = "Zeta Lead"; // prefix match, emitted first
        dupUsers.push_back(prefixed);
        for (int i = 0; i < 50; ++i) {
            TrackerUser u;
            u.AccountId = "dup" + std::to_string(i);
            u.DisplayName = "Zeta Lead"; // same insert -> AddSuggestionUnique would drop each
            dupUsers.push_back(u);
        }
        TrackerUser distinct;
        distinct.AccountId = "d1";
        distinct.DisplayName = "Team Zeta"; // distinct mid-name match, last in catalog order
        dupUsers.push_back(distinct);
        const std::string typed = buf + "zeta";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, dupUsers, &meta);
        CHECK(HasInsert(out, "\"Zeta Lead\""));
        CHECK(HasInsert(out, "\"Team Zeta\""));
    }
    {
        // 51 matching users → the kMaxUsers cap binds at exactly 50 (pins the >= bound).
        std::vector<TrackerUser> capUsers;
        for (int i = 0; i < 51; ++i) {
            TrackerUser u;
            u.AccountId = "cap" + std::to_string(i);
            u.DisplayName = "User " + std::to_string(i);
            capUsers.push_back(u);
        }
        const std::string typed = buf + "user";
        const auto out = Run(typed, static_cast<int>(typed.size()), fields, capUsers, &meta);
        const auto userCount = std::count_if(out.Items.begin(), out.Items.end(),
                                             [](const QuerySuggestion& s) { return s.Label.rfind("User ", 0) == 0; });
        CHECK(userCount == 50);
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

TEST_CASE("tokenizer — quoted literals, escapes, punctuation, multi-char operators, stray bytes") {
    const auto fields = DefaultFields();
    {
        // A closed quoted literal (with an escaped quote inside) is one token; the clause
        // after it resolves back to field mode.
        const std::string buf = "summary ~ \"a\\\"b\" AND stat";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "status"));
    }
    {
        // Two-char operator token (!=) still lands in value mode for the field on its left.
        const std::string buf = "status != ";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "Open"));
    }
    {
        const std::string buf = "created >= start";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "startOfDay()"));
    }
    {
        // A byte that is neither id-char, quote, punct, nor operator becomes a lone token
        // that matches nothing: no field/operator suggestions, no crash.
        const std::string buf = "status = #";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(out.Items.empty());
    }
}

TEST_CASE("value mode — reached through '(' and ',' with the field resolved across NOT/WAS/punct") {
    const auto fields = DefaultFields();
    {
        // IN-list opener: value mode for the field left of the operator.
        const std::string buf = "status in (";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "Open"));
    }
    {
        // Continuing the IN list after a comma stays in value mode.
        const std::string buf = "status in (Open, ";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "\"In Progress\""));
    }
    {
        // NOT between field and operator is skipped when resolving the value field.
        const std::string buf = "status not in (";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "Open"));
    }
    {
        // WAS is skipped the same way.
        const std::string buf = "status was in (";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "Open"));
    }
    {
        // Punctuation between operator and field name (a parenthesised clause) is skipped too.
        const std::string buf = "(status) = ";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "Open"));
    }
    {
        // Unknown field before the opener degrades to field mode, not value mode.
        const std::string buf = "nosuchfield in (";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "status"));
        CHECK_FALSE(HasInsert(out, "Open"));
    }
}

TEST_CASE("order-by tail — after ASC/DESC the engine goes quiet until a new prefix is typed") {
    const auto fields = DefaultFields();
    {
        const std::string buf = "status = Open order by created asc ";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(out.Items.empty());
    }
    {
        // Typing after a sort direction re-enters order-field mode (a second sort key).
        const std::string buf = "status = Open order by created asc cr";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "created"));
    }
}

TEST_CASE("value mode — user rows come from the user catalog only, one row per user") {
    auto fields = DefaultFields();
    TrackerField& assignee = fields[1];
    // Production shape: TrackerFieldCatalog mirrors the whole user catalog into the user
    // field's options. Those options must NOT surface as rows of their own — the raw
    // accountId insert would sit next to the catalog row's name insert as a second,
    // hash-inserting entry per user.
    TrackerFieldOption alice;
    alice.Id = "a1";
    alice.Value = "Alice Smith";
    assignee.AllowedValueOptions.push_back(alice);
    assignee.AllowedValues.push_back("Alice Smith");

    {
        const std::string buf = "assignee = ali";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, DefaultUsers());
        CHECK(HasInsert(out, "\"Alice Smith\"")); // the catalog row's name-form insert
        CHECK_FALSE(HasInsert(out, "a1"));          // no raw-accountId row
        CHECK_FALSE(HasLabel(out, "Alice Smith (display name) -> \"Alice Smith\""));
        const int aliceRows = static_cast<int>(std::count_if(
            out.Items.begin(), out.Items.end(),
            [](const QuerySuggestion& sug) { return sug.Insert == "\"Alice Smith\""; }));
        CHECK(aliceRows == 1);
    }
    {
        // An options-only user (no catalog backing) no longer surfaces at all: a name-form
        // insert for them could never reverse-map to an accountId at the apply boundary.
        const std::string buf = "assignee = jane";
        TrackerFieldOption jane;
        jane.Id = "acc-1";
        jane.Value = "Jane Doe";
        fields[1].AllowedValueOptions.push_back(jane);
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, {});
        CHECK_FALSE(HasInsert(out, "\"Jane Doe\""));
        CHECK_FALSE(HasInsert(out, "acc-1"));
    }
}

TEST_CASE("value mode — non-user option lists suggest by value and by id, skipping empties") {
    auto fields = DefaultFields();
    TrackerField component = MakeField("component", "Component", TrackerFieldFamily::Text);
    TrackerFieldOption backend;
    backend.Id = "10001";
    backend.Value = "Backend";
    component.AllowedValueOptions.push_back(backend);
    component.AllowedValues.push_back(""); // catalog rot: empty entries are ignored
    fields.push_back(component);

    {
        const std::string buf = "component = back";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, {});
        CHECK(HasInsert(out, "Backend"));
        CHECK_FALSE(HasLabel(out, "10001 (Backend)")); // id row requires an id-prefix match
    }
    {
        const std::string buf = "component = 100";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, {});
        CHECK(HasLabel(out, "10001 (Backend)"));
        CHECK(HasInsert(out, "10001"));
    }
}

TEST_CASE("user catalog — accountId insert fallback when the display name is empty") {
    const auto fields = DefaultFields();
    std::vector<TrackerUser> users;
    TrackerUser idOnly; // server sent no display name — the quoted accountId is still queryable
    idOnly.AccountId = "only-id";
    idOnly.EmailAddress = "noname@example.com";
    users.push_back(idOnly);
    TrackerUser ghost; // no display name AND no accountId — the empty insert is dropped, no crash
    ghost.EmailAddress = "ghost@example.com";
    users.push_back(ghost);

    {
        const std::string buf = "assignee = noname";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, users);
        CHECK(HasInsert(out, "\"only-id\""));
        CHECK(HasLabel(out, " (noname@example.com)"));
    }
    {
        // AddSuggestionUnique refuses an empty insert, so the row never surfaces.
        const std::string buf = "assignee = ghost";
        const auto out = Run(buf, static_cast<int>(buf.size()), fields, users);
        CHECK_FALSE(HasLabel(out, " (ghost@example.com)"));
        CHECK(out.Items.empty());
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
