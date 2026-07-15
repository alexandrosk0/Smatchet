// Bucket-A doctest for the pure grid sort + field-classification helpers extracted
// from TicketGridModel.cpp (TicketGridSortPure.{h,cpp}). No ImGui / AppController —
// plain string + field-metadata decisions.

#include "TicketGridSortPure.h"

#include "TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <string>

namespace {

TrackerField Field(const std::string& id, const std::string& type, const std::string& name = "") {
    TrackerField f;
    f.Id = id;
    f.Type = type;
    f.Name = name;
    return f;
}

// Sign of the comparator so tests read as an ordering, not a magnitude.
int Sgn(int v) { return (v > 0) - (v < 0); }

} // namespace

TEST_CASE("CompareFieldValuesForSort — empty values always sort last regardless of direction") {
    // Both empty compare equal.
    CHECK(CompareFieldValuesForSort("summary", nullptr, "", "", /*dir=*/1) == 0);
    // Ascending (dir==1): a non-empty value precedes an empty one.
    CHECK(CompareFieldValuesForSort("summary", nullptr, "", "x", 1) == 1);  // a empty -> a after b
    CHECK(CompareFieldValuesForSort("summary", nullptr, "x", "", 1) == -1); // b empty -> b after a
    // Descending (dir!=1): empty still sorts to the bottom, so the sign flips.
    CHECK(CompareFieldValuesForSort("summary", nullptr, "", "x", 0) == -1);
    CHECK(CompareFieldValuesForSort("summary", nullptr, "x", "", 0) == 1);
}

TEST_CASE("CompareFieldValuesForSort — key/issuekey use natural issue-key order") {
    // PROJ-2 before PROJ-10 (numeric, not lexical).
    CHECK(Sgn(CompareFieldValuesForSort("key", nullptr, "PROJ-2", "PROJ-10", 1)) == -1);
    CHECK(Sgn(CompareFieldValuesForSort("issuekey", nullptr, "PROJ-10", "PROJ-2", 1)) == 1);
    CHECK(CompareFieldValuesForSort("key", nullptr, "PROJ-7", "PROJ-7", 1) == 0);
}

TEST_CASE("CompareFieldValuesForSort — date fields compare lexically on the ISO string") {
    // Known date id.
    CHECK(Sgn(CompareFieldValuesForSort("created", nullptr, "2026-01-01", "2026-07-14", 1)) == -1);
    // date-typed catalog field.
    const TrackerField due = Field("customfield_1", "datetime");
    CHECK(Sgn(CompareFieldValuesForSort("customfield_1", &due, "2026-07-14T09:00", "2026-07-14T08:00", 1)) == 1);
}

TEST_CASE("CompareFieldValuesForSort — numeric fields compare numerically, not lexically") {
    const TrackerField num = Field("story_points", "number");
    // "9" < "10" numerically (lexical would put "10" first).
    CHECK(Sgn(CompareFieldValuesForSort("story_points", &num, "9", "10", 1)) == -1);
    // Non-number field with integer-looking values still parses as int64.
    CHECK(Sgn(CompareFieldValuesForSort("rank", nullptr, "100", "20", 1)) == 1);
    // Non-numeric text falls back to case-insensitive compare.
    CHECK(Sgn(CompareFieldValuesForSort("summary", nullptr, "apple", "Banana", 1)) == -1);
    CHECK(CompareFieldValuesForSort("summary", nullptr, "ABC", "abc", 1) == 0); // case-insensitive equal
}

TEST_CASE("IsTrackerDateOrDateTimeField — ids, catalog type/family, and word heuristics") {
    CHECK(IsTrackerDateOrDateTimeField("created", nullptr));
    CHECK(IsTrackerDateOrDateTimeField("duedate", nullptr));
    // Catalog type / family.
    const TrackerField dt = Field("cf", "datetime");
    CHECK(IsTrackerDateOrDateTimeField("cf", &dt));
    TrackerField fam = Field("cf2", "");
    fam.Family = TrackerFieldFamily::Date;
    CHECK(IsTrackerDateOrDateTimeField("cf2", &fam));
    // Id heuristic (contains "time").
    CHECK(IsTrackerDateOrDateTimeField("start_time", nullptr));
    // Name heuristic on an otherwise-opaque id.
    const TrackerField resolvedOn = Field("customfield_99", "string", "Resolved On");
    CHECK(IsTrackerDateOrDateTimeField("customfield_99", &resolvedOn));
    // Plainly not a date.
    const TrackerField summary = Field("summary", "string", "Summary");
    CHECK_FALSE(IsTrackerDateOrDateTimeField("summary", &summary));
}

TEST_CASE("IsAttachmentFieldId — the attachment column ids, case-insensitive") {
    CHECK(IsAttachmentFieldId("attachment"));
    CHECK(IsAttachmentFieldId("attachments"));
    CHECK(IsAttachmentFieldId("Attachment"));
    CHECK(IsAttachmentFieldId("ATTACHMENTS"));
    CHECK_FALSE(IsAttachmentFieldId("attach"));
    CHECK_FALSE(IsAttachmentFieldId("summary"));
}
