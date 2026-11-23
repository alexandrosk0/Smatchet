// Bucket-A doctest for the pure core of the bulk field edit (Ui/BulkFieldEditPure.h): the Add / Remove
// merge, the summary counts, the mixed-value label, the confirm threshold, the field classification and the
// shared ADR-0016 scalar-base capture. No ImGui, no network, no SQLite.

#include "SpreadsheetState.h"
#include "Ui/BulkFieldEditPure.h"

#include <doctest/doctest.h>

#include <set>
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

// ---- Selection by identity (input UX Wave 3, grid S1): rows, the cell range and the active cell follow their ids ----

namespace {
using IdList = std::vector<std::string>;
const std::string& IdIn(const IdList& list, int index) { return list[static_cast<size_t>(index)]; }
} // namespace

TEST_CASE("GridRectSelection::SyncRows mirrors a changed Rows by id, and only a changed one") {
    GridRectSelection sel;
    const IdList order = {"A-1", "A-2", "A-3", "A-4"};
    auto idAt = [&order](int row) -> const std::string& { return IdIn(order, row); };
    sel.Rows = {1, 3};
    sel.SyncRows(4, idAt);
    CHECK(sel.KeepIssueIds == std::set<std::string>{"A-2", "A-4"});
    CHECK(sel.KeepRows == sel.Rows);
    CHECK_FALSE(sel.KeepStale());
    // The user picks something else: the mirror follows instead of dropping.
    sel.Rows = {0};
    sel.SyncRows(4, idAt);
    CHECK(sel.KeepIssueIds == std::set<std::string>{"A-1"});
    // A cleared selection clears the mirror.
    sel.Rows.clear();
    sel.SyncRows(4, idAt);
    CHECK(sel.KeepIssueIds.empty());
}

TEST_CASE("GridRectSelection::Remap re-expresses rows in a new order and drops the rows that left") {
    GridRectSelection sel;
    IdList order = {"A-1", "A-2", "A-3", "A-4"};
    auto idAt = [&order](int row) -> const std::string& { return IdIn(order, row); };
    IdList cols = {"id", "summary"};
    auto fieldAt = [&cols](int col) -> const std::string& { return IdIn(cols, col); };
    sel.Rows = {0, 2};
    sel.SyncRows(4, idAt);
    order = {"A-3", "A-9", "A-1"}; // re-sorted, A-2 and A-4 left, A-9 arrived
    sel.Remap(3, idAt, 2, fieldAt, 42);
    CHECK(sel.Rows == std::set<int>{0, 2});
    CHECK(sel.SortSignature == 42);
}

TEST_CASE("GridRectSelection::SetRange snapshots the identities of a range") {
    GridRectSelection sel;
    const IdList order = {"A-1", "A-2", "A-3", "A-4"};
    auto idAt = [&order](int row) -> const std::string& { return IdIn(order, row); };
    const IdList cols = {"id", "summary", "priority"};
    auto fieldAt = [&cols](int col) -> const std::string& { return IdIn(cols, col); };
    sel.SetRange(2, 2, 1, 1, 4, idAt, 3, fieldAt);
    CHECK(sel.Active);
    CHECK(sel.RangeMulti);
    CHECK(sel.RangeIssueIds == std::set<std::string>{"A-2", "A-3"});
    CHECK(sel.RangeFieldIds == std::set<std::string>{"summary", "priority"});
    CHECK(sel.AnchorIssueId == "A-3");
    CHECK(sel.ExtentIssueId == "A-2");
    CHECK(sel.RectContains(1, 1));
    CHECK(sel.RectContains(2, 2));
    CHECK_FALSE(sel.RectContains(0, 1));
    CHECK_FALSE(sel.RectContains(1, 0));
    // One cell is a range but not a multi-cell one (it shows the focus ring, not a fill).
    sel.SetRange(1, 1, 1, 1, 4, idAt, 3, fieldAt);
    CHECK(sel.Active);
    CHECK_FALSE(sel.RangeMulti);
    // Selecting B after A leaves no byte of A set.
    sel.SetRange(3, 2, 3, 2, 4, idAt, 3, fieldAt);
    CHECK_FALSE(sel.RectContains(1, 1));
    CHECK(sel.RectContains(3, 2));
    // A corner outside the grid clears instead of writing out of bounds.
    sel.SetRange(0, 0, 9, 0, 4, idAt, 3, fieldAt);
    CHECK_FALSE(sel.Active);
}

TEST_CASE("GridRectSelection::Remap keeps a range that a re-sort split: a row between the members never joins") {
    GridRectSelection sel;
    IdList order = {"A", "B", "C", "D"};
    auto idAt = [&order](int row) -> const std::string& { return IdIn(order, row); };
    IdList cols = {"id", "summary"};
    auto fieldAt = [&cols](int col) -> const std::string& { return IdIn(cols, col); };
    sel.SetRange(0, 1, 1, 1, 4, idAt, 2, fieldAt); // rows A, B of column "summary"
    sel.SortSignature = 1;
    order = {"A", "C", "B", "D"}; // C sorted between A and B: corners 0 and 2 would include it
    sel.Remap(4, idAt, 2, fieldAt, 2);
    CHECK(sel.Active);
    CHECK(sel.RectContains(0, 1));       // A
    CHECK_FALSE(sel.RectContains(1, 1)); // C is not a member
    CHECK(sel.RectContains(2, 1));       // B
    CHECK_FALSE(sel.RectContains(3, 1));
    CHECK(sel.MinRow() == 0);
    CHECK(sel.MaxRow() == 2);
    CHECK(sel.RangeMulti);
}

TEST_CASE("GridRectSelection::Remap drops the rows of a range that left the view, and clears when none is left") {
    GridRectSelection sel;
    IdList order = {"A", "B", "C"};
    auto idAt = [&order](int row) -> const std::string& { return IdIn(order, row); };
    IdList cols = {"id", "summary"};
    auto fieldAt = [&cols](int col) -> const std::string& { return IdIn(cols, col); };
    sel.SetRange(0, 1, 2, 1, 3, idAt, 2, fieldAt);
    order = {"C", "X"}; // A and B left the view
    sel.Remap(2, idAt, 2, fieldAt, 7);
    CHECK(sel.Active);
    CHECK(sel.RectContains(0, 1));
    CHECK_FALSE(sel.RectContains(1, 1));
    CHECK_FALSE(sel.RangeMulti);
    order = {"X", "Y"}; // the last member left too
    sel.Remap(2, idAt, 2, fieldAt, 8);
    CHECK_FALSE(sel.Active);
    CHECK(sel.RangeIssueIds.empty());
}

TEST_CASE("GridRectSelection::Remap puts the anchor and extent where their ids now sit") {
    GridRectSelection sel;
    IdList order = {"A", "B", "C"};
    auto idAt = [&order](int row) -> const std::string& { return IdIn(order, row); };
    IdList cols = {"id", "summary"};
    auto fieldAt = [&cols](int col) -> const std::string& { return IdIn(cols, col); };
    sel.SetRange(0, 1, 1, 1, 3, idAt, 2, fieldAt); // anchor A, extent B
    order = {"B", "C", "A"};
    sel.Remap(3, idAt, 2, fieldAt, 3);
    CHECK(sel.AnchorRow == 2); // A
    CHECK(sel.ExtentRow == 0); // B
}

TEST_CASE("SpreadsheetState::ResolveActive finds the active cell by identity once per change") {
    SpreadsheetState state;
    IdList order = {"A-1", "A-2", "A-3"};
    auto idAt = [&order](int row) -> const std::string& { return IdIn(order, row); };
    IdList cols = {"id", "summary", "priority"};
    auto fieldAt = [&cols](int col) -> const std::string& { return IdIn(cols, col); };
    state.ActiveIssueId = "A-3";
    state.ActiveFieldId = "priority";
    state.ResolveActive(3, idAt, 3, fieldAt, 10);
    CHECK(state.ActiveRow == 2);
    CHECK(state.ActiveCol == 2);
    // A re-sort moves the row; the cell follows by id.
    order = {"A-3", "A-1", "A-2"};
    state.ResolveActive(3, idAt, 3, fieldAt, 11);
    CHECK(state.ActiveRow == 0);
    CHECK(state.ActiveCol == 2);
    // A search that narrows the rows moves them without a new signature: the revision is what triggers the search.
    order = {"A-2", "A-3", "A-1"};
    state.ResolveActive(3, idAt, 3, fieldAt, 11);
    CHECK(state.ActiveRow == 0); // same sig, same revision: the stale index stands
    state.ShownRowsRevision = 1;
    state.ResolveActive(3, idAt, 3, fieldAt, 11);
    CHECK(state.ActiveRow == 1);
    order = {"A-3", "A-1", "A-2"};
    state.ShownRowsRevision = 2;
    state.ResolveActive(3, idAt, 3, fieldAt, 11);
    CHECK(state.ActiveRow == 0);
    // The row left the view: no active cell is drawn, the identity is kept.
    order = {"A-1", "A-2"};
    state.ResolveActive(2, idAt, 3, fieldAt, 12);
    CHECK(state.ActiveRow == -1);
    CHECK(state.ActiveIssueId == "A-3");
    // It comes back.
    order = {"A-1", "A-3"};
    state.ResolveActive(2, idAt, 3, fieldAt, 13);
    CHECK(state.ActiveRow == 1);
    // No field yet: the first column.
    state.ActiveFieldId.clear();
    state.ResolveActive(2, idAt, 3, fieldAt, 13);
    CHECK(state.ActiveCol == 0);
}

TEST_CASE("SpreadsheetState::SetActiveCell marks the indices resolved, ClearActiveCell forgets the cell") {
    SpreadsheetState state;
    state.SetActiveCell("A-2", "summary", 1, 1, 5);
    IdList order = {"A-1", "A-2"};
    int lookups = 0;
    auto idAt = [&order, &lookups](int row) -> const std::string& {
        ++lookups;
        return IdIn(order, row);
    };
    IdList cols = {"id", "summary"};
    auto fieldAt = [&cols](int col) -> const std::string& { return IdIn(cols, col); };
    state.ResolveActive(2, idAt, 2, fieldAt, 5);
    CHECK(lookups == 0); // nothing changed: no search
    CHECK(state.ActiveRow == 1);
    state.ClearActiveCell();
    CHECK(state.ActiveIssueId.empty());
    CHECK(state.ActiveRow == -1);
    CHECK(state.ActiveCol == -1);
}

TEST_CASE("GridRectSelection::Remap keeps the Shift anchor row on its issue") {
    GridRectSelection sel;
    IdList order = {"A", "B", "C", "D"};
    auto idAt = [&order](int row) -> const std::string& { return IdIn(order, row); };
    IdList cols = {"id", "summary"};
    auto fieldAt = [&cols](int col) -> const std::string& { return IdIn(cols, col); };
    sel.Rows = {1, 2}; // B, C: the user anchored on C
    sel.PrimaryRow = 2;
    sel.SyncRows(4, idAt);
    order = {"B", "A", "C", "D"};
    sel.Remap(4, idAt, 2, fieldAt, 9);
    CHECK(sel.Rows == std::set<int>{0, 2});
    CHECK(sel.PrimaryRow == 2); // C, not the lowest selected row
    sel.ClearAll();
    CHECK(sel.PrimaryIssueId.empty());
}
