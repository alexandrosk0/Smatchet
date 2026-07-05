#include <doctest/doctest.h>

#include "PlaneQuerySuggestEnginePure.h"

#include <algorithm>
#include <string>
#include <vector>

// The Plane filter mini-language (`fieldId:value AND fieldId:value`) parses raw omnibox
// keystrokes against a tracker-server-sourced field catalog — both untrusted. These cases
// pin the colon/equals value-context resolver, the field-catalog fallback, and the
// quote-on-demand value inserts.

namespace {

std::vector<TrackerField> DefaultFields() {
    std::vector<TrackerField> fields;

    TrackerField state;
    state.Id = "state";
    state.Name = "State";
    state.Family = TrackerFieldFamily::SelectSingle;
    TrackerFieldOption backlog;
    backlog.Id = "b1";
    backlog.Value = "Backlog";
    state.AllowedValueOptions.push_back(backlog);
    TrackerFieldOption started;
    started.Id = "s1";
    started.Value = "In Flight";
    state.AllowedValueOptions.push_back(started);
    fields.push_back(state);

    TrackerField assignee;
    assignee.Id = "assignees";
    assignee.Name = "Assignees";
    assignee.Family = TrackerFieldFamily::UserMulti;
    assignee.IsUserType = true;
    TrackerFieldOption user;
    user.Id = "u-123";
    user.Value = "Alice";
    assignee.AllowedValueOptions.push_back(user);
    fields.push_back(assignee);

    return fields;
}

bool HasInsert(const QuerySuggestBuild& out, const std::string& insert) {
    return std::any_of(out.Items.begin(), out.Items.end(),
                       [&](const QuerySuggestion& s) { return s.Insert == insert; });
}

QuerySuggestBuild Run(const std::string& buf, int cursor, const std::vector<TrackerField>& fields,
                      QuerySuggestMeta* meta = nullptr) {
    QuerySuggestBuild out;
    BuildPlaneQuerySuggestionsPure(buf.c_str(), static_cast<int>(buf.size()), cursor, cursor, cursor, fields, out,
                                   meta);
    return out;
}

} // namespace

TEST_CASE("field-catalog mode — no value context") {
    {
        const auto out = Run("", 0, DefaultFields());
        CHECK(HasInsert(out, "state"));
        CHECK(HasInsert(out, "assignees"));
        // Logical terms only appear once a prefix exists.
        CHECK_FALSE(HasInsert(out, "AND"));
    }
    {
        const std::string buf = "state:Backlog a";
        const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields());
        CHECK(HasInsert(out, "AND"));
        CHECK(HasInsert(out, "assignees"));
        CHECK_FALSE(HasInsert(out, "OR")); // prefix "a" filters it
    }
}

TEST_CASE("value context — colon and equals separators resolve the field") {
    {
        const std::string buf = "state:";
        const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields());
        CHECK(HasInsert(out, "Backlog"));
        CHECK(HasInsert(out, "\"In Flight\"")); // space forces quoting
    }
    {
        const std::string buf = "state:Ba";
        const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields());
        CHECK(HasInsert(out, "Backlog"));
        CHECK_FALSE(HasInsert(out, "\"In Flight\""));
        CHECK(out.ReplaceStart == 6); // replaces only the value token
    }
    {
        // '=' separator + surrounding whitespace both resolve.
        const std::string buf = "state = In";
        const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields());
        CHECK(HasInsert(out, "\"In Flight\""));
    }
    {
        // Option Id offered alongside the display value when distinct.
        const std::string buf = "state:b1";
        const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields());
        CHECK(HasInsert(out, "b1"));
    }
}

TEST_CASE("value context — user field sets the live-search meta + Plane display label") {
    QuerySuggestMeta meta;
    const std::string buf = "assignees:Ali";
    const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields(), &meta);
    CHECK(meta.UserValueToken);
    CHECK(meta.UserSearchPrefix == "Ali");
    // Plane labels the display-name option " (display)" (deliberately distinct from Jira's
    // " (display name)") and offers the account-id insert too.
    const bool hasDisplayAnnotated = std::any_of(out.Items.begin(), out.Items.end(), [](const QuerySuggestion& s) {
        return s.Label.find(" (display) -> ") != std::string::npos;
    });
    CHECK(hasDisplayAnnotated);
    // Typing the DISPLAY-name prefix ("Ali") — which does NOT prefix-match the account id
    // "u-123" — must still offer the account-id insert. This pins the OR in matchesPrefix
    // (raw-prefix OR label-prefix); an AND regression silently drops the id-insert suggestion.
    CHECK(HasInsert(out, "u-123"));
}

TEST_CASE("unknown field before the separator falls back to the field catalog") {
    const std::string buf = "bogus:va";
    const auto out = Run(buf, static_cast<int>(buf.size()), DefaultFields());
    CHECK_FALSE(HasInsert(out, "Backlog"));
    // "va" matches no field either — but the engine is in catalog mode, not value mode.
    CHECK(out.Items.size() <= 2);
}

TEST_CASE("robustness — null buffer + cursor clamping") {
    QuerySuggestBuild out;
    BuildPlaneQuerySuggestionsPure(nullptr, 0, 7, -1, 42, DefaultFields(), out);
    CHECK(out.Items.empty());

    const std::string buf = "st";
    QuerySuggestBuild out2;
    BuildPlaneQuerySuggestionsPure(buf.c_str(), static_cast<int>(buf.size()), 999, 999, -4, DefaultFields(), out2);
    CHECK(out2.ReplaceEnd <= static_cast<int>(buf.size()));
}
