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
