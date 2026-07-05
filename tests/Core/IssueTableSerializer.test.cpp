// IssueTableSerializer.test.cpp — doctest coverage for pure-logic CSV/TSV/JSON
// import + export in IssueTableSerializer.
//
// Item #31 (build-quality-velocity-hardening): verified that 9 Tracker *Pure units
// already have comprehensive test files. IssueTableSerializer.cpp is the one genuine
// gap: it has no existing test despite containing rich, cpr-free, AppController-free
// pure logic (format detection, column key resolution, draft field application,
// CSV/TSV/JSON parse, and ticket serialization). This file fills that gap.
//
// IssueTableSerializer.cpp links:
//   - nlohmann/json (already in test link libs)
//   - IssueDraft.h, LocalCacheManager.h, TrackerFieldSchema.h (all pure headers;
//     SQLiteCpp already linked in SmatchetTests)
//
// No cpr / AppController / ImGui dependency — confirmed by reading the .cpp source.

#include <doctest/doctest.h>

#include "IssueTableSerializer.h"

#include <string>
#include <vector>

using namespace IssueTableSerializer;

// ---------------------------------------------------------------------------
// Helper builders
// ---------------------------------------------------------------------------

namespace {

TrackerField MakeField(const std::string& id, const std::string& name) {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    return f;
}

// Build a minimal CachedTicket with the given id + one field value.
CachedTicket MakeTicket(const std::string& id, const std::string& fieldId, const std::string& fieldVal) {
    CachedTicket t;
    t.id = id;
    t.fieldValues[fieldId] = fieldVal;
    return t;
}

} // namespace

// ===========================================================================
// DetectFormat
// ===========================================================================

TEST_CASE("DetectFormat: JSON roots ([ and {) are detected correctly") {
    SUBCASE("array start") { CHECK(DetectFormat("[{}]") == Format::Json); }
    SUBCASE("object start") { CHECK(DetectFormat("{\"issues\":[]}") == Format::Json); }
    SUBCASE("leading whitespace before [") { CHECK(DetectFormat("  \n[{}]") == Format::Json); }
    SUBCASE("leading whitespace before {") { CHECK(DetectFormat("\t{\"k\":1}") == Format::Json); }
}

TEST_CASE("DetectFormat: TSV when header line has more tabs than commas") {
    CHECK(DetectFormat("key\tsummary\tproject\nABC-1\tfoo\tPRJ") == Format::Tsv);
}

TEST_CASE("DetectFormat: CSV is the default") {
    CHECK(DetectFormat("key,summary\nABC-1,foo") == Format::Csv);
    CHECK(DetectFormat("") == Format::Csv);
    CHECK(DetectFormat("no-delimiter-at-all") == Format::Csv);
}

// ===========================================================================
// ResolveColumnKey
// ===========================================================================

TEST_CASE("ResolveColumnKey: empty header returns empty string") {
    std::vector<TrackerField> cat;
    CHECK(ResolveColumnKey("", cat) == "");
    CHECK(ResolveColumnKey("  ", cat) == "");
}

TEST_CASE("ResolveColumnKey: key / issuekey / issue key map to __existing_issue_key__") {
    std::vector<TrackerField> cat;
    CHECK(ResolveColumnKey("key", cat) == "__existing_issue_key__");
    CHECK(ResolveColumnKey("Key", cat) == "__existing_issue_key__");
    CHECK(ResolveColumnKey("IssueKey", cat) == "__existing_issue_key__");
    CHECK(ResolveColumnKey("issue key", cat) == "__existing_issue_key__");
}

TEST_CASE("ResolveColumnKey: matches catalog by field id (case-insensitive)") {
    std::vector<TrackerField> cat = {MakeField("customfield_10001", "Story Points")};
    CHECK(ResolveColumnKey("customfield_10001", cat) == "customfield_10001");
    CHECK(ResolveColumnKey("CUSTOMFIELD_10001", cat) == "customfield_10001");
}

TEST_CASE("ResolveColumnKey: falls back to catalog display name") {
    std::vector<TrackerField> cat = {MakeField("customfield_10001", "Story Points")};
    CHECK(ResolveColumnKey("Story Points", cat) == "customfield_10001");
    CHECK(ResolveColumnKey("story points", cat) == "customfield_10001");
}

TEST_CASE("ResolveColumnKey: well-known special aliases") {
    std::vector<TrackerField> cat;
    CHECK(ResolveColumnKey("project", cat) == "project");
    CHECK(ResolveColumnKey("Project Key", cat) == "project");
    CHECK(ResolveColumnKey("issuetype", cat) == "issuetype");
    CHECK(ResolveColumnKey("Issue Type", cat) == "issuetype");
    CHECK(ResolveColumnKey("type", cat) == "issuetype");
    CHECK(ResolveColumnKey("parent", cat) == "parent");
    CHECK(ResolveColumnKey("Parent Key", cat) == "parent");
    CHECK(ResolveColumnKey("attachments", cat) == "attachments");
    CHECK(ResolveColumnKey("attachment", cat) == "attachments");
    CHECK(ResolveColumnKey("summary", cat) == "summary");
    CHECK(ResolveColumnKey("title", cat) == "summary");
    CHECK(ResolveColumnKey("description", cat) == "description");
}

TEST_CASE("ResolveColumnKey: unknown header returns trimmed original") {
    std::vector<TrackerField> cat;
    CHECK(ResolveColumnKey("MyCustomColumn", cat) == "MyCustomColumn");
    CHECK(ResolveColumnKey("  MyCustomColumn  ", cat) == "MyCustomColumn");
}

// ===========================================================================
// ApplyKeyValueToDraft
// ===========================================================================

TEST_CASE("ApplyKeyValueToDraft: project, issuetype, parent, attachments, generic field") {
    SUBCASE("project key") {
        IssueDraft d;
        ApplyKeyValueToDraft(d, "project", "PRJ");
        CHECK(d.ProjectKey == "PRJ");
    }
    SUBCASE("issuetype clears id, sets name") {
        IssueDraft d;
        d.IssueTypeId = "old-id";
        ApplyKeyValueToDraft(d, "issuetype", "Story");
        CHECK(d.IssueTypeName == "Story");
        CHECK(d.IssueTypeId.empty());
    }
    SUBCASE("parent key") {
        IssueDraft d;
        ApplyKeyValueToDraft(d, "parent", "ABC-1");
        CHECK(d.ParentKey == "ABC-1");
    }
    SUBCASE("generic field value") {
        IssueDraft d;
        ApplyKeyValueToDraft(d, "customfield_10001", "8");
        CHECK(d.FieldValues.count("customfield_10001") == 1);
        CHECK(d.FieldValues.at("customfield_10001") == "8");
    }
    SUBCASE("empty key is a no-op") {
        IssueDraft d;
        d.ProjectKey = "PRJ";
        ApplyKeyValueToDraft(d, "", "ignored");
        CHECK(d.ProjectKey == "PRJ");
    }
}

TEST_CASE("ApplyKeyValueToDraft: __existing_issue_key__ skips empty value") {
    IssueDraft d;
    d.ExistingIssueKey = "ABC-1";
    ApplyKeyValueToDraft(d, "__existing_issue_key__", "");
    CHECK(d.ExistingIssueKey == "ABC-1"); // not overwritten by empty
    ApplyKeyValueToDraft(d, "__existing_issue_key__", "ABC-2");
    CHECK(d.ExistingIssueKey == "ABC-2"); // non-empty overwrites
}

TEST_CASE("ApplyKeyValueToDraft: attachments — pipe-separated paths staged") {
    IssueDraft d;
    ApplyKeyValueToDraft(d, "attachments", "/a/b.png|/c/d.txt");
    REQUIRE(d.StagedAttachments.size() == 2);
    CHECK(d.StagedAttachments[0].AbsPath == "/a/b.png");
    CHECK(d.StagedAttachments[0].FileName == "b.png");
    CHECK(d.StagedAttachments[1].AbsPath == "/c/d.txt");
    CHECK(d.StagedAttachments[1].FileName == "d.txt");
}

TEST_CASE("ApplyKeyValueToDraft: attachments — JSON-array value is skipped") {
    IssueDraft d;
    ApplyKeyValueToDraft(d, "attachments", "[{\"filename\":\"ignore.png\"}]");
    CHECK(d.StagedAttachments.empty());
}

TEST_CASE("ApplyKeyValueToDraft: attachments — empty value is no-op") {
    IssueDraft d;
    ApplyKeyValueToDraft(d, "attachments", "");
    CHECK(d.StagedAttachments.empty());
}

// ===========================================================================
// ParseDrafts — CSV
// ===========================================================================

TEST_CASE("ParseDrafts CSV: basic import with well-known headers") {
    const std::string csv = "project,issuetype,summary\nPRJ,Story,First issue\nPRJ,Bug,Second issue\n";
    std::vector<TrackerField> cat;
    ImportResult r = ParseDrafts(csv, Format::Csv, cat, "", "", "");
    CHECK(r.Error.empty());
    CHECK(r.DetectedFormat == Format::Csv);
    REQUIRE(r.Rows.size() == 2);
    CHECK(r.Rows[0].Draft.ProjectKey == "PRJ");
    CHECK(r.Rows[0].Draft.IssueTypeName == "Story");
    CHECK(r.Rows[0].Draft.FieldValues.count("summary") == 1);
    CHECK(r.Rows[0].Draft.FieldValues.at("summary") == "First issue");
    CHECK(r.Rows[1].Draft.IssueTypeName == "Bug");
}

TEST_CASE("ParseDrafts CSV: fallback project/issuetype applied when columns absent") {
    const std::string csv = "summary\nMy task\n";
    std::vector<TrackerField> cat;
    ImportResult r = ParseDrafts(csv, Format::Csv, cat, "DEFAULT", "10000", "Task");
    REQUIRE(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.ProjectKey == "DEFAULT");
    CHECK(r.Rows[0].Draft.IssueTypeId == "10000");
    CHECK(r.Rows[0].Draft.IssueTypeName == "Task");
}

TEST_CASE("ParseDrafts CSV: quoted fields with embedded commas and newlines") {
    // RFC4180: fields with commas or newlines must be quoted; "" is an escaped quote.
    const std::string csv = "summary,description\n\"Hello, world\",\"Line1\nLine2\"\n";
    std::vector<TrackerField> cat;
    ImportResult r = ParseDrafts(csv, Format::Csv, cat, "P", "", "");
    REQUIRE(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.FieldValues.at("summary") == "Hello, world");
    CHECK(r.Rows[0].Draft.FieldValues.at("description") == "Line1\nLine2");
}

TEST_CASE("ParseDrafts CSV: empty trailing lines are skipped") {
    const std::string csv = "summary\nFoo\n\n\n";
    std::vector<TrackerField> cat;
    ImportResult r = ParseDrafts(csv, Format::Csv, cat, "P", "", "");
    // Trailing blank lines should be ignored.
    CHECK(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.FieldValues.at("summary") == "Foo");
}

TEST_CASE("ParseDrafts CSV: empty input returns error") {
    ImportResult r = ParseDrafts("", Format::Csv, {}, "", "", "");
    CHECK_FALSE(r.Error.empty());
    CHECK(r.Rows.empty());
}

TEST_CASE("ParseDrafts CSV: key/issuekey column maps to ExistingIssueKey") {
    const std::string csv = "key,summary\nABC-123,Update existing\n";
    ImportResult r = ParseDrafts(csv, Format::Csv, {}, "P", "", "");
    REQUIRE(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.ExistingIssueKey == "ABC-123");
    CHECK(r.Rows[0].Draft.FieldValues.at("summary") == "Update existing");
}

// ===========================================================================
// ParseDrafts — TSV
// ===========================================================================

TEST_CASE("ParseDrafts TSV: tab-delimited import works") {
    const std::string tsv = "project\tissuetype\tsummary\nPRJ\tBug\tA bug report\n";
    ImportResult r = ParseDrafts(tsv, Format::Tsv, {}, "", "", "");
    CHECK(r.DetectedFormat == Format::Tsv);
    REQUIRE(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.ProjectKey == "PRJ");
    CHECK(r.Rows[0].Draft.IssueTypeName == "Bug");
    CHECK(r.Rows[0].Draft.FieldValues.at("summary") == "A bug report");
}

// ===========================================================================
// ParseDrafts — Auto format detection
// ===========================================================================

TEST_CASE("ParseDrafts Auto: sniffs CSV and TSV correctly") {
    const std::string csv = "summary\nFoo\n";
    const std::string tsv = "summary\tproject\nFoo\tPRJ\n";

    ImportResult rc = ParseDrafts(csv, Format::Auto, {}, "P", "", "");
    CHECK(rc.DetectedFormat == Format::Csv);
    REQUIRE(rc.Rows.size() == 1);

    ImportResult rt = ParseDrafts(tsv, Format::Auto, {}, "P", "", "");
    CHECK(rt.DetectedFormat == Format::Tsv);
    REQUIRE(rt.Rows.size() == 1);
}

// ===========================================================================
// ParseDrafts — JSON
// ===========================================================================

TEST_CASE("ParseDrafts JSON: array of objects") {
    const std::string json =
        "[{\"project\":\"PRJ\",\"issuetype\":\"Story\",\"summary\":\"From JSON\"}]";
    ImportResult r = ParseDrafts(json, Format::Json, {}, "", "", "");
    CHECK(r.DetectedFormat == Format::Json);
    REQUIRE(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.ProjectKey == "PRJ");
    CHECK(r.Rows[0].Draft.IssueTypeName == "Story");
}

TEST_CASE("ParseDrafts JSON: {\"issues\":[...]} wrapper") {
    const std::string json = "{\"issues\":[{\"summary\":\"Wrapped\"}]}";
    ImportResult r = ParseDrafts(json, Format::Json, {}, "P", "", "");
    REQUIRE(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.FieldValues.at("summary") == "Wrapped");
}

TEST_CASE("ParseDrafts JSON: single object treated as array of one") {
    const std::string json = "{\"summary\":\"Single object\"}";
    ImportResult r = ParseDrafts(json, Format::Json, {}, "P", "", "");
    REQUIRE(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.FieldValues.at("summary") == "Single object");
}

TEST_CASE("ParseDrafts JSON: \"fields\" wrapper is merged with top-level keys") {
    const std::string json = "[{\"key\":\"ABC-1\",\"fields\":{\"summary\":\"Wrapped fields\"}}]";
    ImportResult r = ParseDrafts(json, Format::Json, {}, "P", "", "");
    REQUIRE(r.Rows.size() == 1);
    CHECK(r.Rows[0].Draft.ExistingIssueKey == "ABC-1");
    CHECK(r.Rows[0].Draft.FieldValues.at("summary") == "Wrapped fields");
}

TEST_CASE("ParseDrafts JSON: parse error returns non-empty Error") {
    ImportResult r = ParseDrafts("{invalid json}", Format::Json, {}, "", "", "");
    CHECK_FALSE(r.Error.empty());
    CHECK(r.Rows.empty());
}

TEST_CASE("ParseDrafts JSON: non-array non-object root returns error") {
    ImportResult r = ParseDrafts("42", Format::Json, {}, "", "", "");
    CHECK_FALSE(r.Error.empty());
}

// ===========================================================================
// SerializeTickets
// ===========================================================================

TEST_CASE("SerializeTickets CSV: header row + data rows with correct commas") {
    std::vector<CachedTicket> tickets = {MakeTicket("PRJ-1", "summary", "Fix it")};
    const std::string out = SerializeTickets(tickets, {"summary"}, Format::Csv);
    // CSV output must start with "key,summary\n"
    CHECK(out.find("key,summary") != std::string::npos);
    CHECK(out.find("PRJ-1,Fix it") != std::string::npos);
}

TEST_CASE("SerializeTickets CSV: field values with commas are quoted") {
    std::vector<CachedTicket> tickets = {MakeTicket("PRJ-1", "summary", "Hello, world")};
    const std::string out = SerializeTickets(tickets, {"summary"}, Format::Csv);
    // "Hello, world" must be double-quoted in the output.
    CHECK(out.find("\"Hello, world\"") != std::string::npos);
}

TEST_CASE("SerializeTickets TSV: values separated by tab") {
    std::vector<CachedTicket> tickets = {MakeTicket("PRJ-1", "summary", "A note")};
    const std::string out = SerializeTickets(tickets, {"summary"}, Format::Tsv);
    CHECK(out.find("key\tsummary") != std::string::npos);
    CHECK(out.find("PRJ-1\tA note") != std::string::npos);
}

TEST_CASE("SerializeTickets JSON: valid JSON array output") {
    std::vector<CachedTicket> tickets = {MakeTicket("PRJ-1", "summary", "From JSON export")};
    const std::string out = SerializeTickets(tickets, {"summary"}, Format::Json);
    CHECK(out.find("PRJ-1") != std::string::npos);
    CHECK(out.find("From JSON export") != std::string::npos);
    // Must be valid JSON (starts with [)
    // Find first non-whitespace
    size_t pos = out.find_first_not_of(" \t\n\r");
    REQUIRE(pos != std::string::npos);
    CHECK(out[pos] == '[');
}

TEST_CASE("SerializeTickets: empty columns parameter uses union of all field keys") {
    std::vector<CachedTicket> t1 = {MakeTicket("PRJ-1", "summary", "One")};
    t1[0].fieldValues["priority"] = "High";
    const std::string out = SerializeTickets(t1, {}, Format::Csv);
    // Both "summary" and "priority" should appear in the header
    CHECK(out.find("priority") != std::string::npos);
    CHECK(out.find("summary") != std::string::npos);
}

TEST_CASE("SerializeTickets: empty ticket list produces header-only CSV") {
    std::vector<CachedTicket> empty;
    const std::string out = SerializeTickets(empty, {"summary"}, Format::Csv);
    CHECK(out.find("key,summary") != std::string::npos);
    // After the header newline there must be no ticket data rows.
    const size_t nl = out.find('\n');
    REQUIRE(nl != std::string::npos);
    // The remainder after the header line is either absent (no trailing \n) or empty.
    const std::string remainder = out.substr(nl + 1);
    const bool noDataRows = remainder.empty();
    CHECK(noDataRows);
}

TEST_CASE("SerializeTickets: Auto format defaults to CSV") {
    std::vector<CachedTicket> tickets = {MakeTicket("A-1", "summary", "Test")};
    const std::string out = SerializeTickets(tickets, {"summary"}, Format::Auto);
    CHECK(out.find("key,summary") != std::string::npos);
}

TEST_CASE("ParseDrafts: JSON depth bomb is rejected by the bounded import parse (no deep DOM)") {
    // Import files are user-chosen but often externally-originated bytes, so the JSON import
    // routes through json_safe::ParseBounded with import-sized caps: a deeply-nested payload
    // must come back as a clean ImportResult error, not build a deep DOM whose recursive ~json
    // teardown can overflow the stack.
    std::string bomb;
    for (int i = 0; i < 300; ++i) bomb += "[";
    for (int i = 0; i < 300; ++i) bomb += "]";
    const ImportResult r = ParseDrafts(bomb, Format::Json, {}, "PROJ", "10001", "Task");
    CHECK(r.Rows.empty());
    CHECK(r.Error.find("JSON parse error") != std::string::npos);
}

TEST_CASE("ParseDrafts: well-formed JSON import still parses under the bounded caps") {
    const std::string text = "[{\"summary\":\"From import\"}]";
    const ImportResult r = ParseDrafts(text, Format::Json, {}, "PROJ", "10001", "Task");
    CHECK(r.Error.empty());
    REQUIRE(r.Rows.size() == 1);
}
