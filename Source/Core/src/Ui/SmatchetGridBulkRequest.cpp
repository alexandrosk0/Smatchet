// The keyboard bulk writes of the grid (Ctrl+Enter, fill-down, a paste) and their confirm dialog. The writes
// themselves go through the field-edit pipeline (SmatchetGridFieldEditPipeline.cpp); the rules are in the Grid
// bullet of Source/Core/src/Ui/AGENTS.md.
#include "SmatchetGridUiSupport.h"

#include "GridPane.h"
#include "SmatchetLocalization.h"
#include "SmatchetLocalizedImGui.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "Ui/BulkFieldEditPure.h"
#include "Ui/SmatchetBulkFieldEditPopup.h"
#include "Ui/SmatchetDialogScope.h"

#include "imgui.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

using smatchet::grid::bulk::BulkColumnWrite;
using smatchet::grid::bulk::BulkFieldEditJob;

const char* const kConfirmId = "##grid_keyboard_bulk_confirm";

struct WritePreview {
    int ToWrite = 0;
    int Skipped = 0;
    int SingleCellSkipped = 0; ///< skips of the one-row columns, which have no job summary toast to report them
};

// The pre-flight of every column against the pane's tickets as they are now. Memory only.
WritePreview PreviewColumns(AppController& app, const std::string& paneId,
                            const std::vector<BulkColumnWrite>& columns) {
    WritePreview preview;
    for (const BulkColumnWrite& column : columns) {
        std::vector<std::vector<std::string>> rowValues;
        const BulkFieldEditJob job = PrepareBulkFieldEditJob(app, paneId, column.Field, column.Rows, &rowValues);
        const smatchet::grid::bulk::BulkSummary summary = smatchet::grid::bulk::Summarize(job);
        preview.ToWrite += summary.Remaining;
        preview.Skipped += summary.Skipped;
        if (column.Rows.size() == 1) {
            preview.SingleCellSkipped += summary.Skipped;
        }
    }
    return preview;
}

void ToastBulk(const char* message, ToastType type) {
    SmatchetToastManager::Instance().Push(SmatchetLocalization::T("grid.bulk.toast_title", "Bulk edit"), message, type);
}

void ToastReadOnly() {
    ToastBulk(SmatchetLocalization::T("grid.bulk.read_only", "Nothing was changed: the grid is read-only."),
              ToastType::Warning);
}

void ApplyColumns(AppController& app, UiDrawSession& d, const std::string& paneId,
                  const std::vector<BulkColumnWrite>& columns, bool readOnlyMode) {
    if (readOnlyMode) {
        ToastReadOnly();
        return;
    }
    for (const BulkColumnWrite& column : columns) {
        if (column.Rows.size() == 1) {
            // One cell is an ordinary edit: the single-cell toast and history entry, not a job of one row.
            QueueSingleCellFieldEdit(app, d, paneId, column.Field, column.Rows.front(), readOnlyMode);
        } else {
            StartBulkFieldEdit(app, d, paneId, column.Field, column.Rows, readOnlyMode);
        }
    }
}

std::string ConfirmText(const PendingBulkConfirm& pending) {
    if (pending.Columns.size() == 1) {
        return SmatchetLocalization::Format("grid.bulk.confirm_body", "Apply to %d rows? %d will be skipped.",
                                            pending.ToWrite, pending.Skipped);
    }
    return SmatchetLocalization::Format("grid.bulk.confirm_cells", "Apply %d cells in %d columns? %d will be skipped.",
                                        pending.ToWrite, static_cast<int>(pending.Columns.size()), pending.Skipped);
}

// The "Apply to N rows?" dialog of the parked write that came from `pane`.
void DrawGridBulkConfirm(AppController& app, UiDrawSession& d, const GridPane& pane, bool readOnlyMode) {
    PendingBulkConfirm& pending = d.pendingBulkConfirm;
    if (!pending.Active || pending.PaneId != pane.id) {
        return;
    }
    const std::string title =
        std::string(SmatchetLocalization::T("grid.bulk.confirm_title", "Confirm bulk edit")) + kConfirmId;
    if (pending.OpenPending) {
        pending.OpenPending = false;
        SmatchetLocalizedImGui::OpenPopup(title.c_str());
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!smatchet::ui::BeginDialog(pending.Scope, title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        pending = PendingBulkConfirm(); // closed with the X
        return;
    }
    ImGui::TextUnformatted(ConfirmText(pending).c_str());
    ImGui::Separator();
    smatchet::ui::ConfirmFooterSpec spec;
    // Label hands out a temporary buffer: each label is kept as its own string while the footer draws both.
    const std::string apply = SmatchetLocalization::Label("grid.bulk.apply", "Apply", "kbd_bulk_apply");
    const std::string cancel = SmatchetLocalization::Label("grid.bulk.cancel", "Cancel", "kbd_bulk_cancel");
    spec.PrimaryLabel = apply.c_str();
    spec.CancelLabel = cancel.c_str();
    const smatchet::dialogkeys::ConfirmPick pick = smatchet::ui::ConfirmFooter(pending.Scope, spec);
    if (pick == smatchet::dialogkeys::ConfirmPick::Primary) {
        ImGui::CloseCurrentPopup();
    }
    smatchet::ui::EndDialog(pending.Scope);
    if (pick == smatchet::dialogkeys::ConfirmPick::Primary) {
        const PendingBulkConfirm accepted = std::move(pending);
        pending = PendingBulkConfirm();
        ApplyColumns(app, d, accepted.PaneId, accepted.Columns, readOnlyMode);
    } else if (pick == smatchet::dialogkeys::ConfirmPick::Cancel) {
        pending = PendingBulkConfirm();
    }
}

} // namespace

void RequestBulkFieldEdit(AppController& app, UiDrawSession& d, const std::string& paneId,
                          std::vector<BulkColumnWrite> columns, int skipped, bool readOnlyMode) {
    if (readOnlyMode) {
        ToastReadOnly();
        return;
    }
    const WritePreview preview = PreviewColumns(app, paneId, columns);
    const int allSkipped = skipped + preview.Skipped;
    if (preview.ToWrite == 0) {
        ToastBulk(
            SmatchetLocalization::Format("grid.bulk.nothing_to_change", "Nothing to change: %d skipped.", allSkipped),
            ToastType::Info);
        return;
    }
    // The threshold counts the cells asked for, as the context menu's confirm counts the selected rows.
    if (smatchet::grid::bulk::NeedsConfirm(preview.ToWrite + preview.Skipped)) {
        PendingBulkConfirm pending;
        pending.Active = true;
        pending.OpenPending = true;
        pending.PaneId = paneId;
        pending.Columns = std::move(columns);
        pending.ToWrite = preview.ToWrite;
        pending.Skipped = allSkipped;
        d.pendingBulkConfirm = std::move(pending);
        return;
    }
    const int unreported = skipped + preview.SingleCellSkipped;
    if (unreported > 0) {
        ToastBulk(SmatchetLocalization::Format("grid.bulk.cells_skipped", "%d cells were skipped.", unreported),
                  ToastType::Warning);
    }
    ApplyColumns(app, d, paneId, columns, readOnlyMode);
}

void DrawGridBulkDialogs(AppController& app, UiDrawSession& d, GridPane& pane, bool readOnlyMode) {
    DrawBulkFieldEditPopup(app, d, pane, readOnlyMode);
    DrawGridBulkConfirm(app, d, pane, readOnlyMode);
}
