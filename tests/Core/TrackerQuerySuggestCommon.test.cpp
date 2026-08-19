// TrackerQuerySuggestCommon pure-logic tests — exercises the backend-agnostic query-suggest
// helpers extracted from JqlSuggestEngine.cpp + PlaneQuerySuggestEngine.cpp
// (dedup-tracker-query-and-ai-client-clones cluster A).
//
// The field-catalog helpers (FindTrackerField / AppendFieldCatalog) were reparameterized to
// take const std::vector<TrackerField>& directly (away from AppController), which is what makes
// this a bucket-A test — no AppController fixture, just a hand-built field vector.

#include "Tracker/TrackerQuerySuggestCommon.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

using namespace tracker_query_suggest;

namespace {

TrackerField MakeField(const std::string& id, const std::string& name, TrackerFieldFamily family) {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    f.Family = family;
    return f;
}

} // namespace

TEST_CASE("IsQueryIdChar: alnum + _ . - are id chars; others are not") {
    CHECK(IsQueryIdChar(static_cast<unsigned char>('a')));
    CHECK(IsQueryIdChar(static_cast<unsigned char>('Z')));
    CHECK(IsQueryIdChar(static_cast<unsigned char>('0')));
    CHECK(IsQueryIdChar(static_cast<unsigned char>('_')));
    CHECK(IsQueryIdChar(static_cast<unsigned char>('.')));
    CHECK(IsQueryIdChar(static_cast<unsigned char>('-')));
    CHECK_FALSE(IsQueryIdChar(static_cast<unsigned char>(' ')));
    CHECK_FALSE(IsQueryIdChar(static_cast<unsigned char>('"')));
    CHECK_FALSE(IsQueryIdChar(static_cast<unsigned char>('=')));
}

TEST_CASE("QueryValueNeedsQuotes: empty / spaces / quotes / backslash need quotes; plain ids don't") {
    CHECK(QueryValueNeedsQuotes(""));
    CHECK(QueryValueNeedsQuotes("two words"));
    CHECK(QueryValueNeedsQuotes("has\"quote"));
    CHECK(QueryValueNeedsQuotes("back\\slash"));
    CHECK(QueryValueNeedsQuotes("paren(s)"));
    CHECK_FALSE(QueryValueNeedsQuotes("plain"));
    CHECK_FALSE(QueryValueNeedsQuotes("dotted.id-name_1"));
}

TEST_CASE("QueryQuotedValue: wraps in quotes and escapes embedded quotes + backslashes") {
    CHECK(QueryQuotedValue("plain") == "\"plain\"");
    CHECK(QueryQuotedValue("a b") == "\"a b\"");
    CHECK(QueryQuotedValue("has\"q") == "\"has\\\"q\"");
    CHECK(QueryQuotedValue("back\\slash") == "\"back\\\\slash\"");
    CHECK(QueryQuotedValue("") == "\"\"");
}

TEST_CASE("InsertForValueToken: quotes only when needed (round-trip behaviour)") {
    CHECK(InsertForValueToken("plain") == "plain");
    CHECK(InsertForValueToken("two words") == "\"two words\"");
    CHECK(InsertForValueToken("has\"q") == "\"has\\\"q\"");
    // Empty needs quoting per QueryValueNeedsQuotes.
    CHECK(InsertForValueToken("") == "\"\"");
}

TEST_CASE("ScanStringStateToCursor: detects in-string vs out-of-string position") {
    bool inString = false;
    int openIdx = -1;

    // Cursor before any quote — out of string.
    const std::string a = "abc\"def";
    ScanStringStateToCursor(a.data(), static_cast<int>(a.size()), 2, inString, openIdx);
    CHECK_FALSE(inString);
    CHECK(openIdx == -1);

    // Cursor after an opening quote — in string, open index points at the quote.
    ScanStringStateToCursor(a.data(), static_cast<int>(a.size()), 6, inString, openIdx);
    CHECK(inString);
    CHECK(openIdx == 3);

    // Cursor after a closed string — out of string again.
    const std::string b = "\"done\" rest";
    ScanStringStateToCursor(b.data(), static_cast<int>(b.size()), 9, inString, openIdx);
    CHECK_FALSE(inString);
    CHECK(openIdx == -1);

    // Escaped quote inside a string does not close it.
    const std::string c = "\"a\\\"b";
    ScanStringStateToCursor(c.data(), static_cast<int>(c.size()), static_cast<int>(c.size()), inString, openIdx);
    CHECK(inString);
    CHECK(openIdx == 0);
}

TEST_CASE("AsciiStartsWithIgnoreCase: case-folded prefix match; empty prefix always matches") {
    CHECK(AsciiStartsWithIgnoreCase("Project", "proj"));
    CHECK(AsciiStartsWithIgnoreCase("PROJECT", "project"));
    CHECK(AsciiStartsWithIgnoreCase("anything", ""));
    CHECK_FALSE(AsciiStartsWithIgnoreCase("abc", "abcd"));
    CHECK_FALSE(AsciiStartsWithIgnoreCase("status", "proj"));
}

TEST_CASE("AsciiContainsIgnoreCase: case-folded substring match; empty needle always matches") {
    CHECK(AsciiContainsIgnoreCase("Alice Smith", "smith"));
    CHECK(AsciiContainsIgnoreCase("Alice Smith", "ce sm"));
    CHECK(AsciiContainsIgnoreCase("ALICE", "lic"));
    CHECK(AsciiContainsIgnoreCase("anything", ""));
    CHECK_FALSE(AsciiContainsIgnoreCase("abc", "abcd"));
    CHECK_FALSE(AsciiContainsIgnoreCase("status", "proj"));
}

TEST_CASE("AppendValueSuggestions: user fields match mid-value, other families stay prefix-anchored") {
    TrackerField assignee = MakeField("assignee", "Assignee", TrackerFieldFamily::UserSingle);
    TrackerFieldOption alice;
    alice.Id = "a1";
    alice.Value = "Alice Smith";
    assignee.AllowedValueOptions.push_back(alice);

    std::vector<QuerySuggestion> out;
    std::unordered_set<std::string> seen;
    AppendValueSuggestions(assignee, "smith", " (display name) -> ", out, seen);
    CHECK(out.size() == 2); // account-id entry + display-name entry
    CHECK(std::any_of(out.begin(), out.end(), [](const QuerySuggestion& s) { return s.Insert == "a1"; }));
    CHECK(std::any_of(out.begin(), out.end(), [](const QuerySuggestion& s) { return s.Insert == "\"Alice Smith\""; }));

    // Non-user family: a mid-value run must NOT match — status/version pickers stay anchored.
    TrackerField status = MakeField("status", "Status", TrackerFieldFamily::Status);
    status.AllowedValues = {"In Progress"};
    std::vector<QuerySuggestion> statusOut;
    std::unordered_set<std::string> statusSeen;
    AppendValueSuggestions(status, "progress", " (display name) -> ", statusOut, statusSeen);
    CHECK(statusOut.empty());
    AppendValueSuggestions(status, "in p", " (display name) -> ", statusOut, statusSeen);
    CHECK(statusOut.size() == 1);
}

TEST_CASE("AsciiEqualsIgnoreCaseToLowered: equal modulo case (second arg pre-lowered)") {
    CHECK(AsciiEqualsIgnoreCaseToLowered("Status", "status"));
    CHECK(AsciiEqualsIgnoreCaseToLowered("STATUS", "status"));
    CHECK_FALSE(AsciiEqualsIgnoreCaseToLowered("Statuses", "status"));
    CHECK_FALSE(AsciiEqualsIgnoreCaseToLowered("other", "status"));
}

TEST_CASE("AddSuggestionUnique: dedups by insert; empty insert dropped; empty label falls back") {
    std::vector<QuerySuggestion> out;
    std::unordered_set<std::string> seen;

    AddSuggestionUnique(out, seen, "Label A", "insertA");
    AddSuggestionUnique(out, seen, "Label A again", "insertA"); // dup insert -> dropped
    AddSuggestionUnique(out, seen, "", "");                     // empty insert -> dropped
    AddSuggestionUnique(out, seen, "", "insertB");              // empty label -> falls back to insert

    REQUIRE(out.size() == 2);
    CHECK(out[0].Label == "Label A");
    CHECK(out[0].Insert == "insertA");
    CHECK(out[1].Label == "insertB");
    CHECK(out[1].Insert == "insertB");
}

TEST_CASE("FindTrackerField: resolves by id then by name, case-insensitively") {
    std::vector<TrackerField> fields;
    fields.push_back(MakeField("status", "Status", TrackerFieldFamily::Status));
    fields.push_back(MakeField("assignee", "Assignee", TrackerFieldFamily::UserSingle));

    CHECK(FindTrackerField(fields, "STATUS") == &fields[0]);
    CHECK(FindTrackerField(fields, "Assignee") == &fields[1]);
    // Name match when id miss.
    CHECK(FindTrackerField(fields, "status") == &fields[0]);
    // Empty token -> nullptr.
    CHECK(FindTrackerField(fields, "") == nullptr);
    // Unknown -> nullptr.
    CHECK(FindTrackerField(fields, "nope") == nullptr);
}

TEST_CASE("AppendFieldCatalog: prefix-filtered id + name suggestions, deduped by insert (field id)") {
    std::vector<TrackerField> fields;
    fields.push_back(MakeField("status", "Status", TrackerFieldFamily::Status));
    fields.push_back(MakeField("assignee", "Assignee", TrackerFieldFamily::UserSingle));
    fields.push_back(MakeField("summary", "Summary", TrackerFieldFamily::Text));

    std::vector<QuerySuggestion> out;
    std::unordered_set<std::string> seen;
    AppendFieldCatalog(fields, "s", out, seen);

    // "status" and "summary" match by id prefix "s"; "assignee" does not. Each inserts its id.
    bool sawStatus = false;
    bool sawSummary = false;
    bool sawAssignee = false;
    for (const auto& s : out) {
        if (s.Insert == "status")
            sawStatus = true;
        if (s.Insert == "summary")
            sawSummary = true;
        if (s.Insert == "assignee")
            sawAssignee = true;
    }
    CHECK(sawStatus);
    CHECK(sawSummary);
    CHECK_FALSE(sawAssignee);

    // Empty prefix surfaces all three (no further dup of the same id).
    std::vector<QuerySuggestion> all;
    std::unordered_set<std::string> seenAll;
    AppendFieldCatalog(fields, "", all, seenAll);
    CHECK(all.size() == 3);
}

TEST_CASE("AppendTerms: appends only prefix-matching terms (case-insensitive)") {
    static const char* kTerms[] = {"AND", "OR", "ORDER BY"};
    std::vector<QuerySuggestion> out;
    std::unordered_set<std::string> seen;
    AppendTerms("or", kTerms, 3, out, seen);

    REQUIRE(out.size() == 2);
    CHECK(out[0].Insert == "OR");
    CHECK(out[1].Insert == "ORDER BY");
}

TEST_CASE("IsQueryUserField / IsQueryDateField: family predicates") {
    CHECK(IsQueryUserField(MakeField("a", "A", TrackerFieldFamily::UserSingle)));
    CHECK(IsQueryUserField(MakeField("a", "A", TrackerFieldFamily::UserMulti)));
    TrackerField flagged = MakeField("a", "A", TrackerFieldFamily::Text);
    flagged.IsUserType = true;
    CHECK(IsQueryUserField(flagged));
    CHECK_FALSE(IsQueryUserField(MakeField("a", "A", TrackerFieldFamily::Text)));

    CHECK(IsQueryDateField(MakeField("a", "A", TrackerFieldFamily::Date)));
    CHECK(IsQueryDateField(MakeField("a", "A", TrackerFieldFamily::DateTime)));
    CHECK_FALSE(IsQueryDateField(MakeField("a", "A", TrackerFieldFamily::Text)));
}
