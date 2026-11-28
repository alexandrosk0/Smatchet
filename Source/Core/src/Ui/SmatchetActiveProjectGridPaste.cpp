// SmatchetActiveProjectGridPaste.cpp — the grid's bulk commands (input UX Wave 3,
// docs/plans/shipped/input-ux-w3-grid-spreadsheet.md, slice S4): Ctrl+V pastes a value or a TSV block, Ctrl+D fills
// the top row of the cell range down, and Ctrl+Enter in an inline editor applies its value to the selected rows. Each
// one works out which cells it writes and what each cell gets, then hands the columns to RequestBulkFieldEdit (the
// confirm and the pipeline). Where a pasted text lands and what it means are pure: Ui/GridCursorPure.h (PlanPaste)
// and Ui/GridPastePure.h (ResolveCellText).

#include "SmatchetActiveProjectGridIds.h"
#include "SmatchetActiveProjectGridUi_Internal.h"

#include "CachedTicketTypes.h"
#include "ComponentOptionsPickPure.h"
#include "GridPane.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SpreadsheetState.h"
#include "TicketFieldEditor.h"
#include "TicketGridModel.h"
#include "Tracker/ProjectResolver.h"
#include "Tracker/TrackerDateTimePure.h"
#include "Types/ProjectComponentsTypes.h"
#include "Ui/GridCursorPure.h"
#include "Ui/GridPastePure.h"

#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace cursor = smatchet::grid::cursor;
namespace paste = smatchet::grid::paste;
using smatchet::grid::bulk::BulkColumnWrite;
using smatchet::grid::bulk::BulkRowInput;

// The columns one command writes, gathered cell by cell, and the cells it leaves out.
struct BulkTargets {
    std::map<int, BulkColumnWrite> Columns;   ///< by drawn column, so the writes go left to right
    std::map<int, std::string> ComponentsKey; ///< a components column: the project its option list belongs to
    int Skipped = 0;
};

const CachedTicket* TicketAtRow(const ActiveProjectDrawCtx& ctx, int row) {
    const auto& shown = ctx.pane.filteredIndices;
    if (row < 0 || static_cast<std::size_t>(row) >= shown.size() ||
        shown[static_cast<std::size_t>(row)] >= ctx.tickets.size()) {
        return nullptr;
    }
    return &ctx.tickets[shown[static_cast<std::size_t>(row)]];
}

// The catalog field of drawn column `col` when its cells have an editor; null for the key column, a display-only
// column and a field the catalog does not know. Whether this user may write a given row is the pipeline's pre-flight.
const TrackerField* WritableColumnField(const ActiveProjectDrawCtx& ctx, int col) {
    if (col < 0 || static_cast<std::size_t>(col) >= ctx.columns.size()) {
        return nullptr;
    }
    const TicketGridColumn& column = ctx.columns[static_cast<std::size_t>(col)];
    if (column.ColumnKind == TicketGridColumn::Kind::Id || !column.NeedsAllowEditsCheck) {
        return nullptr;
    }
    return ctx.catalogIndex.Find(column.FieldId);
}

// The write of column `col`, made on its first cell. A components column takes the option list of that first row's
// project (never another project's list, Ui AGENTS.md), so rows of other projects are left out of it.
BulkColumnWrite* ColumnFor(ActiveProjectDrawCtx& ctx, BulkTargets& targets, int col, const CachedTicket& ticket) {
    const TrackerField* field = WritableColumnField(ctx, col);
    if (field == nullptr) {
        return nullptr;
    }
    const bool components = field->Id == "components";
    const std::string projectKey = components ? smatchet::ExtractIssueKeyPrefix(ticket.id) : std::string();
    const std::map<int, BulkColumnWrite>::iterator found = targets.Columns.find(col);
    if (found != targets.Columns.end()) {
        return components && targets.ComponentsKey[col] != projectKey ? nullptr : &found->second;
    }
    BulkColumnWrite write;
    write.Field = *field;
    if (components) {
        ProjectComponentsLookup holder;
        const smatchet::components::ComponentOptionsPick pick =
            smatchet::components::PickProjectComponents(ctx.app, projectKey, *field, ctx.catalogProjectKey, holder);
        write.Field.AllowedValueOptions = *pick.Options;
        targets.ComponentsKey[col] = projectKey;
    }
    return &targets.Columns.emplace(col, std::move(write)).first->second;
}

void AddRow(BulkColumnWrite& write, const std::string& issueId, std::vector<std::string> values) {
    BulkRowInput input;
    input.IssueId = issueId;
    input.Values = std::move(values);
    write.Rows.push_back(std::move(input));
}

// One cell that receives `text` (a pasted or filled-down value), read as its field's kind. `exact` is the stored date
// behind the text when this app copied it.
void AddTextCell(ActiveProjectDrawCtx& ctx, BulkTargets& targets, int row, int col, const std::string& text,
                 const std::string& exact, const TrackerParseExplain::CivilDate& today) {
    const CachedTicket* ticket = TicketAtRow(ctx, row);
    BulkColumnWrite* write = ticket != nullptr ? ColumnFor(ctx, targets, col, *ticket) : nullptr;
    if (write == nullptr) {
        ++targets.Skipped;
        return;
    }
    paste::PasteValueInput in;
    in.Text = text;
    in.Exact = exact;
    in.Current = ticket->GetFieldValue(write->Field.Id);
    std::vector<std::string> values;
    if (!paste::ResolveCellText(write->Field, in, today, values)) {
        ++targets.Skipped;
        return;
    }
    AddRow(*write, ticket->id, std::move(values));
}

void Send(ActiveProjectDrawCtx& ctx, BulkTargets& targets) {
    std::vector<BulkColumnWrite> columns;
    columns.reserve(targets.Columns.size());
    for (auto& entry : targets.Columns) {
        if (!entry.second.Rows.empty()) {
            columns.push_back(std::move(entry.second));
        }
    }
    RequestBulkFieldEdit(ctx.app, ctx.d, ctx.pane.id, std::move(columns), targets.Skipped, ctx.readOnlyMode);
}

// Where the cell (issue id, field id) is drawn now; invalid when its row or column is no longer shown.
cursor::CellPos FindCell(const ActiveProjectDrawCtx& ctx, const std::string& issueId, const std::string& fieldId) {
    const GridShownRowIds shownIds{ctx.pane.filteredIndices, ctx.tickets};
    const GridDrawnColumnFieldIds columnIds{ctx.columns};
    const int rowCount = static_cast<int>(ctx.pane.filteredIndices.size());
    const int colCount = static_cast<int>(ctx.columns.size());
    int row = 0;
    while (row < rowCount && shownIds(row) != issueId) {
        ++row;
    }
    int col = 0;
    while (col < colCount && columnIds(col) != fieldId) {
        ++col;
    }
    return row < rowCount && col < colCount ? cursor::CellPos{row, col} : cursor::CellPos{-1, -1};
}

TrackerParseExplain::CivilDate Today() {
    TrackerParseExplain::CivilDate today;
    if (!TrackerDateTimePure::TodayLocalYmd(today.Year, today.Month, today.Day)) {
        today = TrackerParseExplain::CivilDate(); // relative words ("today") then read as nothing and are skipped
    }
    return today;
}

void RunPaste(ActiveProjectDrawCtx& ctx) {
    const char* clipboard = ImGui::GetClipboardText();
    if (clipboard == nullptr || clipboard[0] == '\0') {
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("grid.paste.title", "Paste"),
            SmatchetLocalization::T("grid.paste.empty", "The clipboard holds no text."), ToastType::Info);
        return;
    }
    const std::string text = clipboard;
    const SpreadsheetState& gs = ctx.pane.gridState;
    const GridRectSelection& sel = gs.RectSel;
    bool overRange = sel.Active && sel.RangeMulti;
    cursor::CellPos at{gs.ActiveRow, gs.ActiveCol};
    if (!gs.BulkIssueId.empty()) {
        // The cell menu names the cell it was opened on: a cell outside the range is pasted onto by itself.
        const cursor::CellPos clicked = FindCell(ctx, gs.BulkIssueId, gs.BulkFieldId);
        if (!clicked.Valid()) {
            return;
        }
        if (!overRange || !sel.RectContains(clicked.Row, clicked.Col)) {
            at = clicked;
            overRange = false;
        }
    }
    if (!overRange && !at.Valid()) {
        return; // no cell to paste onto
    }
    cursor::RangeMask mask;
    mask.RowIn = &sel.RowInRange;
    mask.ColIn = &sel.ColInRange;
    const cursor::PastePlan plan =
        cursor::PlanPaste(text, at, overRange ? &mask : nullptr, static_cast<int>(ctx.pane.filteredIndices.size()),
                          static_cast<int>(ctx.columns.size()));
    const paste::CopyRecord& copy = LastGridCopy();
    const bool ownCopy = paste::IsOwnCopy(copy, text);
    const bool oneCellCopy = copy.Exact.size() == 1 && copy.Exact.front().size() == 1;
    const TrackerParseExplain::CivilDate today = Today();
    BulkTargets targets;
    targets.Skipped = plan.Dropped;
    for (const cursor::PasteCell& cell : plan.Cells) {
        std::string exact;
        if (ownCopy) {
            exact =
                oneCellCopy ? paste::ExactAt(copy, 0, 0) : paste::ExactAt(copy, cell.Row - at.Row, cell.Col - at.Col);
        }
        AddTextCell(ctx, targets, cell.Row, cell.Col, cell.Text, exact, today);
    }
    Send(ctx, targets);
}

// The rows of the cell range, in the order shown: its members only (a re-sort can put other rows between them).
std::vector<int> RangeRows(const GridRectSelection& sel, int rowCount) {
    std::vector<int> rows;
    if (!sel.Active) {
        return rows;
    }
    for (int row = (std::max)(sel.MinRow(), 0); row <= sel.MaxRow() && row < rowCount; ++row) {
        const std::size_t at = static_cast<std::size_t>(row);
        if (sel.RowInRange.empty() || (at < sel.RowInRange.size() && sel.RowInRange[at])) {
            rows.push_back(row);
        }
    }
    return rows;
}

void RunFillDown(ActiveProjectDrawCtx& ctx) {
    const GridRectSelection& sel = ctx.pane.gridState.RectSel;
    const std::vector<int> rows = RangeRows(sel, static_cast<int>(ctx.pane.filteredIndices.size()));
    const CachedTicket* source = rows.size() > 1 ? TicketAtRow(ctx, rows.front()) : nullptr;
    if (source == nullptr) {
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("grid.paste.fill_title", "Fill down"),
            SmatchetLocalization::T("grid.paste.fill_need_range", "Select cells in at least two rows first."),
            ToastType::Info);
        return;
    }
    const TrackerParseExplain::CivilDate today = Today();
    BulkTargets targets;
    const int colCount = static_cast<int>(ctx.columns.size());
    for (int col = (std::max)(sel.MinCol(), 0); col <= sel.MaxCol() && col < colCount; ++col) {
        if (!sel.RectContains(rows.front(), col)) {
            continue; // not a column of the range
        }
        const std::string value = source->GetFieldValue(ctx.columns[static_cast<std::size_t>(col)].FieldId);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            AddTextCell(ctx, targets, rows[i], col, value, value, today);
        }
    }
    Send(ctx, targets);
}

// The rows Ctrl+Enter writes: the selected whole rows, else the rows of the cell range. A selection that does not hold
// the edited row is not what the user is working on, so only the edited row is written then.
std::vector<int> ApplyRows(const GridRectSelection& sel, int editedRow, int rowCount) {
    std::vector<int> rows;
    if (sel.Rows.empty()) {
        rows = RangeRows(sel, rowCount);
    } else {
        rows.assign(sel.Rows.begin(), sel.Rows.end());
    }
    if (std::find(rows.begin(), rows.end(), editedRow) == rows.end()) {
        rows.assign(1, editedRow);
    }
    return rows;
}

void RunApplyValue(ActiveProjectDrawCtx& ctx) {
    const SpreadsheetState& gs = ctx.pane.gridState;
    const cursor::CellPos cell = FindCell(ctx, gs.BulkIssueId, gs.BulkFieldId);
    const CachedTicket* edited = TicketAtRow(ctx, cell.Row);
    if (edited == nullptr) {
        return; // the cell left the view between the key and this frame
    }
    const int col = cell.Col;
    const std::vector<int> rows = ApplyRows(gs.RectSel, cell.Row, static_cast<int>(ctx.pane.filteredIndices.size()));
    if (rows.size() == 1 && edited->GetFieldValue(gs.BulkFieldId) == gs.BulkValue) {
        return; // one unchanged cell: Ctrl+Enter just closes the editor, as Enter does
    }
    BulkTargets targets;
    for (const int row : rows) {
        const CachedTicket* ticket = TicketAtRow(ctx, row);
        BulkColumnWrite* write = ticket != nullptr ? ColumnFor(ctx, targets, col, *ticket) : nullptr;
        if (write == nullptr) {
            ++targets.Skipped;
            continue;
        }
        // The editor's own text, written as the editor writes it.
        AddRow(*write, ticket->id, std::vector<std::string>{gs.BulkValue});
    }
    Send(ctx, targets);
}

} // namespace

void RunGridBulkCommand(ActiveProjectDrawCtx& ctx) {
    SpreadsheetState& gs = ctx.pane.gridState;
    switch (gs.BulkCommand) {
    case GridBulkCommand::None:
        return;
    case GridBulkCommand::ApplyValue:
        RunApplyValue(ctx);
        break;
    case GridBulkCommand::Paste:
        RunPaste(ctx);
        break;
    case GridBulkCommand::FillDown:
        RunFillDown(ctx);
        break;
    }
    gs.ClearBulkCommand();
}
