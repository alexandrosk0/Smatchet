#pragma once

// BulkFieldEditPure — the pure core of "set one field on N selected grid rows" (docs/plans/bulk-edit-rows.md).
//
// A BulkFieldEditJob snapshots everything the job needs when it starts: the selected issue ids (never sort-order
// row indices, which stop meaning anything once the first landed edit bumps the snapshot revision), the field, the
// values, the mode and each row's before-value, plus one state per row. The grid fans the job out into ordinary
// PendingFieldEdit rows (SmatchetGridFieldEditPipeline.cpp), so every row still goes through the one commit path
// (offline queue, audit, ADR-0016 conflict replay). This header only holds the data model and the decisions that
// need no UI, network or SQLite: the Add / Remove merge, the summary counts, the mixed-value label, the confirm
// threshold, and the shared ADR-0016 scalar-base capture. No ImGui, cpr or SQLite includes.

#include "Tracker/TrackerFieldSchema.h"
#include "Ui/ToastHistoryPure.h" // ToastType

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace grid {
namespace bulk {

/// Rows above this count ask for a confirm step before the job fans out.
constexpr int kConfirmThreshold = 10;

enum class BulkRowState { Pending, Saving, Saved, Queued, Failed, Skipped };

enum class BulkMode { Replace, Add, Remove };

/// How a field takes part in a bulk edit.
enum class BulkFieldKind {
    Unsupported,  ///< text / date / number / cascading / special columns (P2 or never)
    SingleChoice, ///< one option id: select, user, priority, status
    MultiChoice,  ///< a set of option ids: multi-select, components, versions
    Labels        ///< a set of free-form label strings
};

struct BulkRow {
    std::string IssueId;
    std::string BeforeValue;               ///< the cached display value, also the ADR-0016 conflict base
    std::vector<std::string> BeforeValues; ///< before-value as option ids (labels: the label strings)
    BulkRowState State = BulkRowState::Pending;
    std::string Message; ///< failure message or skip reason
};

struct BulkFieldEditJob {
    std::uint64_t Id = 0;
    std::string PaneId; ///< the pane the user acted in; its target is latched onto every row
    TrackerField Field;
    std::vector<std::string> Values;
    BulkMode Mode = BulkMode::Replace;
    std::vector<BulkRow> Rows;
    bool IsRevert = false;
    bool Reported = false; ///< the summary toast of this job was shown (it is shown once)
};

struct BulkSummary {
    int Total = 0;
    int Saved = 0;
    int Queued = 0;
    int Failed = 0;
    int Skipped = 0;
    int Remaining = 0; ///< rows still Pending or Saving
    bool Done = false; ///< no row is Pending or Saving
};

/// Classifies `field` for a bulk edit. Mirrors the grid's editor dispatch (TicketGridModel.cpp,
/// ResolveEditableFieldPlan) for the choice editors, so a field the grid edits with a dropdown is bulk-editable.
BulkFieldKind ClassifyBulkField(const TrackerField& field);

/// True for the kinds that hold several values (and so offer the Replace / Add / Remove choice).
bool IsMultiValueKind(BulkFieldKind kind);

BulkSummary Summarize(const BulkFieldEditJob& job);

/// "3 saved, 1 queued, 2 failed, 1 skipped" (zero parts omitted, localized) and the toast severity for the same
/// counts; shared by the grid's summary toast and the ticket.set_field_bulk worker's toast.
std::string BulkSummaryText(const BulkSummary& s);
ToastType BulkSummaryToastType(const BulkSummary& s);

/// Sets the final state of `issueId`'s row. False when the job has no such row. A row that is already final
/// (Saved / Queued / Failed / Skipped) is left as it is, so a late duplicate report cannot flip it.
bool SetRowState(BulkFieldEditJob& job, const std::string& issueId, BulkRowState state, const std::string& message);

/// The values to write for one row. Replace: `picked`. Add: `before` plus `picked`. Remove: `before` minus
/// `picked`. The result is deduplicated, drops empty entries and is sorted (the order the multi-select editors
/// write). `caseInsensitive` compares entries ignoring ASCII case (labels).
std::vector<std::string> ComputeValuesForRow(BulkMode mode, const std::vector<std::string>& before,
                                             const std::vector<std::string>& picked, bool caseInsensitive = false);

/// Number of distinct before-values among the rows that are not Skipped. 0 when there are none.
int DistinctBeforeValueCount(const BulkFieldEditJob& job);

/// "mixed (k values)" when the rows differ, the single value when they all agree ("" when it is empty).
/// English only: the UI localizes the mixed case from DistinctBeforeValueCount.
std::string MixedValueLabel(const BulkFieldEditJob& job);

/// True when `rowCount` is above `threshold` (default kConfirmThreshold): the confirm step runs.
bool NeedsConfirm(int rowCount, int threshold = kConfirmThreshold);

/// A row's current value in the form the editors write: option ids for a choice field (resolved from the cached
/// display string), the trimmed label strings for labels. Empty for an empty value; an entry it cannot resolve
/// passes through as raw text (see AllEntriesResolve).
std::vector<std::string> BeforeValuesFor(const TrackerField& field, BulkFieldKind kind,
                                         const std::string& displayValue);

/// The values to write for one row, in the form the single-cell editors queue: `picked` for a single choice, the
/// Replace / Add / Remove merge (ComputeValuesForRow) for a multi value.
std::vector<std::string> NewValuesForRow(BulkFieldKind kind, BulkMode mode, const std::vector<std::string>& before,
                                         const std::vector<std::string>& picked);

/// True when every entry names an option of `field` (by id or name). BeforeValuesFor passes an entry it cannot
/// resolve through as raw text, so a multi-value Add / Remove on a row whose current value holds one (an archived
/// version, a component of another project) would write display names where the tracker wants ids. The caller
/// skips such a row instead.
bool AllEntriesResolve(const TrackerField& field, const std::vector<std::string>& entries);

/// True when writing `after` leaves the row as it is: same set (order and duplicates ignored; labels are case-sensitive
/// on the tracker), same single option id. `before` and `after` are the forms BeforeValuesFor / NewValuesForRow return.
bool IsRowUnchanged(BulkFieldKind kind, const std::vector<std::string>& before, const std::vector<std::string>& after);

/// ADR-0016 scalar conflict base: the pre-edit DISPLAY value of the cell, recorded on the edit so an offline
/// replay can detect a concurrent server change. Shared by the grid's single-cell QueueEdit choke point and the
/// bulk job so both capture it the same way. `EditT` is PendingFieldEdit (a template only to keep this header
/// free of the UI session type).
template <class EditT> void CaptureScalarBase(const std::string& displayValue, EditT& edit) {
    edit.OriginalValue = displayValue;
    edit.HasOriginalValue = true; // a scalar base WAS captured, even if it is blank (ADR-0016)
}

} // namespace bulk
} // namespace grid
} // namespace smatchet
