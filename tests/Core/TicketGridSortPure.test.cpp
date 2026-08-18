// Bucket-A doctest for the pure grid sort + field-classification helpers extracted
// from TicketGridModel.cpp (TicketGridSortPure.{h,cpp}). No ImGui / AppController —
// plain string + field-metadata decisions.

#include "TicketGridSortPure.h"

#include "TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

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

TEST_CASE("CompareFieldValuesForSort — non-finite number cells never claim a numeric order") {
    const TrackerField num = Field("story_points", "number");
    // strtod whole-string-matches these; two NaN cells used to report "unequal but neither
    // smaller" (+1 both ways). They must now compare as equivalent / consistently ordered.
    CHECK(CompareFieldValuesForSort("story_points", &num, "nan", "nan", 1) == 0);
    CHECK(CompareFieldValuesForSort("story_points", &num, "nan", "NaN", 1) == 0); // case-insensitive fallback
    CHECK(Sgn(CompareFieldValuesForSort("story_points", &num, "nan", "inf", 1)) ==
          -Sgn(CompareFieldValuesForSort("story_points", &num, "inf", "nan", 1)));
    CHECK(CompareFieldValuesForSort("story_points", &num, "inf", "inf", 1) == 0);
    // Finite values keep comparing numerically.
    CHECK(Sgn(CompareFieldValuesForSort("story_points", &num, "2", "10", 1)) == -1);
}

TEST_CASE("CompareFieldValuesForSort — mixed number/text column is a strict weak ordering") {
    const TrackerField num = Field("story_points", "number");
    // The grid's stable_sort predicate: SmatchetActiveProjectGridTable feeds the comparator
    // through `(cmp * dir) < 0`. Exercise that exact shape over a value set that MIXES the
    // parseable and unparseable classes — a comparator that orders some pairs numerically
    // and others lexically is precisely what cycles, and a set of clean numbers cannot see it.
    // Load-bearing members: "5x"/"zz"/"nan" don't parse yet land lexically BETWEEN parseable
    // neighbours (the #2079 cycle "5x" < "9" < "10" < "5x"); "-1"/"-0"/"+5"/" 7" put the
    // sign/whitespace bytes on both sides of the digits, where byte order and numeric order
    // disagree; "1"/"1.0"/"01" are numerically equal but textually distinct, so they probe
    // the tie-break; "-inf"/"inf" stay in the numeric class.
    const std::vector<std::string> values = {"",   "-inf", "-1", "-0", "0",  "+5",  " 7",  "1",   "1.0",
                                             "01", "2",    "3",  "10", "5x", "inf", "nan", "NaN", "zz"};
    // Both column shapes: number-typed (strtod path) and untyped (strtoll path).
    const TrackerField* const metas[2] = {&num, nullptr};
    const int dirs[2] = {1, -1};
    for (int m = 0; m < 2; ++m) {
        const TrackerField* const meta = metas[m];
        for (int d = 0; d < 2; ++d) {
            const int dir = dirs[d];
            auto less = [meta, dir](const std::string& x, const std::string& y) {
                return (CompareFieldValuesForSort("story_points", meta, x, y, dir) * dir) < 0;
            };
            for (size_t i = 0; i < values.size(); ++i) {
                CHECK_FALSE(less(values[i], values[i])); // irreflexive
                for (size_t j = 0; j < values.size(); ++j) {
                    if (less(values[i], values[j])) {
                        CHECK_FALSE(less(values[j], values[i])); // asymmetric
                    }
                    for (size_t k = 0; k < values.size(); ++k) {
                        if (less(values[i], values[j]) && less(values[j], values[k])) {
                            CHECK(less(values[i], values[k])); // transitive
                        }
                        // Transitivity of equivalence (the half a NaN comparator classically breaks).
                        const bool eqIJ = !less(values[i], values[j]) && !less(values[j], values[i]);
                        const bool eqJK = !less(values[j], values[k]) && !less(values[k], values[j]);
                        if (eqIJ && eqJK) {
                            CHECK(!less(values[i], values[k]));
                            CHECK(!less(values[k], values[i]));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("CompareFieldValuesForSort — numbers sort as a class ahead of unparseable text") {
    // #2079: with the numeric path taken only when BOTH cells parsed, a mixed column
    // compared "5x" vs "9" lexically ('5' < '9'), "9" vs "10" numerically (9 < 10) and
    // "10" vs "5x" lexically again ('1' < '5') — a < b < c < a. Parseability is now the
    // primary key, so a cell never compares numerically with one peer and lexically
    // with another.
    CHECK(Sgn(CompareFieldValuesForSort("rank", nullptr, "9", "10", 1)) == -1);
    CHECK(Sgn(CompareFieldValuesForSort("rank", nullptr, "9", "5x", 1)) == -1);
    CHECK(Sgn(CompareFieldValuesForSort("rank", nullptr, "10", "5x", 1)) == -1);
    // Same on the number-typed (strtod) path.
    const TrackerField num = Field("story_points", "number");
    CHECK(Sgn(CompareFieldValuesForSort("story_points", &num, "3.5", "n/a", 1)) == -1);
    CHECK(Sgn(CompareFieldValuesForSort("story_points", &num, "n/a", "3.5", 1)) == 1);
    // Text-only pairs keep the plain case-insensitive order.
    CHECK(Sgn(CompareFieldValuesForSort("rank", nullptr, "5x", "zz", 1)) == -1);
    // A numeric tie falls through to the text compare, so equal-valued spellings stay
    // ordered against each other instead of collapsing into one equivalence class.
    CHECK(Sgn(CompareFieldValuesForSort("story_points", &num, "1", "1.0", 1)) == -1);
    CHECK(Sgn(CompareFieldValuesForSort("story_points", &num, "1.0", "1", 1)) == 1);
    CHECK(Sgn(CompareFieldValuesForSort("rank", nullptr, "01", "1", 1)) == -1);
}

TEST_CASE("IsTrackerDateOrDateTimeField — time-tracking duration ids are not dates") {
    // Raw-seconds duration columns; the "time" word heuristic used to swallow all of them.
    CHECK_FALSE(IsTrackerDateOrDateTimeField("timespent", nullptr));
    CHECK_FALSE(IsTrackerDateOrDateTimeField("timeestimate", nullptr));
    CHECK_FALSE(IsTrackerDateOrDateTimeField("timeoriginalestimate", nullptr));
    CHECK_FALSE(IsTrackerDateOrDateTimeField("aggregatetimespent", nullptr));
    CHECK_FALSE(IsTrackerDateOrDateTimeField("aggregatetimeestimate", nullptr));
    CHECK_FALSE(IsTrackerDateOrDateTimeField("aggregatetimeoriginalestimate", nullptr));
    // Catalog metadata must not resurrect them either — the id decides first.
    const TrackerField spent = Field("timespent", "number", "Time Spent");
    CHECK_FALSE(IsTrackerDateOrDateTimeField("timespent", &spent));
    // A genuinely date-ish "time" id is still a date.
    CHECK(IsTrackerDateOrDateTimeField("start_time", nullptr));
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
