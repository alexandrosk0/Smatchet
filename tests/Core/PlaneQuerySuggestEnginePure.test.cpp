#include <doctest/doctest.h>

// Coverage for the Plane filter-mini-language autocomplete core extracted from
// PlaneQuerySuggestEngine.cpp (TEST_COVERAGE_GAP_MAP.md Tier 1 #5). Contract: the
// `fieldId:value` / `fieldId=value` context parse resolves the field left of the
// separator, value suggestions quote when needed, everything else falls back to
// field-catalog + logical keywords, and any buffer terminates with sane spans.
#include "Tracker/PlaneQuerySuggestEnginePure.h"

#include "Tracker/TrackerFieldSchema.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::vector<TrackerField> CatalogFields() {
    std::vector<TrackerField> fields;
    TrackerField state;
    state.Id = "state";
    state.Name = "State";
    state.Family = TrackerFieldFamily::Status;
    TrackerFieldOption todo;
    todo.Id = "uuid-1";
    todo.Value = "To Do";
    TrackerFieldOption done;
    done.Id = "uuid-2";
    done.Value = "Done";
    state.AllowedValueOptions.push_back(todo);
    state.AllowedValueOptions.push_back(done);
    fields.push_back(state);

    TrackerField assignee;
    assignee.Id = "assignee";
    assignee.Name = "Assignee";
    assignee.Family = TrackerFieldFamily::UserSingle;
    TrackerFieldOption alice;
    alice.Id = "user-uuid-1";
    alice.Value = "Alice Smith";
    assignee.AllowedValueOptions.push_back(alice);
    fields.push_back(assignee);

    TrackerField priority;
    priority.Id = "priority";
    priority.Name = "Priority";
    priority.Family = TrackerFieldFamily::SelectSingle;
    priority.AllowedValues.push_back("urgent");
    priority.AllowedValues.push_back("high");
    fields.push_back(priority);
    return fields;
}

QuerySuggestBuild Run(const std::string& q, QuerySuggestMeta* meta = nullptr) {
    QuerySuggestBuild out;
    const std::vector<TrackerField> fields = CatalogFields();
    const int len = static_cast<int>(q.size());
    PlaneQuerySuggestEnginePure::BuildPlaneQuerySuggestions(q.c_str(), len, len, len, len, fields, out, meta);
    return out;
}

bool HasInsert(const QuerySuggestBuild& b, const std::string& insert) {
    return std::any_of(b.Items.begin(), b.Items.end(), [&](const QuerySuggestion& s) { return s.Insert == insert; });
}

bool HasLabel(const QuerySuggestBuild& b, const std::string& label) {
    return std::any_of(b.Items.begin(), b.Items.end(), [&](const QuerySuggestion& s) { return s.Label == label; });
}

} // namespace

TEST_CASE("field mode lists the catalog, prefix-filtered") {
    const QuerySuggestBuild all = Run("");
    CHECK(HasInsert(all, "state"));
    CHECK(HasInsert(all, "assignee"));
    CHECK(HasInsert(all, "priority"));
    const QuerySuggestBuild pri = Run("pri");
    CHECK(HasInsert(pri, "priority"));
    CHECK(!HasInsert(pri, "state"));
}

TEST_CASE("colon separator enters value mode for the field on its left") {
    const QuerySuggestBuild b = Run("state:");
    CHECK(HasInsert(b, "\"To Do\""));
    CHECK(HasInsert(b, "Done"));
    // Field names are not suggested inside a value token.
    CHECK(!HasInsert(b, "priority"));
}

TEST_CASE("equals separator and surrounding whitespace also enter value mode") {
    CHECK(HasInsert(Run("state="), "\"To Do\""));
    const QuerySuggestBuild spaced = Run("state : In");
    // Prefix "In" filters the options; nothing matches ("To Do"/"Done").
    CHECK(spaced.Items.empty());
    CHECK(HasInsert(Run("state : To"), "\"To Do\""));
}

TEST_CASE("plain AllowedValues suggest unquoted id-safe tokens") {
    const QuerySuggestBuild b = Run("priority:u");
    CHECK(HasInsert(b, "urgent"));
    CHECK(!HasInsert(b, "high"));
}

TEST_CASE("user-type field value sets the live-search meta and labels options as (display)") {
    QuerySuggestMeta meta;
    const QuerySuggestBuild b = Run("assignee:Ali", &meta);
    CHECK(meta.UserValueToken);
    CHECK(meta.UserSearchPrefix == "Ali");
    CHECK(HasInsert(b, "\"Alice Smith\""));
    CHECK(HasLabel(b, "Alice Smith (display) -> \"Alice Smith\""));
}

TEST_CASE("outside a value context, logical keywords join the field catalog") {
    const QuerySuggestBuild b = Run("state:Done An");
    CHECK(HasInsert(b, "AND"));
    CHECK(!HasInsert(b, "OR"));
}

TEST_CASE("null buffer and clamped cursor degrade to empty output") {
    QuerySuggestBuild out;
    const std::vector<TrackerField> fields = CatalogFields();
    PlaneQuerySuggestEnginePure::BuildPlaneQuerySuggestions(nullptr, 0, 3, -1, 42, fields, out, nullptr);
    CHECK(out.Items.empty());
    CHECK(out.ReplaceStart == 0);
    CHECK(out.ReplaceEnd == 0);
}
