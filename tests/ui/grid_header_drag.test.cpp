// grid_header_drag.test.cpp — bucket-E, end to end on the real ticket grid (deterministic Jira
// fixture backend): a column header can be dragged from one end of a horizontally scrolling grid
// to the other in ONE drag, and doing so keeps every column's width and leaves the Views editor
// clean.
//
// What this pins, each a user-reported failure:
//   1. The drag used to stop after a single neighbour swap: the reorder was committed to the view
//      mid-drag, which rebuilt the column array under ImGui and renumbered the held column.
//   2. The held column could never travel past what fit on screen: ImGui's own header drag moves
//      one neighbour per frame and never auto-scrolls the table.
//   3. After any reorder each column showed the previous occupant's width: ImGui keeps widths by
//      column INDEX, the view keeps them by KEY, and nothing re-synced the two.
//   4. After a grid drag, switching views prompted "unsaved changes" and "Save & switch" reverted
//      the drag: the Views editor's draft was a stale load-time snapshot.
//
// Drive: real mouse input through the test engine (press on a header, move into the grid's edge
// auto-scroll band, hold, release). Observation: the grid pane's drawn column keys (the committed
// view order), the live ImGuiTable's per-index widths mapped back to keys, and the editor draft.
// Needs SMATCHET_TEST_JIRA_BACKEND_FIXTURE (skips cleanly without it); registered under the
// JiraDeterministic category so the CI fixture-backend lane runs it. Run locally with:
//   UI_TEST_FILTER=JiraDeterministic/GridHeaderDrag bash scripts/dev/test-ui-jira-deterministic-backend.sh
//
// The GridHeaderRemoveColumn tests below share these helpers: a plain right-click on a field header
// reaches "Remove column from view" in ImGui's own table menu, which stays otherwise intact, ID does
// not offer it, Shift+right-click still opens the metadata panel, and only the clicked pane changes.
// Run them with UI_TEST_FILTER=GridHeaderRemoveColumn.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "GridPane.h"
#include "SmatchetGridColumnInteraction_detail.h"
#include "SmatchetGridUiSupport.h" // ComputeGridSortSignature
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "ViewColumnsPure.h"
#include "ui_test_skip.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern UiDrawSession g_ui;

namespace {

const char* const kGridWindow = "Smatchet - Active Project";

// Steps frames until `done()` holds or `budget` frames have passed; returns the final verdict.
template <typename Done> bool StepUntil(ImGuiTestContext* ctx, int budget, Done done) {
    while (!done() && budget-- > 0) {
        ctx->Yield();
    }
    return done();
}

// A pane's live TicketGrid table, the primary's by default (its scrolling child is named
// "<window>/TicketGrid_<id>").
ImGuiTable* LiveTicketGrid(const char* windowName = kGridWindow) {
    ImGuiContext& g = *GImGui;
    for (int n = 0; n < g.Tables.GetMapSize(); ++n) {
        ImGuiTable* table = g.Tables.TryGetMapData(n);
        if (table == nullptr || table->InnerWindow == nullptr || table->OuterWindow == nullptr ||
            table->LastFrameActive < g.FrameCount - 1) {
            continue;
        }
        const ImGuiWindow* root = table->OuterWindow->RootWindow;
        if (std::strstr(table->InnerWindow->Name, "/TicketGrid_") != nullptr && root != nullptr &&
            std::strcmp(root->Name, windowName) == 0) {
            return table;
        }
    }
    return nullptr;
}

const std::vector<std::string>& DrawnKeys() { return g_ui.gridPanes.front().lastDrawnColumnKeys; }

std::unordered_map<std::string, float> WidthsByKey(const ImGuiTable& table) {
    std::unordered_map<std::string, float> widths;
    const std::vector<std::string>& keys = DrawnKeys();
    for (int i = 0; i < table.ColumnsCount && i < static_cast<int>(keys.size()); ++i) {
        widths[keys[static_cast<size_t>(i)]] = table.Columns[i].WidthGiven;
    }
    return widths;
}

int IndexOfKey(const std::string& key) {
    const std::vector<std::string>& keys = DrawnKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

float HeaderRowY(ImGuiTable& table) {
    const float headerH = ImGui::TableGetInstanceData(&table, 0)->LastTopHeadersRowHeight;
    return table.InnerClipRect.Min.y + (headerH > 0.0f ? headerH : ImGui::GetFrameHeight()) * 0.5f;
}

float ColumnCenterX(const ImGuiTable& table, int columnIndex) {
    const ImGuiTableColumn& c = table.Columns[columnIndex];
    return 0.5f * (c.MinX + c.MaxX);
}

// Focuses the grid, then steps until its visible strip and scroll range have held still for a
// few frames: closing or docking a window next to the grid re-lays it out over several frames,
// and a grab point computed mid-relayout lands on the wrong header.
bool FocusGrid(ImGuiTestContext* ctx) {
    const bool live = StepUntil(ctx, 300, [] {
        g_ui.requestActiveProjectFocus = true;
        return LiveTicketGrid() != nullptr && !g_ui.gridPanes.empty() && !DrawnKeys().empty();
    });
    if (!live) {
        return false;
    }
    const int kQuietFrames = 8;
    ImRect lastClip;
    float lastScrollMax = -1.0f;
    int quiet = 0;
    for (int frame = 0; frame < 240 && quiet < kQuietFrames; ++frame) {
        ctx->Yield();
        const ImGuiTable* table = LiveTicketGrid();
        if (table == nullptr) {
            quiet = 0;
            continue;
        }
        const ImRect clip = table->InnerClipRect;
        const float scrollMax = table->InnerWindow->ScrollMax.x;
        const bool still = clip.Min.x == lastClip.Min.x && clip.Max.x == lastClip.Max.x &&
                           clip.Min.y == lastClip.Min.y && scrollMax == lastScrollMax;
        quiet = still ? quiet + 1 : 0;
        lastClip = clip;
        lastScrollMax = scrollMax;
    }
    return quiet >= kQuietFrames && LiveTicketGrid() != nullptr;
}

// Press on `key`'s header, carry it into the edge band on `toRight` side, hold until the grid has
// scrolled all the way and the column sits at `wantOrder`, then release. Returns false (after a
// logged check failure) when the grid or the header vanished mid-gesture.
bool DragHeaderIntoEdgeAndHold(ImGuiTestContext* ctx, const std::string& key, bool toRight, int wantOrder) {
    ImGuiTable* table = LiveTicketGrid();
    const int index = IndexOfKey(key);
    if (table == nullptr || index < 0) {
        IM_CHECK_NO_RET(table != nullptr && index >= 0);
        return false;
    }
    const float y = HeaderRowY(*table);
    const ImVec2 grab(ColumnCenterX(*table, index), y);
    ctx->MouseMoveToPos(grab);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(grab.x + (toRight ? 24.0f : -24.0f), y)); // past the drag threshold
    // The press must have caught THIS column's header; anything else would only time out below.
    const ImGuiTable* pressed = LiveTicketGrid();
    const bool heldRight = pressed != nullptr && pressed->HeldHeaderColumn == index;
    IM_CHECK_NO_RET(heldRight);
    if (!heldRight) {
        ctx->MouseUp(ImGuiMouseButton_Left);
        return false;
    }

    float stripMinX = 0.0f;
    float stripMaxX = 0.0f;
    smatchet::ui::GridScrollableStrip(*table, stripMinX, stripMaxX);
    ctx->MouseMoveToPos(ImVec2(toRight ? stripMaxX - 4.0f : stripMinX + 4.0f, y));

    const bool arrived = StepUntil(ctx, 900, [&] {
        ImGuiTable* live = LiveTicketGrid();
        if (live == nullptr) {
            return false;
        }
        const float scroll = live->InnerWindow->Scroll.x;
        const bool scrolledToEnd = toRight ? scroll >= live->InnerWindow->ScrollMax.x - 1.0f : scroll <= 1.0f;
        return scrolledToEnd && live->Columns[index].DisplayOrder == wantOrder;
    });
    ctx->MouseUp(ImGuiMouseButton_Left);
    IM_CHECK_NO_RET(arrived);
    return arrived;
}

// Drags `key`'s right header border by `deltaPx` (grid scrolled to its left end first so the
// border is on screen). Returns the column's new rendered width, or -1 when it is gone.
float ResizeColumnBy(ImGuiTestContext* ctx, const std::string& key, float deltaPx) {
    ImGuiTable* table = LiveTicketGrid();
    const int index = IndexOfKey(key);
    if (table == nullptr || index < 0) {
        return -1.0f;
    }
    ImGui::SetScrollX(table->InnerWindow, 0.0f);
    ctx->Yield();
    ctx->Yield();
    table = LiveTicketGrid();
    if (table == nullptr) {
        return -1.0f;
    }
    const float y = HeaderRowY(*table);
    const float borderX = table->Columns[index].MaxX - 1.0f;
    ctx->MouseMoveToPos(ImVec2(borderX, y));
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(ImVec2(borderX + deltaPx, y));
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield();
    ctx->Yield();
    table = LiveTicketGrid();
    const int after = IndexOfKey(key);
    return (table != nullptr && after >= 0) ? table->Columns[after].WidthGiven : -1.0f;
}

// Scenario body: left end -> right end -> left end in two drags, asserting order, widths and the
// Views editor draft along the way. Returns early (with a logged check) on the first failure.
void DragAcrossAndBack(ImGuiTestContext* ctx, const std::vector<std::string>& original) {
    const std::string& dragged = original[1];
    const std::unordered_map<std::string, float> widths = WidthsByKey(*LiveTicketGrid());

    // 1) One drag, left end -> right end, auto-scrolling on the way.
    if (!DragHeaderIntoEdgeAndHold(ctx, dragged, /*toRight=*/true, static_cast<int>(original.size()) - 1)) {
        return;
    }
    std::vector<std::string> expected = original;
    expected.erase(expected.begin() + 1);
    expected.push_back(dragged);
    const bool committedRight = StepUntil(ctx, 60, [&] { return DrawnKeys() == expected; });
    IM_CHECK_NO_RET(committedRight);

    // Widths stay with their columns, not with their old index.
    ctx->Yield();
    const std::unordered_map<std::string, float> afterRight = WidthsByKey(*LiveTicketGrid());
    for (const auto& kv : widths) {
        const auto it = afterRight.find(kv.first);
        IM_CHECK_NO_RET(it != afterRight.end() && std::fabs(it->second - kv.second) <= 1.0f);
    }

    // 2) The Views editor adopts the grid's order and stays clean — no phantom
    //    "unsaved changes" confirm on the next view switch.
    g_ui.showViewsDashboard = true;
    const bool draftAdopted = StepUntil(ctx, 120, [&] {
        std::vector<std::string> draftKeys;
        for (const ViewColumn& col : g_ui.viewDraft.Columns) {
            draftKeys.push_back(col.Key);
        }
        return draftKeys == expected;
    });
    IM_CHECK_NO_RET(draftAdopted);
    IM_CHECK_NO_RET(!ViewDraftHasUnsavedEdits(g_ui.viewDraft, g_ui.viewDraftBase));
    g_ui.showViewsDashboard = false;
    if (!FocusGrid(ctx)) {
        IM_CHECK_NO_RET(false);
        return;
    }

    // 3) And back again: right end -> left end, auto-scrolling left.
    if (!DragHeaderIntoEdgeAndHold(ctx, dragged, /*toRight=*/false, 1)) {
        return;
    }
    const bool committedLeft = StepUntil(ctx, 60, [&] { return DrawnKeys() == original; });
    IM_CHECK_NO_RET(committedLeft);
    ctx->Yield();
    const std::unordered_map<std::string, float> afterLeft = WidthsByKey(*LiveTicketGrid());
    for (const auto& kv : widths) {
        const auto it = afterLeft.find(kv.first);
        IM_CHECK_NO_RET(it != afterLeft.end() && std::fabs(it->second - kv.second) <= 1.0f);
    }
}

// Restores the first-run flag this test sets for the rest of the in-process suite.
struct WhisperBannerScope {
    const bool saved = g_ui.cfg.WhisperSetupCompleted;
    WhisperBannerScope() { g_ui.cfg.WhisperSetupCompleted = true; } // keep the banner off the header row
    ~WhisperBannerScope() { g_ui.cfg.WhisperSetupCompleted = saved; }
};

void RegisterGridHeaderDragAcrossScroll(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "JiraDeterministic", "GridHeaderDrag_AcrossScrolledGridKeepsWidths");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!SmatchetUiTestHasJiraFixture()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — fixture backend absent");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        WhisperBannerScope bannerScope;
        app->SyncWithBackend();
        IM_CHECK_NO_RET(StepUntil(ctx, 600, [&] { return !app->IsSyncPendingOrActive(); }));
        ctx->SetRef(kGridWindow);
        if (!FocusGrid(ctx)) {
            IM_CHECK_NO_RET(false);
            return;
        }
        const std::vector<std::string> original = DrawnKeys();
        if (original.size() < 4) {
            ctx->LogInfo("SKIP: fixture view has fewer than 4 columns (%d)", static_cast<int>(original.size()));
            return;
        }
        const std::string dragged = original[1]; // first movable column ("id" is frozen at 0)

        // Load the Views editor draft against the ORIGINAL order, then close the editor again so
        // the grid is on screen for the drag. The draft persists in the session while closed.
        g_ui.showViewsDashboard = true;
        IM_CHECK_NO_RET(StepUntil(
            ctx, 300, [] { return !g_ui.viewDraftId.empty() && g_ui.viewDraftId == g_ui.gridPanes.front().viewId; }));
        g_ui.showViewsDashboard = false;
        if (!FocusGrid(ctx)) {
            IM_CHECK_NO_RET(false);
            return;
        }

        // Make the grid overflow horizontally: widen the first movable column until its right
        // border sits just inside the visible edge (still grabbable for the restore below),
        // pushing every later column off screen.
        ImGuiTable* grid = LiveTicketGrid();
        const float originalWidth = grid->Columns[IndexOfKey(dragged)].WidthGiven;
        float stripMinX = 0.0f;
        float stripMaxX = 0.0f;
        smatchet::ui::GridScrollableStrip(*grid, stripMinX, stripMaxX);
        ResizeColumnBy(ctx, dragged, (stripMaxX - 16.0f) - grid->Columns[IndexOfKey(dragged)].MaxX);
        const bool overflows = StepUntil(ctx, 60, [] {
            const ImGuiTable* table = LiveTicketGrid();
            return table != nullptr && table->InnerWindow->ScrollMax.x > 200.0f;
        });
        IM_CHECK_NO_RET(overflows);
        if (overflows) {
            DragAcrossAndBack(ctx, original);
        }

        // Put the widened column back so later tests in this process see the fixture layout.
        const std::unordered_map<std::string, float> endWidths = WidthsByKey(*LiveTicketGrid());
        const auto widened = endWidths.find(dragged);
        if (widened != endWidths.end()) {
            const float restored = ResizeColumnBy(ctx, dragged, originalWidth - widened->second);
            IM_CHECK_NO_RET(std::fabs(restored - originalWidth) <= 1.0f);
        }
    };
}

// ---- Header right-click menu: Remove column -------------------------------------------------

// The view.* handlers hop to the UI thread and block on it, so they run on a worker while this
// test keeps stepping frames.
smatchet::cmd::CommandResult RunCommand(ImGuiTestContext* ctx, AppController* app, const std::string& name,
                                        const nlohmann::json& args) {
    std::future<smatchet::cmd::CommandResult> pending = std::async(std::launch::async, [app, name, args] {
        smatchet::cmd::CommandContext cmdCtx;
        cmdCtx.ConfirmedDestructive = true;
        return app->Commands().Dispatch(name, args, cmdCtx);
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        ctx->Yield();
    }
    return pending.get();
}

// Puts the active view's columns back to `keys`, for the later tests in this process.
void RestoreActiveViewColumns(ImGuiTestContext* ctx, AppController* app, const std::vector<std::string>& keys) {
    nlohmann::json fields = nlohmann::json::array();
    for (const std::string& key : keys) {
        if (key != "id") {
            fields.push_back(key.substr(6));
        }
    }
    IM_CHECK_NO_RET(RunCommand(ctx, app, "view.update", {{"fields", fields}}).Ok);
    IM_CHECK_NO_RET(RunCommand(ctx, app, "view.set_column_order", {{"order", keys}}).Ok);
}

std::string RemoveColumnItemRef() {
    return std::string("//$FOCUSED/") + SmatchetLocalization::T("views.remove_column", "Remove column from view");
}

// Right-clicks `key`'s header in the pane whose window is `windowName` and whose drawn columns are
// `keys`, then lets the menu finish appearing.
bool RightClickHeader(ImGuiTestContext* ctx, const char* windowName, const std::vector<std::string>& keys,
                      const std::string& key, bool withShift = false) {
    ImGuiTable* table = LiveTicketGrid(windowName);
    const auto it = std::find(keys.begin(), keys.end(), key);
    if (table == nullptr || it == keys.end()) {
        IM_CHECK_NO_RET(table != nullptr && it != keys.end());
        return false;
    }
    ctx->MouseMoveToPos(ImVec2(ColumnCenterX(*table, static_cast<int>(it - keys.begin())), HeaderRowY(*table)));
    if (withShift) {
        ctx->KeyDown(ImGuiMod_Shift);
    }
    ctx->MouseClick(ImGuiMouseButton_Right);
    if (withShift) {
        ctx->KeyUp(ImGuiMod_Shift);
    }
    ctx->Yield(3);
    return true;
}

// What each right-click gesture offers: ID has no Remove, a field does, the sizing and order items
// stay, and Shift still opens the metadata panel instead.
void CheckHeaderMenuOffers(ImGuiTestContext* ctx, const std::vector<std::string>& keys, const std::string& field) {
    if (RightClickHeader(ctx, kGridWindow, keys, "id")) {
        IM_CHECK_NO_RET(ctx->ItemExists("//$FOCUSED/###SizeOne"));
        IM_CHECK_NO_RET(!ctx->ItemExists(RemoveColumnItemRef().c_str()));
        ctx->PopupCloseAll();
    }
    if (RightClickHeader(ctx, kGridWindow, keys, field)) {
        IM_CHECK_NO_RET(ctx->ItemExists("//$FOCUSED/###SizeOne"));
        IM_CHECK_NO_RET(ctx->ItemExists("//$FOCUSED/###ResetOrder"));
        IM_CHECK_NO_RET(ctx->ItemExists(RemoveColumnItemRef().c_str()));
        ctx->PopupCloseAll();
    }
    if (RightClickHeader(ctx, kGridWindow, keys, field, /*withShift=*/true)) {
        IM_CHECK_NO_RET(ctx->ItemExists("//$FOCUSED/Copy field id"));
        IM_CHECK_NO_RET(!ctx->ItemExists(RemoveColumnItemRef().c_str()));
        ctx->PopupCloseAll();
    }
}

// `keys` with the columns at `a` and `b` swapped.
std::vector<std::string> SwapKeys(std::vector<std::string> keys, size_t a, size_t b) {
    std::swap(keys[a], keys[b]);
    return keys;
}

// A column-order change from outside the header menu (a view.set_column_order, as a Views-editor
// Apply or a header drag in another pane would make) drops a rectangle selection, which addresses
// columns by index, and keeps the row selection, which does not. The selection is seeded with the
// grid's current sort signature, and first shown to survive unchanged frames, so only the column
// change can clear it. Expects and leaves the active view on `keys`.
void CheckColumnOrderChangeClearsRectSel(ImGuiTestContext* ctx, AppController* app,
                                         const std::vector<std::string>& keys) {
    GridPane& pane = g_ui.gridPanes.front();
    GridRectSelection& rect = pane.gridState.RectSel;
    rect.ClearAll();
    rect.Active = true;
    rect.AnchorRow = rect.ExtentRow = 0;
    rect.AnchorCol = rect.ExtentCol = 1;
    rect.Rows.insert(0);
    rect.SortSignature = ComputeGridSortSignature(pane.cachedSortFingerprint, pane.snapshotRevision,
                                                  pane.ticketsSnapshot ? pane.ticketsSnapshot->size() : 0);
    ctx->Yield(5);
    IM_CHECK_NO_RET(rect.Active); // control: nothing else clears it
    const std::vector<std::string> swapped = SwapKeys(keys, 1, 2);
    IM_CHECK_NO_RET(RunCommand(ctx, app, "view.set_column_order", {{"order", swapped}}).Ok);
    IM_CHECK_NO_RET(StepUntil(ctx, 120, [&] { return DrawnKeys() == swapped; }));
    IM_CHECK_NO_RET(!rect.Active);
    IM_CHECK_NO_RET(rect.Rows.count(0) == 1);
    rect.ClearAll();
    IM_CHECK_NO_RET(RunCommand(ctx, app, "view.set_column_order", {{"order", keys}}).Ok);
    IM_CHECK_NO_RET(StepUntil(ctx, 120, [&] { return DrawnKeys() == keys; }));
}

// The menu's Remove targets the column key latched when it opened, not ImGui's column index. Here
// the columns are reordered under the open menu (same column count, so ImGui keeps the menu open
// on the old index, which now holds a different column); Remove must still take off the column
// that was right-clicked. Expects the active view on `keys`; leaves it there with `keys[1]` re-added.
void CheckRemoveFollowsLatchedKeyAfterReorder(ImGuiTestContext* ctx, AppController* app,
                                              const std::vector<std::string>& keys) {
    const std::string clicked = keys[1];
    if (!RightClickHeader(ctx, kGridWindow, keys, clicked)) {
        return;
    }
    const std::vector<std::string> swapped = SwapKeys(keys, 1, 2);
    IM_CHECK_NO_RET(RunCommand(ctx, app, "view.set_column_order", {{"order", swapped}}).Ok);
    IM_CHECK_NO_RET(StepUntil(ctx, 120, [&] { return DrawnKeys() == swapped; }));
    ctx->Yield(2);
    IM_CHECK_NO_RET(ctx->ItemExists("//$FOCUSED/###ResetOrder")); // the menu is still open
    if (!ctx->ItemExists(RemoveColumnItemRef().c_str())) {
        IM_CHECK_NO_RET(false);
        ctx->PopupCloseAll();
        return;
    }
    ctx->ItemClick(RemoveColumnItemRef().c_str());
    std::vector<std::string> expected = swapped;
    expected.erase(std::find(expected.begin(), expected.end(), clicked));
    IM_CHECK_NO_RET(StepUntil(ctx, 60, [&] { return DrawnKeys() == expected; }));
    RestoreActiveViewColumns(ctx, app, keys);
    IM_CHECK_NO_RET(StepUntil(ctx, 120, [&] { return DrawnKeys() == keys; }));
}

// The re-added column comes back at a default width; put the fixture width back so the armed
// layout autosave does not persist a different one for the later tests in this process.
void RestoreRemovedColumnWidth(ImGuiTestContext* ctx, const std::string& key, float originalWidth) {
    ctx->Yield(2);
    const std::unordered_map<std::string, float> now = WidthsByKey(*LiveTicketGrid());
    const auto it = now.find(key);
    IM_CHECK_NO_RET(it != now.end());
    if (it != now.end() && std::fabs(it->second - originalWidth) > 1.0f) {
        const float restored = ResizeColumnBy(ctx, key, originalWidth - it->second);
        IM_CHECK_NO_RET(std::fabs(restored - originalWidth) <= 1.0f);
    }
}

void RegisterGridHeaderRemoveColumn(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "JiraDeterministic", "GridHeaderRemoveColumn_PlainRightClick");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (!SmatchetUiTestHasJiraFixture() || app == nullptr) {
            ctx->LogInfo("SKIP: fixture backend absent or app not booted");
            return;
        }
        WhisperBannerScope bannerScope;
        app->SyncWithBackend();
        IM_CHECK_NO_RET(StepUntil(ctx, 600, [&] { return !app->IsSyncPendingOrActive(); }));
        ctx->SetRef(kGridWindow);
        // Load the editor draft against the full column set, so the removal has a draft to stay
        // clean against. The draft persists in the session while the editor is closed.
        g_ui.showViewsDashboard = true;
        IM_CHECK_NO_RET(StepUntil(
            ctx, 300, [] { return !g_ui.viewDraftId.empty() && g_ui.viewDraftId == g_ui.gridPanes.front().viewId; }));
        g_ui.showViewsDashboard = false;
        if (!FocusGrid(ctx)) {
            IM_CHECK_NO_RET(false);
            return;
        }
        const std::vector<std::string> original = DrawnKeys();
        if (original.size() < 3) {
            ctx->LogInfo("SKIP: fixture view has fewer than 3 columns (%d)", static_cast<int>(original.size()));
            return;
        }
        const std::string field = original[1]; // the first column after the frozen "id"
        const std::unordered_map<std::string, float> widths = WidthsByKey(*LiveTicketGrid());

        CheckHeaderMenuOffers(ctx, original, field);

        g_ui.viewLayoutSaveAt = std::chrono::steady_clock::time_point::max();
        if (!RightClickHeader(ctx, kGridWindow, original, field)) {
            return;
        }
        ctx->ItemClick(RemoveColumnItemRef().c_str());
        std::vector<std::string> expected = original;
        expected.erase(expected.begin() + 1);
        IM_CHECK_NO_RET(StepUntil(ctx, 60, [&] { return DrawnKeys() == expected; }));
        // Autosaved like a header drag: the debounced layout save is armed.
        IM_CHECK_NO_RET(g_ui.viewLayoutSaveAt != std::chrono::steady_clock::time_point::max());
        // The next sync asks for the same fields a Fields-tab Apply would leave.
        IM_CHECK_NO_RET(std::find(g_ui.cfg.SelectedFields.begin(), g_ui.cfg.SelectedFields.end(), field.substr(6)) ==
                        g_ui.cfg.SelectedFields.end());

        // Every surviving column keeps its width, though each one after the removed column moved
        // to a new ImGui column index.
        ctx->Yield(2);
        const std::unordered_map<std::string, float> after = WidthsByKey(*LiveTicketGrid());
        for (const std::string& key : expected) {
            IM_CHECK_NO_RET(after.count(key) == 1 && std::fabs(after.at(key) - widths.at(key)) <= 1.0f);
        }

        // The editor follows the grid with nothing unsaved: no phantom "unsaved changes" prompt.
        g_ui.showViewsDashboard = true;
        IM_CHECK_NO_RET(StepUntil(ctx, 120, [&] {
            return std::none_of(g_ui.viewDraft.Columns.begin(), g_ui.viewDraft.Columns.end(),
                                [&](const ViewColumn& col) { return col.Key == field; });
        }));
        ctx->Yield(2);
        IM_CHECK_NO_RET(!ViewDraftHasUnsavedEdits(g_ui.viewDraft, g_ui.viewDraftBase));
        g_ui.showViewsDashboard = false;

        RestoreActiveViewColumns(ctx, app, original);
        IM_CHECK_NO_RET(StepUntil(ctx, 120, [&] { return DrawnKeys() == original; }));
        IM_CHECK_NO_RET(StepUntil(ctx, 600, [&] { return !app->IsSyncPendingOrActive(); }));
        CheckColumnOrderChangeClearsRectSel(ctx, app, original);
        CheckRemoveFollowsLatchedKeyAfterReorder(ctx, app, original); // removes and re-adds `field` again
        RestoreRemovedColumnWidth(ctx, field, widths.at(field));
    };
}

bool StoredViewHasColumn(ImGuiTestContext* ctx, AppController* app, const std::string& viewId, const std::string& key) {
    const smatchet::cmd::CommandResult view = RunCommand(ctx, app, "view.get", {{"id", viewId}});
    IM_CHECK_NO_RET(view.Ok && view.Data != nullptr);
    if (!view.Ok || view.Data == nullptr) {
        return false;
    }
    for (const nlohmann::json& col : (*view.Data)["columns"]) {
        if (col.value("key", std::string()) == key) {
            return true;
        }
    }
    return false;
}

// Removal reaches the view of the pane that was clicked and nothing else: two panes on two views,
// side by side, with the other pane focused when the right-click lands. The open menu can pull pane
// focus to the clicked pane (its popup counts as that pane's focus), so this cannot prove a click
// made while unfocused; it checks the stored views as well as what each pane draws. Two panes on the
// SAME view share its column list by design, so they change together.
void CheckRemovalStaysInClickedPane(ImGuiTestContext* ctx, AppController* app, const std::string& paneTwoId) {
    const GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId);
    IM_CHECK_NO_RET(paneTwo != nullptr);
    if (paneTwo == nullptr) {
        return;
    }
    const std::string name = paneTwo->cachedWindowName;
    const std::string paneTwoViewId = paneTwo->viewId;
    const std::string mainViewId = g_ui.gridPanes.front().viewId;
    const std::string absRef = std::string("//") + name;
    const std::string primaryAbsRef = std::string("//") + kGridWindow;
    ctx->DockInto(absRef.c_str(), primaryAbsRef.c_str(), ImGuiDir_Right);
    ctx->WindowFocus(primaryAbsRef.c_str());
    const bool settled = StepUntil(ctx, 300, [&] {
        const GridPane* pane = FindGridPaneById(g_ui.gridPanes, paneTwoId);
        return pane != nullptr && g_ui.focusedPaneId == "main" && !pane->lastDrawnColumnKeys.empty() &&
               LiveTicketGrid(name.c_str()) != nullptr && LiveTicketGrid() != nullptr;
    });
    IM_CHECK_NO_RET(settled);
    if (!settled) {
        return;
    }
    ctx->Yield(10); // the docked pane's layout settles
    const std::vector<std::string> mainKeys = DrawnKeys();
    const std::vector<std::string> paneTwoKeys = FindGridPaneById(g_ui.gridPanes, paneTwoId)->lastDrawnColumnKeys;
    IM_CHECK_NO_RET(paneTwoKeys.size() >= 3);
    if (paneTwoKeys.size() < 3 || !RightClickHeader(ctx, name.c_str(), paneTwoKeys, paneTwoKeys[1])) {
        return;
    }
    ctx->LogInfo("Focused pane when Remove is clicked: %s (clicked pane: %s)", g_ui.focusedPaneId.c_str(),
                 paneTwoId.c_str());
    ctx->ItemClick(RemoveColumnItemRef().c_str());

    std::vector<std::string> expected = paneTwoKeys;
    expected.erase(expected.begin() + 1);
    IM_CHECK_NO_RET(StepUntil(ctx, 60, [&] {
        const GridPane* pane = FindGridPaneById(g_ui.gridPanes, paneTwoId);
        return pane != nullptr && pane->lastDrawnColumnKeys == expected;
    }));
    ctx->Yield(5);
    IM_CHECK_NO_RET(DrawnKeys() == mainKeys);
    IM_CHECK_NO_RET(!StoredViewHasColumn(ctx, app, paneTwoViewId, paneTwoKeys[1]));
    IM_CHECK_NO_RET(StoredViewHasColumn(ctx, app, mainViewId, paneTwoKeys[1]));
}

void RegisterGridHeaderRemoveColumnClickedPaneOnly(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "JiraDeterministic", "GridHeaderRemoveColumn_ClickedPaneOnly");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (!SmatchetUiTestHasJiraFixture() || app == nullptr) {
            ctx->LogInfo("SKIP: fixture backend absent or app not booted");
            return;
        }
        WhisperBannerScope bannerScope;
        app->SyncWithBackend();
        IM_CHECK_NO_RET(StepUntil(ctx, 600, [&] { return !app->IsSyncPendingOrActive(); }));
        ctx->SetRef(kGridWindow);
        if (!FocusGrid(ctx) || g_ui.gridPanes.size() != 1) {
            IM_CHECK_NO_RET(false);
            return;
        }
        const std::vector<std::string> original = DrawnKeys();
        const std::string fixtureViewId = g_ui.gridPanes.front().viewId;

        // A second view with the same columns; creating it also makes it the active one.
        nlohmann::json columns = nlohmann::json::array();
        for (const std::string& key : original) {
            columns.push_back(key);
        }
        const smatchet::cmd::CommandResult created =
            RunCommand(ctx, app, "view.create", {{"name", "Remove column pane test"}, {"columns", columns}});
        IM_CHECK_NO_RET(created.Ok && created.Data != nullptr);
        if (!created.Ok || created.Data == nullptr) {
            return;
        }
        const std::string otherViewId = created.Data->value("id", std::string());
        ctx->Yield(5);

        // Open the second pane on whichever of the two views the primary is not showing.
        const std::string mainViewId = g_ui.gridPanes.front().viewId;
        g_ui.paneAddRequest.sourceId = g_ui.gridPanes.front().id;
        g_ui.paneAddRequest.targetViewId = mainViewId == otherViewId ? fixtureViewId : otherViewId;
        const bool paneReady = StepUntil(ctx, 300, [] {
            return g_ui.gridPanes.size() == 2 && g_ui.gridPanes[1].selectTabFrames == 0 &&
                   !g_ui.gridPanes[1].cachedWindowName.empty();
        });
        IM_CHECK_NO_RET(paneReady);
        if (paneReady) {
            const std::string paneTwoId = g_ui.gridPanes[1].id;
            IM_CHECK_NO_RET(g_ui.gridPanes[1].viewId != g_ui.gridPanes.front().viewId);
            CheckRemovalStaysInClickedPane(ctx, app, paneTwoId);
            if (GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId)) {
                paneTwo->open = false;
            }
            IM_CHECK_NO_RET(StepUntil(ctx, 120, [] { return g_ui.gridPanes.size() == 1; }));
        }

        // Back to the fixture view with its full column list, for the later tests in this process.
        IM_CHECK_NO_RET(RunCommand(ctx, app, "view.activate", {{"id", fixtureViewId}}).Ok);
        IM_CHECK_NO_RET(RunCommand(ctx, app, "view.delete", {{"id", otherViewId}}).Ok);
        IM_CHECK_NO_RET(RunCommand(ctx, app, "view.activate", {{"id", fixtureViewId}}).Ok);
        RestoreActiveViewColumns(ctx, app, original);
        IM_CHECK_NO_RET(StepUntil(
            ctx, 300, [&] { return g_ui.gridPanes.front().viewId == fixtureViewId && DrawnKeys() == original; }));
    };
}

} // namespace

extern "C" void SmatchetRegisterGridHeaderDragTests(ImGuiTestEngine* engine) {
    RegisterGridHeaderDragAcrossScroll(engine);
    RegisterGridHeaderRemoveColumn(engine);
    RegisterGridHeaderRemoveColumnClickedPaneOnly(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
