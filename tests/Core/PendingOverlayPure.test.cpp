#include <doctest/doctest.h>

#include "Sync/PendingOverlayPure.h"

#include <string>

using namespace smatchet::sync;

namespace {

PendingFieldEditRecord Edit(const std::string& backend, const std::string& issue, const std::string& field,
                            bool conflict = false) {
    PendingFieldEditRecord r;
    r.BackendKey = backend;
    r.IssueKey = issue;
    r.FieldId = field;
    r.HasMergeConflict = conflict;
    return r;
}

DeadPendingFieldEdit DeadEdit(const std::string& backend, const std::string& issue, const std::string& field) {
    DeadPendingFieldEdit r;
    r.BackendKey = backend;
    r.IssueKey = issue;
    r.FieldId = field;
    return r;
}

PendingActionRecord Action(const std::string& backend, const std::string& issue, const std::string& state) {
    PendingActionRecord r;
    r.BackendKey = backend;
    r.IssueKey = issue;
    r.State = state;
    r.Kind = PendingActionKindWire(PendingActionKind::CommentAdd);
    return r;
}

} // namespace

TEST_CASE("BuildPendingOverlay: no snapshots, or empty ones, give an empty overlay") {
    CHECK(BuildPendingOverlay(nullptr, nullptr).Empty());
    const OfflineQueueSnapshot queue;
    const PendingActionsSnapshot actions;
    const PendingOverlay overlay = BuildPendingOverlay(&queue, &actions);
    CHECK(overlay.Empty());
    CHECK(overlay.Row("Jira@a", "A-1") == PendingEditStatus::None);
    CHECK(overlay.Cell("Jira@a", "A-1", "summary") == PendingEditStatus::None);
    CHECK(overlay.RowCount("Jira@a", "A-1") == 0);
}

TEST_CASE("BuildPendingOverlay: a queued field edit marks its cell and its row, and no other cell") {
    OfflineQueueSnapshot queue;
    queue.PendingEdits.push_back(Edit("Jira@a", "A-1", "summary"));
    const PendingOverlay overlay = BuildPendingOverlay(&queue, nullptr);
    CHECK_FALSE(overlay.Empty());
    CHECK(overlay.Row("Jira@a", "A-1") == PendingEditStatus::Queued);
    CHECK(overlay.Cell("Jira@a", "A-1", "summary") == PendingEditStatus::Queued);
    CHECK(overlay.Cell("Jira@a", "A-1", "status") == PendingEditStatus::None); // another field of the same row
    CHECK(overlay.Row("Jira@a", "A-2") == PendingEditStatus::None);
    CHECK(overlay.RowCount("Jira@a", "A-1") == 1);
}

TEST_CASE("BuildPendingOverlay: the same issue key on another tracker namespace is a different row") {
    OfflineQueueSnapshot queue;
    queue.PendingEdits.push_back(Edit("Jira@a", "X-1", "summary"));
    const PendingOverlay overlay = BuildPendingOverlay(&queue, nullptr);
    CHECK(overlay.Row("Jira@a", "X-1") == PendingEditStatus::Queued);
    CHECK(overlay.Row("Jira@b", "X-1") == PendingEditStatus::None);
    CHECK(overlay.Row("Plane@c", "X-1") == PendingEditStatus::None);
}

TEST_CASE("BuildPendingOverlay: a dead-lettered or conflicted edit needs attention and wins over a queued one") {
    OfflineQueueSnapshot queue;
    queue.PendingEdits.push_back(Edit("Jira@a", "A-1", "summary"));
    queue.DeadEdits.push_back(DeadEdit("Jira@a", "A-1", "summary"));
    queue.PendingEdits.push_back(Edit("Jira@a", "A-2", "assignee", /*conflict*/ true));
    queue.PendingEdits.push_back(Edit("Jira@a", "A-3", "assignee"));
    queue.DeadEdits.push_back(DeadEdit("Jira@a", "A-3", "labels"));
    const PendingOverlay overlay = BuildPendingOverlay(&queue, nullptr);
    CHECK(overlay.Cell("Jira@a", "A-1", "summary") == PendingEditStatus::NeedsAttention);
    CHECK(overlay.Row("Jira@a", "A-1") == PendingEditStatus::NeedsAttention);
    CHECK(overlay.RowCount("Jira@a", "A-1") == 2);
    CHECK(overlay.Cell("Jira@a", "A-2", "assignee") == PendingEditStatus::NeedsAttention);
    // One queued and one dead field on a row: each cell keeps its own status, the row shows the worst.
    CHECK(overlay.Cell("Jira@a", "A-3", "assignee") == PendingEditStatus::Queued);
    CHECK(overlay.Cell("Jira@a", "A-3", "labels") == PendingEditStatus::NeedsAttention);
    CHECK(overlay.Row("Jira@a", "A-3") == PendingEditStatus::NeedsAttention);
}

TEST_CASE("BuildPendingOverlay: queued actions mark the row only; stuck and dead ones need attention") {
    PendingActionsSnapshot actions;
    actions.Pending.push_back(Action("Jira@a", "A-1", PendingActionState::kPending));
    actions.Pending.push_back(Action("Jira@a", "A-2", PendingActionState::kSending));
    actions.Pending.push_back(Action("Jira@a", "A-3", PendingActionState::kAmbiguous));
    actions.Pending.push_back(Action("Jira@a", "A-4", PendingActionState::kNeedsReview));
    actions.Pending.push_back(Action("Jira@a", "A-5", "a_state_a_newer_build_wrote"));
    DeadPendingAction dead;
    dead.Row = Action("Jira@a", "A-6", PendingActionState::kPending);
    actions.Dead.push_back(dead);
    const PendingOverlay overlay = BuildPendingOverlay(nullptr, &actions);
    CHECK(overlay.Row("Jira@a", "A-1") == PendingEditStatus::Queued);
    CHECK(overlay.Row("Jira@a", "A-2") == PendingEditStatus::Queued);
    CHECK(overlay.Row("Jira@a", "A-3") == PendingEditStatus::NeedsAttention);
    CHECK(overlay.Row("Jira@a", "A-4") == PendingEditStatus::NeedsAttention);
    CHECK(overlay.Row("Jira@a", "A-5") == PendingEditStatus::Queued); // unknown state: still unconfirmed
    CHECK(overlay.Row("Jira@a", "A-6") == PendingEditStatus::NeedsAttention);
    CHECK(overlay.Cell("Jira@a", "A-1", "summary") == PendingEditStatus::None); // an action has no cell
}

TEST_CASE("BuildPendingOverlay: pending creates and rows without an issue key are not part of it") {
    OfflineQueueSnapshot queue;
    queue.PendingCreates.push_back(PendingCreate());
    queue.PendingEdits.push_back(Edit("Jira@a", "", "summary"));
    CHECK(BuildPendingOverlay(&queue, nullptr).Empty());
}

TEST_CASE("PendingActionStatus: only ambiguous and needs-review rows are stuck on the user") {
    CHECK(PendingActionStatus(PendingActionState::kPending) == PendingEditStatus::Queued);
    CHECK(PendingActionStatus(PendingActionState::kSending) == PendingEditStatus::Queued);
    CHECK(PendingActionStatus(PendingActionState::kAmbiguous) == PendingEditStatus::NeedsAttention);
    CHECK(PendingActionStatus(PendingActionState::kNeedsReview) == PendingEditStatus::NeedsAttention);
}

// The three tests below cover the lookups the grid's render path calls (SmatchetActiveProjectGridTable.cpp row loop
// -> PendingRowCueFor; SmatchetActiveProjectGridCells.cpp key cell -> DrawPendingRowCue, value cell ->
// PendingCellBadge -> PendingCellCueFor / PendingCellGlyph; SmatchetGridUiSupport.cpp RefreshPendingOverlay ->
// RefreshPendingOverlayIfChanged).

TEST_CASE("PendingRowCueFor: a row with a queued edit carries its status and count, on its own tracker only") {
    OfflineQueueSnapshot queue;
    queue.PendingEdits.push_back(Edit("Jira@a", "PRJ-1", "summary"));
    queue.PendingEdits.push_back(Edit("Jira@a", "PRJ-1", "labels"));
    queue.DeadEdits.push_back(DeadEdit("Jira@a", "PRJ-2", "summary"));
    const PendingOverlay overlay = BuildPendingOverlay(&queue, nullptr);

    const PendingRowCue queued = PendingRowCueFor(overlay, "Jira@a", "PRJ-1");
    CHECK(queued.Status == PendingEditStatus::Queued);
    CHECK(queued.Count == 2);
    const PendingRowCue dead = PendingRowCueFor(overlay, "Jira@a", "PRJ-2");
    CHECK(dead.Status == PendingEditStatus::NeedsAttention);
    CHECK(dead.Count == 1);
    CHECK(PendingRowCueFor(overlay, "Jira@a", "PRJ-3").Status == PendingEditStatus::None);
    CHECK(PendingRowCueFor(overlay, "Jira@b", "PRJ-1").Status == PendingEditStatus::None); // another tracker's pane
    CHECK(PendingRowCueFor(PendingOverlay(), "Jira@a", "PRJ-1").Status == PendingEditStatus::None);
}

TEST_CASE("PendingCellCueFor: only the field that has the edit gets the badge, and a row without a cue none") {
    OfflineQueueSnapshot queue;
    queue.PendingEdits.push_back(Edit("Jira@a", "PRJ-1", "summary"));
    queue.DeadEdits.push_back(DeadEdit("Jira@a", "PRJ-1", "labels"));
    PendingActionsSnapshot actions;
    actions.Pending.push_back(Action("Jira@a", "PRJ-5", PendingActionState::kPending));
    const PendingOverlay overlay = BuildPendingOverlay(&queue, &actions);

    const PendingRowCue row = PendingRowCueFor(overlay, "Jira@a", "PRJ-1");
    CHECK(PendingCellCueFor(overlay, row, "Jira@a", "PRJ-1", "summary") == PendingEditStatus::Queued);
    CHECK(PendingCellCueFor(overlay, row, "Jira@a", "PRJ-1", "labels") == PendingEditStatus::NeedsAttention);
    CHECK(PendingCellCueFor(overlay, row, "Jira@a", "PRJ-1", "status") == PendingEditStatus::None);
    // A queued action marks the row but no cell.
    const PendingRowCue actionRow = PendingRowCueFor(overlay, "Jira@a", "PRJ-5");
    CHECK(actionRow.Status == PendingEditStatus::Queued);
    CHECK(PendingCellCueFor(overlay, actionRow, "Jira@a", "PRJ-5", "summary") == PendingEditStatus::None);
    // The render path hands a row without a cue straight through: the cell is never looked up.
    CHECK(PendingCellCueFor(overlay, PendingRowCue(), "Jira@a", "PRJ-1", "summary") == PendingEditStatus::None);
}

TEST_CASE("PendingCellGlyph: ~ while queued, ! when it needs attention, nothing otherwise") {
    CHECK(std::string(PendingCellGlyph(PendingEditStatus::Queued)) == "~");
    CHECK(std::string(PendingCellGlyph(PendingEditStatus::NeedsAttention)) == "!");
    CHECK(std::string(PendingCellGlyph(PendingEditStatus::None)).empty());
}

TEST_CASE("RefreshPendingOverlayIfChanged: rebuilds only when a snapshot object was replaced") {
    PendingOverlay overlay;
    std::shared_ptr<const OfflineQueueSnapshot> heldQueue;
    std::shared_ptr<const PendingActionsSnapshot> heldActions;
    auto queue = std::make_shared<OfflineQueueSnapshot>();
    queue->PendingEdits.push_back(Edit("Jira@a", "PRJ-1", "summary"));
    const std::shared_ptr<const OfflineQueueSnapshot> queue1 = queue;
    const std::shared_ptr<const PendingActionsSnapshot> actions1 = std::make_shared<PendingActionsSnapshot>();

    CHECK(RefreshPendingOverlayIfChanged(overlay, heldQueue, heldActions, queue1, actions1)); // first call builds
    CHECK(overlay.Row("Jira@a", "PRJ-1") == PendingEditStatus::Queued);
    for (int frame = 0; frame < 5; ++frame) { // the same snapshots on later frames: no rebuild
        CHECK_FALSE(RefreshPendingOverlayIfChanged(overlay, heldQueue, heldActions, queue1, actions1));
    }

    // The queue worker publishes a new snapshot (the edit was sent): the next call rebuilds and the cue is gone.
    const std::shared_ptr<const OfflineQueueSnapshot> queue2 = std::make_shared<OfflineQueueSnapshot>();
    CHECK(RefreshPendingOverlayIfChanged(overlay, heldQueue, heldActions, queue2, actions1));
    CHECK(overlay.Empty());

    // The action queue's snapshot is replaced on its own: also a rebuild.
    auto actions = std::make_shared<PendingActionsSnapshot>();
    actions->Pending.push_back(Action("Jira@a", "PRJ-7", PendingActionState::kPending));
    CHECK(RefreshPendingOverlayIfChanged(overlay, heldQueue, heldActions, queue2, actions));
    CHECK(overlay.Row("Jira@a", "PRJ-7") == PendingEditStatus::Queued);
}
