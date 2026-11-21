// Bucket-A doctest for the pure core of the bulk field edit (Ui/BulkFieldEditPure.h): the Add / Remove
// merge, the summary counts, the mixed-value label, the confirm threshold, the field classification and the
// shared ADR-0016 scalar-base capture. No ImGui, no network, no SQLite.

#include "SpreadsheetState.h"
#include "Ui/BulkFieldEditPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace smatchet::grid::bulk;

namespace {

BulkRow MakeRow(const std::string& id, const std::string& before, BulkRowState state = BulkRowState::Pending) {
    BulkRow row;
    row.IssueId = id;
    row.BeforeValue = before;
    row.State = state;
    return row;
}

struct FakeEdit {
    std::string OriginalValue;
    bool HasOriginalValue = false;
};

} // namespace

TEST_CASE("ComputeValuesForRow Replace keeps the picked values, deduplicated and sorted") {
    CHECK(ComputeValuesForRow(BulkMode::Replace, {"x"}, {"b", "a", "b"}) == std::vector<std::string>{"a", "b"});
    CHECK(ComputeValuesForRow(BulkMode::Replace, {"x"}, {}).empty());
}

TEST_CASE("ComputeValuesForRow Add merges the picked values into the before-value") {
    CHECK(ComputeValuesForRow(BulkMode::Add, {"b"}, {"a", "c"}) == std::vector<std::string>{"a", "b", "c"});
    // Duplicates collapse, including a pick the row already holds.
    CHECK(ComputeValuesForRow(BulkMode::Add, {"a", "b"}, {"b", "b"}) == std::vector<std::string>{"a", "b"});
    // An empty before-value (the row had nothing) takes the picks as they are.
    CHECK(ComputeValuesForRow(BulkMode::Add, {}, {"z", "y"}) == std::vector<std::string>{"y", "z"});
}

TEST_CASE("ComputeValuesForRow Remove drops the picked values from the before-value") {
    CHECK(ComputeValuesForRow(BulkMode::Remove, {"a", "b", "c"}, {"b"}) == std::vector<std::string>{"a", "c"});
    // Removing something the row does not hold changes nothing.
    CHECK(ComputeValuesForRow(BulkMode::Remove, {"a"}, {"q"}) == std::vector<std::string>{"a"});
    // An empty before-value stays empty; removing every value leaves nothing.
    CHECK(ComputeValuesForRow(BulkMode::Remove, {}, {"a"}).empty());
    CHECK(ComputeValuesForRow(BulkMode::Remove, {"a", "b"}, {"a", "b"}).empty());
}

TEST_CASE("ComputeValuesForRow drops empty entries") {
    CHECK(ComputeValuesForRow(BulkMode::Add, {""}, {"a", ""}) == std::vector<std::string>{"a"});
}

TEST_CASE("ComputeValuesForRow compares ignoring case only when asked (labels)") {
    CHECK(ComputeValuesForRow(BulkMode::Add, {"Bug"}, {"bug"}, true) == std::vector<std::string>{"Bug"});
    CHECK(ComputeValuesForRow(BulkMode::Add, {"Bug"}, {"bug"}, false) == std::vector<std::string>{"Bug", "bug"});
    CHECK(ComputeValuesForRow(BulkMode::Remove, {"Bug", "ui"}, {"BUG"}, true) == std::vector<std::string>{"ui"});
    CHECK(ComputeValuesForRow(BulkMode::Remove, {"Bug", "ui"}, {"BUG"}, false) ==
          std::vector<std::string>{"Bug", "ui"});
}

TEST_CASE("Summarize counts each state and reports Done only when nothing is pending or saving") {
    BulkFieldEditJob job;
    job.Rows.push_back(MakeRow("A-1", "x", BulkRowState::Saved));
    job.Rows.push_back(MakeRow("A-2", "x", BulkRowState::Queued));
    job.Rows.push_back(MakeRow("A-3", "x", BulkRowState::Failed));
    job.Rows.push_back(MakeRow("A-4", "x", BulkRowState::Skipped));
    job.Rows.push_back(MakeRow("A-5", "x", BulkRowState::Saving));
    BulkSummary s = Summarize(job);
    CHECK(s.Total == 5);
    CHECK(s.Saved == 1);
    CHECK(s.Queued == 1);
    CHECK(s.Failed == 1);
    CHECK(s.Skipped == 1);
    CHECK(s.Remaining == 1);
    CHECK_FALSE(s.Done);

    job.Rows[4].State = BulkRowState::Saved;
    s = Summarize(job);
    CHECK(s.Saved == 2);
    CHECK(s.Remaining == 0);
    CHECK(s.Done);
}

TEST_CASE("Summarize of an empty job is Done with zero rows") {
    const BulkSummary s = Summarize(BulkFieldEditJob());
    CHECK(s.Total == 0);
    CHECK(s.Done);
}

TEST_CASE("SetRowState finds a row by issue id and keeps the first final outcome") {
    BulkFieldEditJob job;
    job.Rows.push_back(MakeRow("A-1", "x"));
    job.Rows.push_back(MakeRow("A-2", "x"));
    CHECK(SetRowState(job, "A-2", BulkRowState::Failed, "boom"));
    CHECK(job.Rows[1].State == BulkRowState::Failed);
    CHECK(job.Rows[1].Message == "boom");
    // A late duplicate report cannot flip a final row.
    CHECK(SetRowState(job, "A-2", BulkRowState::Saved, ""));
    CHECK(job.Rows[1].State == BulkRowState::Failed);
    CHECK(job.Rows[0].State == BulkRowState::Pending);
    // An unknown id is reported, not applied.
    CHECK_FALSE(SetRowState(job, "A-9", BulkRowState::Saved, ""));
}

TEST_CASE("MixedValueLabel names the single value or the number of distinct values") {
    BulkFieldEditJob job;
    CHECK(MixedValueLabel(job).empty());
    CHECK(DistinctBeforeValueCount(job) == 0);

    job.Rows.push_back(MakeRow("A-1", "Alice"));
    job.Rows.push_back(MakeRow("A-2", "Alice"));
    CHECK(DistinctBeforeValueCount(job) == 1);
    CHECK(MixedValueLabel(job) == "Alice");

    job.Rows.push_back(MakeRow("A-3", "Bob"));
    job.Rows.push_back(MakeRow("A-4", ""));
    CHECK(DistinctBeforeValueCount(job) == 3);
    CHECK(MixedValueLabel(job) == "mixed (3 values)");

    // Skipped rows do not count: the label describes the rows that will be edited.
    job.Rows[2].State = BulkRowState::Skipped;
    job.Rows[3].State = BulkRowState::Skipped;
    CHECK(MixedValueLabel(job) == "Alice");
}

TEST_CASE("NeedsConfirm is true only above the threshold") {
    CHECK_FALSE(NeedsConfirm(10, 10));
    CHECK(NeedsConfirm(11, 10));
    CHECK_FALSE(NeedsConfirm(0, 10));
    CHECK_FALSE(NeedsConfirm(kConfirmThreshold));
    CHECK(NeedsConfirm(kConfirmThreshold + 1));
}

TEST_CASE("CaptureScalarBase records the display value and marks the base as captured, even when blank") {
    FakeEdit edit;
    CaptureScalarBase(std::string("In Progress"), edit);
    CHECK(edit.OriginalValue == "In Progress");
    CHECK(edit.HasOriginalValue);

    FakeEdit blank;
    CaptureScalarBase(std::string(), blank);
    CHECK(blank.OriginalValue.empty());
    CHECK(blank.HasOriginalValue);
}

TEST_CASE("ClassifyBulkField follows the grid editor dispatch for choice fields") {
    const auto option = [](const char* id) {
        TrackerFieldOption o;
        o.Id = id;
        o.Value = id;
        return o;
    };
    TrackerField single;
    single.Id = "priority";
    single.Family = TrackerFieldFamily::SelectSingle;
    single.AllowedValueOptions.push_back(option("1"));
    CHECK(ClassifyBulkField(single) == BulkFieldKind::SingleChoice);

    TrackerField user = single;
    user.Id = "assignee";
    user.Family = TrackerFieldFamily::UserSingle;
    CHECK(ClassifyBulkField(user) == BulkFieldKind::SingleChoice);

    TrackerField status = single;
    status.Id = "status";
    status.Family = TrackerFieldFamily::Status;
    CHECK(ClassifyBulkField(status) == BulkFieldKind::SingleChoice);

    TrackerField multi = single;
    multi.Id = "fixVersions";
    multi.Family = TrackerFieldFamily::SelectMulti;
    CHECK(ClassifyBulkField(multi) == BulkFieldKind::MultiChoice);
    CHECK(IsMultiValueKind(BulkFieldKind::MultiChoice));

    TrackerField components;
    components.Id = "components";
    components.IsArray = true;
    components.ItemsType = "component";
    CHECK(ClassifyBulkField(components) == BulkFieldKind::MultiChoice); // empty list, filled in lazily

    TrackerField labels;
    labels.Id = "Labels";
    CHECK(ClassifyBulkField(labels) == BulkFieldKind::Labels);
    CHECK(IsMultiValueKind(BulkFieldKind::Labels));
    CHECK_FALSE(IsMultiValueKind(BulkFieldKind::SingleChoice));
}

TEST_CASE("ClassifyBulkField refuses read-only, cascading and free-text fields") {
    TrackerField readOnly;
    readOnly.Id = "priority";
    readOnly.Family = TrackerFieldFamily::SelectSingle;
    readOnly.ReadOnly = true;
    TrackerFieldOption o;
    o.Id = "1";
    readOnly.AllowedValueOptions.push_back(o);
    CHECK(ClassifyBulkField(readOnly) == BulkFieldKind::Unsupported);

    TrackerField cascading = readOnly;
    cascading.ReadOnly = false;
    cascading.Family = TrackerFieldFamily::CascadingSelect;
    CHECK(ClassifyBulkField(cascading) == BulkFieldKind::Unsupported);

    TrackerField text;
    text.Id = "summary";
    text.Family = TrackerFieldFamily::Text;
    CHECK(ClassifyBulkField(text) == BulkFieldKind::Unsupported);
}

TEST_CASE("BeforeValuesFor resolves a choice display value to its option id and parses labels") {
    TrackerField priority;
    priority.Id = "priority";
    priority.Family = TrackerFieldFamily::SelectSingle;
    TrackerFieldOption high;
    high.Id = "2";
    high.Value = "High";
    priority.AllowedValueOptions.push_back(high);
    CHECK(BeforeValuesFor(priority, BulkFieldKind::SingleChoice, "High") == std::vector<std::string>{"2"});
    CHECK(BeforeValuesFor(priority, BulkFieldKind::SingleChoice, "").empty());

    TrackerField labels;
    labels.Id = "labels";
    CHECK(BeforeValuesFor(labels, BulkFieldKind::Labels, " ui , backend ,,") ==
          std::vector<std::string>{"ui", "backend"});
}

TEST_CASE("NewValuesForRow keeps the pick for a single choice and merges a multi value") {
    const std::vector<std::string> before = {"a", "b"};
    CHECK(NewValuesForRow(BulkFieldKind::SingleChoice, BulkMode::Add, before, {"z"}) == std::vector<std::string>{"z"});
    CHECK(NewValuesForRow(BulkFieldKind::MultiChoice, BulkMode::Add, before, {"c"}) ==
          std::vector<std::string>{"a", "b", "c"});
    CHECK(NewValuesForRow(BulkFieldKind::MultiChoice, BulkMode::Remove, before, {"a"}) ==
          std::vector<std::string>{"b"});
}

TEST_CASE("AllEntriesResolve is false when any current entry is not in the option list") {
    TrackerField field;
    field.Id = "fixVersions";
    field.Family = TrackerFieldFamily::SelectMulti;
    TrackerFieldOption a;
    a.Id = "10";
    a.Value = "v1";
    field.AllowedValueOptions.push_back(a);
    CHECK(AllEntriesResolve(field, {}));
    CHECK(AllEntriesResolve(field, {"10"}));
    CHECK(AllEntriesResolve(field, {"v1"})); // by display name too
    CHECK_FALSE(AllEntriesResolve(field, {"10", "gone"}));
}

TEST_CASE("BeforeValuesFor keeps an unresolvable multi-choice entry so the caller can skip the row") {
    TrackerField field;
    field.Id = "fixVersions";
    field.Family = TrackerFieldFamily::SelectMulti;
    TrackerFieldOption a;
    a.Id = "10";
    a.Value = "v1";
    field.AllowedValueOptions.push_back(a);
    const std::vector<std::string> before = BeforeValuesFor(field, BulkFieldKind::MultiChoice, "v1, retired");
    CHECK_FALSE(AllEntriesResolve(field, before));
}

TEST_CASE("IsRowUnchanged compares a single choice exactly and a multi value as a normalised set") {
    CHECK(IsRowUnchanged(BulkFieldKind::SingleChoice, {"2"}, {"2"}));
    CHECK_FALSE(IsRowUnchanged(BulkFieldKind::SingleChoice, {"2"}, {"3"}));
    CHECK_FALSE(IsRowUnchanged(BulkFieldKind::SingleChoice, {}, {"3"}));
    // Order and duplicates in the before-value do not make a multi value "different".
    CHECK(IsRowUnchanged(BulkFieldKind::MultiChoice, {"b", "a", "a"}, {"a", "b"}));
    CHECK_FALSE(IsRowUnchanged(BulkFieldKind::MultiChoice, {"a"}, {"a", "b"}));
    // Labels are case-sensitive on the tracker, so a different case is a real change.
    CHECK_FALSE(IsRowUnchanged(BulkFieldKind::Labels, {"UI"}, {"ui"}));
    CHECK(IsRowUnchanged(BulkFieldKind::Labels, {"ui", "ui"}, {"ui"}));
}

TEST_CASE("GridRectSelection keeps selected issue ids across a re-sort") {
    GridRectSelection sel;
    sel.Rows = {0, 2};
    sel.KeepIssueIds = {"A-1", "A-3"};
    sel.KeepRows = sel.Rows;
    CHECK_FALSE(sel.KeepStale());

    // The user made a selection gesture: Rows moved away from what the ids were matched to.
    sel.Rows = {1};
    CHECK(sel.KeepStale());

    // The sort order changed and the selection was cleared: rebuild it from the ids.
    sel.Rows.clear();
    sel.KeepRows = {0, 2};
    const std::vector<std::string> order = {"A-3", "A-2", "A-1", "A-4"};
    const int n = sel.RestoreKeptRows(
        static_cast<int>(order.size()), [&order](int row) -> const std::string& { return order[row]; }, 77);
    CHECK(n == 2);
    CHECK(sel.Rows == std::set<int>{0, 2});
    CHECK(sel.PrimaryRow == 0);
    CHECK(sel.SortSignature == 77);
    CHECK_FALSE(sel.KeepStale());
}

TEST_CASE("GridRectSelection drops the kept ids when none of them is shown any more") {
    GridRectSelection sel;
    sel.KeepIssueIds = {"A-9"};
    const std::vector<std::string> order = {"A-1", "A-2"};
    const int n = sel.RestoreKeptRows(
        static_cast<int>(order.size()), [&order](int row) -> const std::string& { return order[row]; }, 5);
    CHECK(n == 0);
    CHECK(sel.KeepIssueIds.empty());
    CHECK(sel.PrimaryRow == -1);
}
