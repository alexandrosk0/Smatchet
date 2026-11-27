// grid_spreadsheet_keys.test.cpp — bucket-E, the Active Project grid's keyboard cell cursor end to end (input UX Wave
// 3, docs/plans/active/input-ux-w3-grid-spreadsheet.md, slice S1; audit findings #29 navigation half, #30, #32).
//
// What this pins, driven with real key presses and real clicks on the real grid (deterministic Jira fixture backend):
//   1. A click on a row's ID cell makes it the active cell; an arrow key then moves exactly ONE cell (and ImGui's
//   generic
//      nav cursor does not move with it: the grid owns those keys); Home / End / Ctrl+Home stay on the row / jump to
//      the first cell; Up at the top row stays; Shift+arrow grows a range while the active cell stays put.
//   2. The active cell scrolls into view clear of the frozen header row: walking up with the arrow key never leaves the
//      active row under the header.
//   3. Dragging from one value cell to another selects a range and opens no editor. A same-cell click opens the editor
//      with single-click-to-edit on (the default) and only selects with it off, where a double-click opens it.
//   4. The active cell and the range are kept by id: a sync (a new snapshot revision) leaves them on their issues, and
//   a
//      search that narrows the grid moves the active cell with its row and back when the box is cleared.
//   5. (S2, audit #26 / #29 edit half) F2, Enter and a typed character open the active cell's editor (caret at the end,
//      value selected, value replaced); a dropdown cell opens its popup. Enter / Shift+Enter / Tab / Shift+Tab commit
//      and move the active cell down / up / right / left over editable cells, opening no editor; Esc reverts and stays;
//      an unchanged Enter writes nothing and still moves.
//   6. (S3, audit #28 undo / #31 / #33 / #34) Ctrl+Z puts a saved cell back: through the "saved" toast's Undo while it
//      is up, and through the grid's own history once it is gone. A bulk set undoes row by row (each row gets ITS
//      OWN old value back, not one shared value). A cell whose save was rejected reopens on the rejected value with the
//      message under it, and Esc drops it.
//
// Needs the grid-spreadsheet fixture (60 issues) named by SMATCHET_TEST_JIRA_BACKEND_FIXTURE;
// scripts/dev/test-ui-grid-spreadsheet.sh sets it and the GridSpreadsheet filter. Skips cleanly without it.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "CachedTicketTypes.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "GridPane.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SpreadsheetState.h"
#include "Ui/BulkFieldEditPure.h"
#include "UiTestWriteScope.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

namespace {

const char* const kGridWindow = "Smatchet - Active Project";
constexpr int kFixtureRows = 60;

// A check whose failure must stop the test: later lines read what it guards.
#define SPREADSHEET_REQUIRE(cond)                                                                                      \
    do {                                                                                                               \
        const bool requireOk_ = static_cast<bool>(cond);                                                               \
        IM_CHECK_NO_RET(requireOk_);                                                                                   \
        if (!requireOk_) {                                                                                             \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

GridPane& Pane() { return g_ui.gridPanes.front(); }
SpreadsheetState& State() { return Pane().gridState; }

bool FixtureActive(ImGuiTestContext* ctx) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform
#endif
    const char* fixture = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (fixture != nullptr && std::strstr(fixture, "grid-spreadsheet") != nullptr) {
        return true;
    }
    ctx->LogInfo("SKIP: grid-spreadsheet fixture not active");
    return false;
}

template <typename Done> bool StepUntil(ImGuiTestContext* ctx, int budget, Done done) {
    while (!done() && budget-- > 0) {
        ctx->Yield();
    }
    return done();
}

const ImGuiTable* GridTable() {
    ImGuiContext& g = *GImGui;
    for (int n = 0; n < g.Tables.GetMapSize(); ++n) {
        const ImGuiTable* table = g.Tables.TryGetMapData(n);
        if (table != nullptr && table->InnerWindow != nullptr && table->LastFrameActive >= g.FrameCount - 1 &&
            std::strstr(table->InnerWindow->Name, "/TicketGrid_") != nullptr &&
            std::strncmp(table->InnerWindow->Name, kGridWindow, std::strlen(kGridWindow)) == 0) {
            return table;
        }
    }
    return nullptr;
}

std::string IdAtRow(int row) {
    const GridPane& pane = Pane();
    if (!pane.ticketsSnapshot || row < 0 || static_cast<size_t>(row) >= pane.filteredIndices.size()) {
        return std::string();
    }
    const size_t idx = pane.filteredIndices[static_cast<size_t>(row)];
    return idx < pane.ticketsSnapshot->size() ? (*pane.ticketsSnapshot)[idx].id : std::string();
}

// The shown row of issue `id`, -1 when the grid does not show it.
int RowOf(const std::string& id) {
    for (int row = 0; row < static_cast<int>(Pane().filteredIndices.size()); ++row) {
        if (IdAtRow(row) == id) {
            return row;
        }
    }
    return -1;
}

int ColumnCount() { return static_cast<int>(Pane().lastDrawnColumnKeys.size()); }

// The on-screen rectangle of the cell (`row`, `col`): x from the table's column, y from the row's ID-cell item (named
// by the issue id). False when the row is not drawn.
bool CellRect(ImGuiTestContext* ctx, int row, int col, ImRect* out) {
    const ImGuiTable* table = GridTable();
    if (table == nullptr || col < 0 || col >= table->ColumnsCount) {
        return false;
    }
    const std::string id = IdAtRow(row);
    ImGuiTestItemList items;
    ctx->GatherItems(&items, (std::string("//") + kGridWindow).c_str(), 99);
    for (const ImGuiTestItemInfo& item : items) {
        if (id == item.DebugLabel) {
            const ImGuiTableColumn& column = table->Columns[col];
            *out = ImRect(column.MinX, item.RectFull.Min.y, column.MaxX, item.RectFull.Max.y);
            return true;
        }
    }
    return false;
}

bool ClickCell(ImGuiTestContext* ctx, int row, int col) {
    ImRect rect;
    if (!CellRect(ctx, row, col, &rect)) {
        return false;
    }
    ctx->MouseMoveToPos(rect.GetCenter());
    ctx->Yield();
    ctx->MouseClick(0);
    ctx->Yield(2);
    return true;
}

// Fixture gate, sync, and a drawn grid with every fixture row; clears the selection and the active cell.
bool PrepareGrid(ImGuiTestContext* ctx, AppController** app) {
    if (!FixtureActive(ctx)) {
        return false;
    }
    *app = SmatchetActiveUiTestAppController();
    IM_CHECK_NO_RET(*app != nullptr);
    if (*app == nullptr) {
        return false;
    }
    (*app)->SyncWithBackend();
    ctx->SetRef(kGridWindow);
    const bool live = StepUntil(ctx, 900, [&] {
        g_ui.requestActiveProjectFocus = true;
        return GridTable() != nullptr && !g_ui.gridPanes.empty() && ColumnCount() > 2 && Pane().ticketsSnapshot &&
               static_cast<int>(Pane().filteredIndices.size()) == kFixtureRows;
    });
    IM_CHECK_NO_RET(live);
    if (!live) {
        return false;
    }
    State().RectSel.Clear();
    State().ClearActiveCell();
    // An earlier test may have left an editor or a dropdown open.
    State().ClearEditing();
    State().ClearKeyEdit();
    State().EditArmedKey.clear();
    State().EditBusyLastFrame = false;
    ImGui::ClosePopupsOverWindow(nullptr, false);
    // An earlier test may have left the grid scrolled: start every test from the top-left.
    const ImGuiTable* table = GridTable();
    if (table != nullptr) {
        ImGui::SetScrollY(table->InnerWindow, 0.0f);
        ImGui::SetScrollX(table->InnerWindow, 0.0f);
    }
    ctx->Yield(10); // layout settles before any coordinate is read
    return true;
}

// True when the active cell sits at (`row`, `col`) and names that row's issue.
bool ActiveAt(int row, int col) {
    const SpreadsheetState& gs = State();
    return gs.ActiveRow == row && gs.ActiveCol == col && gs.ActiveIssueId == IdAtRow(row);
}

// The press moved the active cell exactly where `ActiveAt` says, in one frame step, and ImGui's own nav cursor stayed.
bool PressMoves(ImGuiTestContext* ctx, ImGuiKeyChord chord, int row, int col) {
    const ImGuiID navBefore = GImGui->NavId;
    ctx->KeyPress(chord);
    ctx->Yield(2);
    return ActiveAt(row, col) && GImGui->NavId == navBefore;
}

// The cached summary of issue `id` (what a landed commit leaves), "" when the grid has no such issue.
std::string SummaryOf(AppController& app, const std::string& id) {
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.GetActiveTicketsSnapshot();
    if (!tickets) {
        return std::string();
    }
    for (const CachedTicket& ticket : *tickets) {
        if (ticket.id == id) {
            return ticket.GetFieldValue("summary");
        }
    }
    return std::string();
}

// The drawn column of field `fieldId`, -1 when the grid does not draw it.
int ColumnOf(const char* fieldId) {
    const std::vector<std::string>& keys = Pane().lastDrawnColumnKeys;
    const std::vector<std::string>::const_iterator it =
        std::find(keys.begin(), keys.end(), std::string("field:") + fieldId);
    return it == keys.end() ? -1 : static_cast<int>(it - keys.begin());
}

// The active cell is the summary cell of `row` (column 1 in the fixture's view) and its editor is open.
bool EditingSummaryAt(int row) {
    const SpreadsheetState& gs = State();
    return gs.EditingId == IdAtRow(row) && gs.EditingFieldId == "summary";
}

// Opens the editor of the summary cell at `row` from the keyboard: a click on the row's ID cell, then Right.
bool ActivateSummaryCell(ImGuiTestContext* ctx, int row) {
    return ClickCell(ctx, row, 0) && PressMoves(ctx, ImGuiKey_RightArrow, row, 1);
}

} // namespace

// GridSpreadsheet/Keys_ArrowsMoveOneCellAndShiftExtends
static void RegisterArrowKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Keys_ArrowsMoveOneCellAndShiftExtends");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        const int lastCol = ColumnCount() - 1;

        SPREADSHEET_REQUIRE(ClickCell(ctx, 2, 0)); // the ID cell of the third row
        IM_CHECK_NO_RET(ActiveAt(2, 0));

        IM_CHECK_NO_RET(PressMoves(ctx, ImGuiKey_DownArrow, 3, 0));
        IM_CHECK_NO_RET(PressMoves(ctx, ImGuiKey_RightArrow, 3, 1));
        IM_CHECK_NO_RET(State().RectSel.Active);
        IM_CHECK_NO_RET(!State().RectSel.RangeMulti); // one cell: the focus ring, no fill
        IM_CHECK_NO_RET(PressMoves(ctx, ImGuiKey_End, 3, lastCol));
        IM_CHECK_NO_RET(PressMoves(ctx, ImGuiKey_RightArrow, 3, lastCol)); // the edge holds
        IM_CHECK_NO_RET(PressMoves(ctx, ImGuiKey_Home, 3, 0));
        IM_CHECK_NO_RET(PressMoves(ctx, ImGuiKey_Home | ImGuiMod_Ctrl, 0, 0));
        IM_CHECK_NO_RET(PressMoves(ctx, ImGuiKey_UpArrow, 0, 0)); // the top holds

        // Shift+arrow grows a range from the anchor; the active cell stays where it is.
        IM_CHECK_NO_RET(PressMoves(ctx, ImGuiKey_RightArrow, 0, 1));
        ctx->KeyPress(ImGuiKey_DownArrow | ImGuiMod_Shift);
        ctx->KeyPress(ImGuiKey_DownArrow | ImGuiMod_Shift);
        ctx->Yield(2);
        const GridRectSelection& sel = State().RectSel;
        IM_CHECK_NO_RET(ActiveAt(0, 1));
        IM_CHECK_NO_RET(sel.RangeMulti);
        IM_CHECK_NO_RET(sel.RectContains(0, 1) && sel.RectContains(1, 1) && sel.RectContains(2, 1));
        IM_CHECK_NO_RET(!sel.RectContains(3, 1) && !sel.RectContains(1, 0) && !sel.RectContains(1, 2));
        IM_CHECK_NO_RET(sel.RectContains(1, 1) && sel.Rows.empty());

        // Ctrl+Shift+End grows the range to the last cell too (it does not collapse it); the active cell stays.
        ctx->KeyPress(ImGuiKey_End | ImGuiMod_Ctrl | ImGuiMod_Shift);
        ctx->Yield(2);
        IM_CHECK_NO_RET(ActiveAt(0, 1));
        IM_CHECK_NO_RET(sel.RectContains(0, 1) && sel.RectContains(kFixtureRows - 1, lastCol));
    };
}

// GridSpreadsheet/Keys_ActiveCellScrollsClearOfFrozenHeader
static void RegisterScrollIntoView(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Keys_ActiveCellScrollsClearOfFrozenHeader");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        SPREADSHEET_REQUIRE(ClickCell(ctx, 0, 0));
        ctx->KeyPress(ImGuiKey_End | ImGuiMod_Ctrl); // the last cell: scrolls to the bottom
        ctx->Yield(4);
        IM_CHECK_NO_RET(ActiveAt(kFixtureRows - 1, ColumnCount() - 1));
        const ImGuiTable* table = GridTable();
        SPREADSHEET_REQUIRE(table != nullptr);
        ImRect rect;
        SPREADSHEET_REQUIRE(CellRect(ctx, kFixtureRows - 1, 0, &rect));
        IM_CHECK_NO_RET(rect.Max.y <= table->InnerClipRect.Max.y + 1.0f); // the last row is not below the window

        // Walk up past the top of the viewport: every step keeps the active row below the frozen header row.
        for (int step = 0; step < 40; ++step) {
            ctx->KeyPress(ImGuiKey_UpArrow);
            ctx->Yield(2);
            table = GridTable();
            SPREADSHEET_REQUIRE(table != nullptr);
            SPREADSHEET_REQUIRE(CellRect(ctx, State().ActiveRow, 0, &rect));
            // The ID cell's selectable reaches half an item spacing past the cell; a row hidden by the header is a
            // whole row (24 px) off, far beyond that.
            const float headerBottom = table->BgClipRect.Min.y - ImGui::GetStyle().ItemSpacing.y;
            if (rect.Min.y < headerBottom) {
                ctx->LogError("row %d sits under the frozen header (top %.1f < %.1f)", State().ActiveRow, rect.Min.y,
                              headerBottom);
                IM_CHECK_NO_RET(false);
                return;
            }
        }
        IM_CHECK_NO_RET(State().ActiveRow == kFixtureRows - 1 - 40);

        // The same sideways: from the last column, every step left keeps the active column clear of the frozen ID
        // column.
        ctx->KeyPress(ImGuiKey_End);
        ctx->Yield(4);
        for (int step = 0; step < ColumnCount() - 1; ++step) {
            ctx->KeyPress(ImGuiKey_LeftArrow);
            ctx->Yield(2);
            table = GridTable();
            SPREADSHEET_REQUIRE(table != nullptr);
            const int col = State().ActiveCol;
            if (col > 0 && table->Columns[col].MinX < table->Columns[0].MaxX - ImGui::GetStyle().ItemSpacing.x) {
                ctx->LogError("column %d sits under the frozen ID column (left %.1f < %.1f)", col,
                              table->Columns[col].MinX, table->Columns[0].MaxX);
                IM_CHECK_NO_RET(false);
                return;
            }
        }
        IM_CHECK_NO_RET(State().ActiveCol == 0);
    };
}

// GridSpreadsheet/Gesture_DragBetweenCellsSelectsRangeWithoutEditor
static void RegisterDragSelects(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Gesture_DragBetweenCellsSelectsRangeWithoutEditor");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        ImRect from;
        ImRect to;
        SPREADSHEET_REQUIRE(CellRect(ctx, 1, 1, &from) && CellRect(ctx, 3, 2, &to)); // summary column to the next one
        ctx->MouseMoveToPos(from.GetCenter());
        ctx->Yield();
        ctx->MouseDown(0);
        ctx->Yield(2);
        ctx->MouseMoveToPos(to.GetCenter());
        ctx->Yield(2);
        ctx->MouseUp(0);
        ctx->Yield(3);
        const SpreadsheetState& gs = State();
        IM_CHECK_NO_RET(gs.EditingId.empty()); // no editor opened under the drag
        IM_CHECK_NO_RET(gs.RectSel.RangeMulti);
        IM_CHECK_NO_RET(gs.RectSel.RectContains(1, 1) && gs.RectSel.RectContains(3, 2) &&
                        gs.RectSel.RectContains(2, 1));
        IM_CHECK_NO_RET(!gs.RectSel.RectContains(0, 1) && !gs.RectSel.RectContains(4, 2));
        IM_CHECK_NO_RET(gs.ActiveRow == 1 && gs.ActiveCol == 1); // the press cell stays the active cell
    };
}

// GridSpreadsheet/Identity_ActiveCellAndRangeSurviveASyncAndASearch
static void RegisterIdentity(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Identity_ActiveCellAndRangeSurviveASyncAndASearch");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        // SMAT-5 .. SMAT-7 in the summary column, active on SMAT-5.
        SPREADSHEET_REQUIRE(ClickCell(ctx, 4, 0));
        SPREADSHEET_REQUIRE(PressMoves(ctx, ImGuiKey_RightArrow, 4, 1));
        ctx->KeyPress(ImGuiKey_DownArrow | ImGuiMod_Shift);
        ctx->KeyPress(ImGuiKey_DownArrow | ImGuiMod_Shift);
        ctx->Yield(2);
        SPREADSHEET_REQUIRE(ActiveAt(4, 1) && State().RectSel.RectContains(6, 1));
        SPREADSHEET_REQUIRE(IdAtRow(4) == "SMAT-5");

        // A sync republishes the snapshot (a new revision, so a new sort signature): the range stays on its issues.
        const std::uint64_t revisionBefore = Pane().snapshotRevision;
        app->SyncWithBackend();
        SPREADSHEET_REQUIRE(StepUntil(ctx, 900, [&] { return Pane().snapshotRevision != revisionBefore; }));
        SPREADSHEET_REQUIRE(StepUntil(ctx, 900, [&] { return !app->IsSyncPendingOrActive(); }));
        ctx->Yield(4);
        IM_CHECK_NO_RET(ActiveAt(4, 1));
        IM_CHECK_NO_RET(State().RectSel.Active && State().RectSel.RangeMulti);
        IM_CHECK_NO_RET(State().RectSel.RectContains(4, 1) && State().RectSel.RectContains(6, 1));
        IM_CHECK_NO_RET(!State().RectSel.RectContains(3, 1) && !State().RectSel.RectContains(7, 1));

        // A search that narrows the grid moves the rows without a new signature: the active cell follows SMAT-5, and
        // comes back at its row when the box is cleared.
        ctx->ItemInput("##GridFilter");
        ctx->KeyCharsReplace("issue 5");
        ctx->Yield(3);
        const int shown = static_cast<int>(Pane().filteredIndices.size());
        IM_CHECK_NO_RET(shown > 0 && shown < kFixtureRows);
        const int row5 = RowOf("SMAT-5");
        SPREADSHEET_REQUIRE(row5 >= 0);
        IM_CHECK_NO_RET(ActiveAt(row5, 1));
        IM_CHECK_NO_RET(!State().RectSel.Active); // a search clears the range (and the row selection) as before
        // The box owns the keys while it is being typed in: an arrow there must not move the grid cursor.
        ctx->KeyPress(ImGuiKey_DownArrow);
        ctx->Yield(2);
        IM_CHECK_NO_RET(ActiveAt(row5, 1));

        ctx->ItemInput("##GridFilter");
        ctx->KeyPress(ImGuiKey_A | ImGuiMod_Ctrl);
        ctx->KeyPress(ImGuiKey_Delete);
        ctx->Yield(3);
        IM_CHECK_NO_RET(static_cast<int>(Pane().filteredIndices.size()) == kFixtureRows);
        IM_CHECK_NO_RET(ActiveAt(4, 1));

        // A selected row (key column) is cleared by a search too, not brought back by the identity remap.
        SPREADSHEET_REQUIRE(ClickCell(ctx, 8, 0));
        SPREADSHEET_REQUIRE(State().RectSel.Rows.count(8) == 1);
        ctx->ItemInput("##GridFilter");
        ctx->KeyCharsReplace("issue 5");
        ctx->Yield(3);
        IM_CHECK_NO_RET(State().RectSel.Rows.empty());
        ctx->ItemInput("##GridFilter");
        ctx->KeyPress(ImGuiKey_A | ImGuiMod_Ctrl);
        ctx->KeyPress(ImGuiKey_Delete);
        ctx->Yield(3);
    };
}

// GridSpreadsheet/Gesture_SameCellClickOpensEditorOnlyInSingleClickMode
static void RegisterSingleClickPreference(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Gesture_SameCellClickOpensEditorOnlyInSingleClickMode");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        struct RestorePreference {
            bool Saved = g_ui.cfg.SingleClickToEditGridCells;
            ~RestorePreference() { g_ui.cfg.SingleClickToEditGridCells = Saved; }
        } restore;

        // Single-click off: a click only selects (the cell becomes active, no editor), a double-click opens the editor.
        g_ui.cfg.SingleClickToEditGridCells = false;
        SPREADSHEET_REQUIRE(ClickCell(ctx, 2, 1));
        IM_CHECK_NO_RET(ActiveAt(2, 1) && State().EditingId.empty());
        ImRect rect;
        SPREADSHEET_REQUIRE(CellRect(ctx, 2, 1, &rect));
        ctx->MouseMoveToPos(rect.GetCenter());
        ctx->MouseDoubleClick(0);
        ctx->Yield(3);
        IM_CHECK_NO_RET(State().EditingId == IdAtRow(2));
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        SPREADSHEET_REQUIRE(State().EditingId.empty());

        // Single-click on (the default): the same click opens the editor on release over the same cell.
        g_ui.cfg.SingleClickToEditGridCells = true;
        SPREADSHEET_REQUIRE(ClickCell(ctx, 3, 1));
        IM_CHECK_NO_RET(State().EditingId == IdAtRow(3));
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_NO_RET(State().EditingId.empty());
    };
}

// GridSpreadsheet/Edit_KeysOpenTheEditorOfTheActiveCell
static void RegisterEditOpens(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Edit_KeysOpenTheEditorOfTheActiveCell");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        SPREADSHEET_REQUIRE(ActivateSummaryCell(ctx, 2));
        const std::string original = SummaryOf(*app, IdAtRow(2));
        SPREADSHEET_REQUIRE(!original.empty());

        // F2: the editor opens with the whole value in the buffer, the caret at its end.
        ctx->KeyPress(ImGuiKey_F2);
        ctx->Yield(4);
        IM_CHECK_NO_RET(EditingSummaryAt(2));
        IM_CHECK_NO_RET(original == State().EditBuffer);
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_NO_RET(State().EditingId.empty());
        IM_CHECK_NO_RET(ActiveAt(2, 1)); // Esc ends the edit; the active cell stays
        IM_CHECK_NO_RET(SummaryOf(*app, IdAtRow(2)) == original);

        // Enter: the editor opens, and the next typed text replaces the (selected) value.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(4);
        IM_CHECK_NO_RET(EditingSummaryAt(2));
        ctx->KeyChars("zz");
        ctx->Yield(2);
        IM_CHECK_NO_RET(std::string(State().EditBuffer) == "zz");
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_NO_RET(State().EditingId.empty() && SummaryOf(*app, IdAtRow(2)) == original);

        // A typed character: the buffer starts as that text (type-to-edit replaces the value), the original stays the
        // dirty baseline so an edit that ends equal to it never writes.
        ctx->KeyChars("qr"); // a fast second keystroke lands in the editor too, none is lost in the hand-over
        ctx->Yield(4);
        IM_CHECK_NO_RET(EditingSummaryAt(2));
        IM_CHECK_NO_RET(std::string(State().EditBuffer) == "qr");
        IM_CHECK_NO_RET(State().EditInitialValue == original);
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_NO_RET(State().EditingId.empty() && SummaryOf(*app, IdAtRow(2)) == original);

        // A read-only cell (the key column) does not open anything.
        SPREADSHEET_REQUIRE(PressMoves(ctx, ImGuiKey_LeftArrow, 2, 0));
        ctx->KeyPress(ImGuiKey_F2);
        ctx->KeyChars("q");
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditingId.empty() && State().EditArmedKey.empty());
    };
}

// GridSpreadsheet/Edit_KeysOpenADropdownCell
static void RegisterEditOpensDropdown(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Edit_KeysOpenADropdownCell");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        SPREADSHEET_REQUIRE(ActivateSummaryCell(ctx, 2));
        // A dropdown cell (Priority): F2 and a typed character arm and open its popup; Esc closes it and the cell
        // stays.
        const int priorityCol = ColumnOf("priority");
        SPREADSHEET_REQUIRE(priorityCol > 1);
        for (int col = 1; col < priorityCol; ++col) {
            ctx->KeyPress(ImGuiKey_RightArrow);
        }
        ctx->Yield(3);
        SPREADSHEET_REQUIRE(ActiveAt(2, priorityCol));
        ctx->KeyPress(ImGuiKey_F2);
        ctx->Yield(4);
        IM_CHECK_NO_RET(!State().EditArmedKey.empty());
        IM_CHECK_NO_RET(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditArmedKey.empty());
        IM_CHECK_NO_RET(ActiveAt(2, priorityCol));
        ctx->KeyChars("h");
        ctx->Yield(5);
        IM_CHECK_NO_RET(!State().EditArmedKey.empty());
        IM_CHECK_NO_RET(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditArmedKey.empty() && ActiveAt(2, priorityCol));
        // An arm whose popup is gone and whose cell is not drawn must not leave the keys dead.
        State().EditArmedKey = "stale::arm";
        State().EditArmedJustOpened = false;
        ctx->Yield(2);
        IM_CHECK_NO_RET(State().EditArmedKey.empty());
        ctx->KeyPress(ImGuiKey_LeftArrow);
        ctx->Yield(3);
        IM_CHECK_NO_RET(ActiveAt(2, priorityCol - 1));
    };
}

// GridSpreadsheet/Edit_EnterAndTabCommitAndMoveEscRevertsAndStays
static void RegisterEditFinishes(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Edit_EnterAndTabCommitAndMoveEscRevertsAndStays");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        // A landed write can re-sort the grid (the active cell follows its issue), so every position below is read
        // from the order at the moment of the key press, and every expectation names an issue, not a row.
        const auto activeOn = [](const std::string& id, int col) {
            const SpreadsheetState& gs = State();
            return !id.empty() && gs.ActiveIssueId == id && gs.ActiveCol == col && gs.ActiveRow == RowOf(id);
        };
        const auto landed = [&](ImGuiTestContext* c, const std::string& id, const char* summary) {
            const bool ok = StepUntil(c, 600, [&] { return SummaryOf(*app, id) == summary; });
            c->Yield(3);
            return ok;
        };
        SPREADSHEET_REQUIRE(ActivateSummaryCell(ctx, 3));
        const std::string a = IdAtRow(3);
        const std::string b = IdAtRow(4);
        const std::string originalB = SummaryOf(*app, b);

        // Enter commits and moves the active cell down; the next cell's editor stays closed.
        ctx->KeyPress(ImGuiKey_F2);
        ctx->Yield(4);
        SPREADSHEET_REQUIRE(State().EditingId == a);
        ctx->KeyCharsReplace("Renamed A");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditingId.empty());
        IM_CHECK_NO_RET(activeOn(b, 1));
        SPREADSHEET_REQUIRE(landed(ctx, a, "Renamed A"));
        IM_CHECK_NO_RET(activeOn(b, 1)); // the write landed and re-sorted the grid: the cell stayed on its issue

        // Esc reverts and stays: nothing is written, the active cell stays on the cell that was being edited.
        ctx->KeyPress(ImGuiKey_F2);
        ctx->Yield(4);
        SPREADSHEET_REQUIRE(State().EditingId == b);
        ctx->KeyCharsReplace("never saved");
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditingId.empty() && activeOn(b, 1));
        IM_CHECK_NO_RET(SummaryOf(*app, b) == originalB);

        // Enter on an unchanged value writes nothing, and still moves down.
        const std::string below = IdAtRow(RowOf(b) + 1);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(4);
        SPREADSHEET_REQUIRE(State().EditingId == b);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditingId.empty() && activeOn(below, 1));
        IM_CHECK_NO_RET(SummaryOf(*app, b) == originalB);

        // Shift+Enter commits and moves up.
        const std::string above = IdAtRow(RowOf(below) - 1);
        ctx->KeyPress(ImGuiKey_F2);
        ctx->Yield(4);
        SPREADSHEET_REQUIRE(State().EditingId == below);
        ctx->KeyCharsReplace("Renamed B");
        ctx->KeyPress(ImGuiKey_Enter | ImGuiMod_Shift);
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditingId.empty() && activeOn(above, 1));
        SPREADSHEET_REQUIRE(landed(ctx, below, "Renamed B"));

        // Tab commits and moves right to the next EDITABLE cell (read-only columns are skipped); no focus jump.
        ctx->KeyPress(ImGuiKey_F2);
        ctx->Yield(4);
        SPREADSHEET_REQUIRE(State().EditingId == above);
        const ImGuiID navBefore = GImGui->NavId;
        ctx->KeyCharsReplace("Renamed C");
        ctx->KeyPress(ImGuiKey_Tab);
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditingId.empty());
        IM_CHECK_NO_RET(State().ActiveIssueId == above && State().ActiveCol > 1 && GImGui->NavId == navBefore);
        IM_CHECK_NO_RET(landed(ctx, above, "Renamed C"));
    };
}

// GridSpreadsheet/Undo_CtrlZRestoresASavedCellThroughTheToastAndTheGrid
static void RegisterUndoSingle(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Undo_CtrlZRestoresASavedCellThroughTheToastAndTheGrid");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        const auto editActiveTo = [&](const std::string& id, const char* text) {
            ctx->KeyPress(ImGuiKey_F2);
            ctx->Yield(4);
            IM_CHECK_NO_RET(State().EditingId == id);
            ctx->KeyCharsReplace(text);
            ctx->KeyPress(ImGuiKey_Enter);
            ctx->Yield(4);
            const bool ok = StepUntil(ctx, 600, [&] { return SummaryOf(*app, id) == text; });
            ctx->Yield(3);
            return ok;
        };
        SPREADSHEET_REQUIRE(ActivateSummaryCell(ctx, 5));
        SmatchetToastManager::Instance().DismissAllLive();
        const std::string a = IdAtRow(5);
        const std::string originalA = SummaryOf(*app, a);
        SPREADSHEET_REQUIRE(editActiveTo(a, "Undo A"));

        // The saved toast carries Undo; Ctrl+Z is the shortcut for it while it is up.
        SPREADSHEET_REQUIRE(
            StepUntil(ctx, 600, [] { return SmatchetToastManager::Instance().LiveUndoableCount() == 1; }));
        ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
        IM_CHECK_NO_RET(StepUntil(ctx, 900, [&] { return SummaryOf(*app, a) == originalA; }));

        // The toast gone, Ctrl+Z on the focused grid undoes the pane's newest edit from the history.
        const std::string b = State().ActiveIssueId;
        SPREADSHEET_REQUIRE(!b.empty() && b != a);
        const std::string originalB = SummaryOf(*app, b);
        SPREADSHEET_REQUIRE(editActiveTo(b, "Undo B"));
        SmatchetToastManager::Instance().DismissAllLive();
        ctx->Yield(2);
        IM_CHECK_NO_RET(SmatchetToastManager::Instance().LiveUndoableCount() == 0);
        ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
        IM_CHECK_NO_RET(StepUntil(ctx, 900, [&] { return SummaryOf(*app, b) == originalB; }));
        IM_CHECK_NO_RET(SummaryOf(*app, a) == originalA); // the first undo is not undone again
    };
}

// GridSpreadsheet/Undo_BulkSetUndoesEachRowToItsOwnValue
static void RegisterUndoBulk(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Undo_BulkSetUndoesEachRowToItsOwnValue");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        const TrackerField* priority = app->FindFieldById("priority");
        SPREADSHEET_REQUIRE(priority != nullptr);
        std::string trivialId;
        for (const TrackerFieldOption& option : priority->AllowedValueOptions) {
            if (option.Value == "Trivial") {
                trivialId = option.Id;
            }
        }
        SPREADSHEET_REQUIRE(!trivialId.empty());
        const auto priorityOf = [&](const std::string& id) {
            const std::shared_ptr<const std::vector<CachedTicket>> snapshot = app->GetActiveTicketsSnapshot();
            if (snapshot) {
                for (const CachedTicket& ticket : *snapshot) {
                    if (ticket.id == id) {
                        return ticket.GetFieldValue("priority");
                    }
                }
            }
            return std::string();
        };
        // One issue of each priority the fixture holds: High and Medium differ, so a shared undo would be visible.
        std::string high;
        std::string medium;
        const std::shared_ptr<const std::vector<CachedTicket>> tickets = app->GetActiveTicketsSnapshot();
        SPREADSHEET_REQUIRE(tickets != nullptr);
        for (const CachedTicket& ticket : *tickets) {
            const std::string value = ticket.GetFieldValue("priority");
            if (high.empty() && value == "High") {
                high = ticket.id;
            } else if (medium.empty() && value == "Medium") {
                medium = ticket.id;
            }
        }
        SPREADSHEET_REQUIRE(!high.empty() && !medium.empty());
        SmatchetToastManager::Instance().DismissAllLive();

        const std::vector<std::string> ids = {high, medium};
        SPREADSHEET_REQUIRE(StartBulkFieldEdit(*app, g_ui, Pane().id, *priority, ids,
                                               std::vector<std::string>{trivialId},
                                               smatchet::grid::bulk::BulkMode::Replace, false) != 0);
        const auto isTrivial = [&](const std::string& id) {
            const std::string value = priorityOf(id);
            return value == trivialId || value == "Trivial";
        };
        SPREADSHEET_REQUIRE(StepUntil(ctx, 900, [&] { return isTrivial(high) && isTrivial(medium); }));
        SPREADSHEET_REQUIRE(
            StepUntil(ctx, 600, [] { return SmatchetToastManager::Instance().LiveUndoableCount() == 1; }));

        ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
        const auto isOneOf = [&](const std::string& id, const char* name, const char* optionId) {
            const std::string value = priorityOf(id);
            return value == name || value == optionId;
        };
        IM_CHECK_NO_RET(
            StepUntil(ctx, 900, [&] { return isOneOf(high, "High", "2") && isOneOf(medium, "Medium", "3"); }));
    };
}

// GridSpreadsheet/Undo_RejectedValueComesBackInTheEditorUntilEsc
static void RegisterRejectedValue(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridSpreadsheet", "Undo_RejectedValueComesBackInTheEditorUntilEsc");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = nullptr;
        if (!PrepareGrid(ctx, &app)) {
            return;
        }
        BucketE::UiTestWriteScope writeScope;
        ctx->SetRef(kGridWindow);
        SPREADSHEET_REQUIRE(ActivateSummaryCell(ctx, 7));
        const std::string a = IdAtRow(7);
        const std::string original = SummaryOf(*app, a);
        const std::string key = BuildCellKey(a, "summary");
        // The state a commit the tracker rejected leaves (SmatchetGridFieldEditPipeline.cpp).
        CellWriteFeedback rejected;
        rejected.State = CellWriteState::Error;
        rejected.Message = "Summary is too long.";
        rejected.RejectedValue = "a title the tracker refused";
        g_ui.cellFeedbackByKey[key] = rejected;

        ctx->KeyPress(ImGuiKey_F2);
        ctx->Yield(4);
        SPREADSHEET_REQUIRE(State().EditingId == a);
        IM_CHECK_NO_RET(std::string(State().EditBuffer) == "a title the tracker refused");
        IM_CHECK_NO_RET(State().EditRejectedMessage == "Summary is too long.");
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(4);
        IM_CHECK_NO_RET(State().EditingId.empty());
        IM_CHECK_NO_RET(SummaryOf(*app, a) == original); // nothing was sent
        IM_CHECK_NO_RET(g_ui.cellFeedbackByKey[key].RejectedValue.empty());

        // Esc dropped it: the editor opens on the cell's own value again, with no message.
        ctx->KeyPress(ImGuiKey_F2);
        ctx->Yield(4);
        SPREADSHEET_REQUIRE(State().EditingId == a);
        IM_CHECK_NO_RET(std::string(State().EditBuffer) == original);
        IM_CHECK_NO_RET(State().EditRejectedMessage.empty());
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(4);
        g_ui.cellFeedbackByKey.erase(key);
    };
}

extern "C" void SmatchetRegisterGridSpreadsheetKeysTests(ImGuiTestEngine* engine) {
    RegisterArrowKeys(engine);
    RegisterScrollIntoView(engine);
    RegisterDragSelects(engine);
    RegisterIdentity(engine);
    RegisterSingleClickPreference(engine);
    RegisterEditOpens(engine);
    RegisterEditOpensDropdown(engine);
    RegisterEditFinishes(engine);
    RegisterUndoSingle(engine);
    RegisterUndoBulk(engine);
    RegisterRejectedValue(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
