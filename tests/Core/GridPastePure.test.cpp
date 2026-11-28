// Bucket-A doctest for what one pasted grid cell writes (Ui/GridPastePure.h): the option lookup of a choice field,
// labels, dates (typed, stored, and the exact dates of this app's own copy), durations, free text, and the fields
// that take no paste. No ImGui, no network, no SQLite.

#include "CompactDateFormat.h"
#include "Ui/GridPastePure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::grid::paste::CopyRecord;
using smatchet::grid::paste::ExactAt;
using smatchet::grid::paste::IsFreeValueField;
using smatchet::grid::paste::IsOwnCopy;
using smatchet::grid::paste::PasteValueInput;
using smatchet::grid::paste::ResolveCellText;

namespace {

using Values = std::vector<std::string>;

TrackerParseExplain::CivilDate Today() {
    TrackerParseExplain::CivilDate today;
    today.Year = 2026;
    today.Month = 11;
    today.Day = 27;
    return today;
}

TrackerField TextField() {
    TrackerField field;
    field.Id = "summary";
    field.Family = TrackerFieldFamily::Text;
    return field;
}

TrackerField PriorityField() {
    TrackerField field;
    field.Id = "priority";
    field.Family = TrackerFieldFamily::SelectSingle;
    TrackerFieldOption high;
    high.Id = "2";
    high.Value = "High";
    TrackerFieldOption low;
    low.Id = "4";
    low.Value = "Low";
    field.AllowedValueOptions = {high, low};
    return field;
}

TrackerField DateField(const char* type) {
    TrackerField field;
    field.Id = "duedate";
    field.Type = type;
    return field;
}

PasteValueInput In(const std::string& text, const std::string& exact = std::string(),
                   const std::string& current = std::string()) {
    PasteValueInput in;
    in.Text = text;
    in.Exact = exact;
    in.Current = current;
    return in;
}

// The values `text` resolves to; `ok` says whether the cell takes it at all.
Values Resolve(const TrackerField& field, const PasteValueInput& in, bool* ok) {
    Values out;
    *ok = ResolveCellText(field, in, Today(), out);
    return out;
}

} // namespace

TEST_CASE("IsOwnCopy: the clipboard still holds the copied text, carriage returns aside") {
    CopyRecord copy;
    copy.Tsv = "a\tb\nc\td\n";
    CHECK(IsOwnCopy(copy, "a\tb\nc\td\n"));
    CHECK(IsOwnCopy(copy, "a\tb\r\nc\td\r\n")); // the OS clipboard may hand LF back as CRLF
    CHECK_FALSE(IsOwnCopy(copy, "a\tb\nc\tx\n"));
    CHECK_FALSE(IsOwnCopy(CopyRecord(), "")); // nothing was copied yet
}

TEST_CASE("ExactAt: the stored date of a copied cell, empty outside the copy") {
    CopyRecord copy;
    copy.Exact = {{"", "2026-01-05"}, {"2026-02-06"}};
    CHECK(ExactAt(copy, 0, 1) == "2026-01-05");
    CHECK(ExactAt(copy, 1, 0) == "2026-02-06");
    CHECK(ExactAt(copy, 0, 0).empty());
    CHECK(ExactAt(copy, 1, 1).empty()); // a ragged row
    CHECK(ExactAt(copy, 2, 0).empty());
    CHECK(ExactAt(copy, -1, 0).empty());
    CHECK(ExactAt(copy, 0, -1).empty());
}

TEST_CASE("IsFreeValueField: text, number and duration with no option list") {
    CHECK(IsFreeValueField(TextField()));
    TrackerField number;
    number.Id = "customfield_1";
    number.Family = TrackerFieldFamily::Number;
    CHECK(IsFreeValueField(number));
    TrackerField estimate;
    estimate.Id = "timeoriginalestimate";
    CHECK(IsFreeValueField(estimate));
    CHECK_FALSE(IsFreeValueField(PriorityField()));
    TrackerField lazySelect; // an option list that fills in later is not free text
    lazySelect.Id = "customfield_2";
    lazySelect.Family = TrackerFieldFamily::SelectSingle;
    CHECK_FALSE(IsFreeValueField(lazySelect));
}

TEST_CASE("ResolveCellText: free text is written as pasted, an empty text clears") {
    bool ok = false;
    CHECK(Resolve(TextField(), In(" two words "), &ok) == Values{" two words "});
    CHECK(ok);
    CHECK(Resolve(TextField(), In(""), &ok) == Values{""});
    CHECK(ok);
}

TEST_CASE("ResolveCellText: a number column takes a plain number, trimmed, and refuses other text") {
    bool ok = false;
    TrackerField points;
    points.Id = "customfield_1";
    points.Family = TrackerFieldFamily::Number;
    CHECK(Resolve(points, In(" 3.5 "), &ok) == Values{"3.5"});
    CHECK(ok);
    CHECK(Resolve(points, In("-2"), &ok) == Values{"-2"});
    CHECK(ok);
    CHECK(Resolve(points, In(""), &ok) == Values{""}); // clears the number
    CHECK(ok);
    // A pasted header row or a note is not a number: the cell is skipped, never sent as text.
    CHECK(Resolve(points, In("Story Points"), &ok).empty());
    CHECK_FALSE(ok);
    CHECK(Resolve(points, In("N/A"), &ok).empty());
    CHECK_FALSE(ok);
    CHECK(Resolve(points, In("1.2.3"), &ok).empty());
    CHECK_FALSE(ok);
    CHECK(Resolve(points, In("-"), &ok).empty());
    CHECK_FALSE(ok);
    CHECK(Resolve(points, In("1e3"), &ok).empty());
    CHECK_FALSE(ok);
}

TEST_CASE("ResolveCellText: read-only, rich-text and option-less choice fields take no paste") {
    bool ok = true;
    TrackerField readOnly = TextField();
    readOnly.ReadOnly = true;
    CHECK(Resolve(readOnly, In("x"), &ok).empty());
    CHECK_FALSE(ok);

    TrackerField description;
    description.Id = "description";
    description.Family = TrackerFieldFamily::Text;
    CHECK(Resolve(description, In("x"), &ok).empty());
    CHECK_FALSE(ok);

    TrackerField lazySelect;
    lazySelect.Id = "customfield_2";
    lazySelect.Family = TrackerFieldFamily::SelectSingle;
    CHECK(Resolve(lazySelect, In("x"), &ok).empty());
    CHECK_FALSE(ok);

    TrackerField cascading = PriorityField();
    cascading.Family = TrackerFieldFamily::CascadingSelect;
    CHECK(Resolve(cascading, In("High"), &ok).empty());
    CHECK_FALSE(ok);
}

TEST_CASE("ResolveCellText: a choice resolves to its option id, and a name not in the list is refused") {
    bool ok = false;
    CHECK(Resolve(PriorityField(), In(" High "), &ok) == Values{"2"});
    CHECK(ok);
    CHECK(Resolve(PriorityField(), In("Urgent"), &ok).empty());
    CHECK_FALSE(ok); // never written as raw text
    // An empty text clears a choice.
    CHECK(Resolve(PriorityField(), In(""), &ok).empty());
    CHECK(ok);
}

TEST_CASE("ResolveCellText: a ticket always has a status and a type, so an empty text is refused there") {
    bool ok = true;
    TrackerField status = PriorityField();
    status.Id = "status";
    status.Family = TrackerFieldFamily::Status;
    CHECK(Resolve(status, In("  "), &ok).empty());
    CHECK_FALSE(ok);
    TrackerField type = PriorityField();
    type.Id = "issuetype";
    type.Family = TrackerFieldFamily::IssueType;
    CHECK(Resolve(type, In(""), &ok).empty());
    CHECK_FALSE(ok);
    CHECK(Resolve(status, In("Low"), &ok) == Values{"4"});
    CHECK(ok);
}

TEST_CASE("ResolveCellText: labels are free entries, a multi choice needs every entry in its list") {
    bool ok = false;
    TrackerField labels;
    labels.Id = "labels";
    CHECK(Resolve(labels, In(" ui , backend ,,"), &ok) == Values{"ui", "backend"});
    CHECK(ok);

    TrackerField multi = PriorityField();
    multi.Id = "customfield_3";
    multi.Family = TrackerFieldFamily::SelectMulti;
    multi.IsArray = true;
    const Values both = Resolve(multi, In("High, Low"), &ok);
    CHECK(ok);
    CHECK(both.size() == 2);
    CHECK(Resolve(multi, In("High, Urgent"), &ok).empty());
    CHECK_FALSE(ok);

    // A components list that has not been fetched yet resolves nothing: the cell is skipped, not written raw.
    TrackerField lazyComponents;
    lazyComponents.Id = "components";
    lazyComponents.IsArray = true;
    lazyComponents.ItemsType = "component";
    CHECK(Resolve(lazyComponents, In("Backend"), &ok).empty());
    CHECK_FALSE(ok);
}

TEST_CASE("ResolveCellText: a date reads the stored form and the typed forms, and refuses what is not a day") {
    bool ok = false;
    const TrackerField due = DateField("date");
    CHECK(Resolve(due, In("2026-03-04"), &ok) == Values{"2026-03-04"});
    CHECK(ok);
    CHECK(Resolve(due, In("Mar 4"), &ok) == Values{"2026-03-04"});
    CHECK(ok);
    CHECK(Resolve(due, In("tomorrow"), &ok) == Values{"2026-11-28"});
    CHECK(ok);
    // A stored date-time pasted onto a date field keeps its day.
    CHECK(Resolve(due, In("2026-03-04T10:20:30.000+0000"), &ok) == Values{"2026-03-04"});
    CHECK(ok);
    CHECK(Resolve(due, In("not a date"), &ok).empty());
    CHECK_FALSE(ok);
    CHECK(Resolve(due, In("2026-02-30"), &ok).empty()); // no such day
    CHECK_FALSE(ok);
    // An empty text clears the date.
    CHECK(Resolve(due, In("  "), &ok).empty());
    CHECK(ok);
}

TEST_CASE("ResolveCellText: the display form of an old date is read from this app's own copy, never guessed") {
    bool ok = true;
    const TrackerField due = DateField("date");
    // The grid shows a far date as a signed offset from the day it was drawn: the text alone no longer says which day.
    CHECK(Resolve(due, In("-2w"), &ok).empty());
    CHECK_FALSE(ok);
    CHECK(Resolve(due, In("+3d"), &ok).empty());
    CHECK_FALSE(ok);
    // The same text with the stored date behind it (CopyRecord) pastes that date.
    CHECK(Resolve(due, In("-2w", "2026-01-05"), &ok) == Values{"2026-01-05"});
    CHECK(ok);
    CHECK(Resolve(due, In("Jan 5 '26", "2026-01-05"), &ok) == Values{"2026-01-05"});
    CHECK(ok);
}

TEST_CASE("ResolveCellText: a day pasted onto a date-time keeps the cell's time of day, or gets noon UTC") {
    bool ok = false;
    const TrackerField updated = DateField("datetime");
    const std::string current = "2026-01-01T09:30:00.000+0200";
    const Values kept = Resolve(updated, In("2026-03-04", std::string(), current), &ok);
    REQUIRE(ok);
    REQUIRE(kept.size() == 1);
    CHECK(kept.front().compare(0, 23, "2026-03-04T09:30:00.000") == 0);
    // The zone of the cell is kept with its time.
    ParsedJiraDateTime parsed;
    REQUIRE(TryParseJiraDateTime(kept.front(), parsed));
    CHECK(parsed.HasTimeZoneSuffix);
    CHECK(parsed.OffsetSec == 2 * 3600);

    CHECK(Resolve(updated, In("2026-03-04"), &ok) == Values{"2026-03-04T12:00:00.000Z"});
    CHECK(ok);

    // A copied date-time carries its own time: nothing of the target cell is mixed in.
    const std::string exact = "2026-05-06T07:08:09.000+0000";
    ParsedJiraDateTime exactParsed;
    REQUIRE(TryParseJiraDateTime(exact, exactParsed));
    CHECK(Resolve(updated, In("-6mo", exact, current), &ok) ==
          Values{FormatJiraDateOrDateTimeForApi(false, exactParsed)});
    CHECK(ok);
}

TEST_CASE("ResolveCellText: a duration is read the tolerant way and written in its canonical spelling") {
    bool ok = false;
    TrackerField estimate;
    estimate.Id = "timeoriginalestimate";
    CHECK(Resolve(estimate, In("90 m"), &ok) == Values{"1h 30m"});
    CHECK(ok);
    CHECK(Resolve(estimate, In("1h 30m"), &ok) == Values{"1h 30m"});
    CHECK(ok);
    CHECK(Resolve(estimate, In("soon"), &ok).empty());
    CHECK_FALSE(ok);
    CHECK(Resolve(estimate, In(""), &ok) == Values{""}); // clears the estimate
    CHECK(ok);
}
