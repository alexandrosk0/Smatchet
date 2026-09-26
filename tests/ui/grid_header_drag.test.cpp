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

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "GridPane.h"
#include "SmatchetGridColumnInteraction_detail.h"
#include "SmatchetUiSession.h"
#include "ViewColumnsPure.h"
#include "ui_test_skip.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
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

// The primary pane's live TicketGrid table (its scrolling child is named "<window>/TicketGrid_<id>").
ImGuiTable* LiveTicketGrid() {
    ImGuiContext& g = *GImGui;
    for (int n = 0; n < g.Tables.GetMapSize(); ++n) {
        ImGuiTable* table = g.Tables.TryGetMapData(n);
        if (table == nullptr || table->InnerWindow == nullptr || table->OuterWindow == nullptr ||
            table->LastFrameActive < g.FrameCount - 1) {
            continue;
        }
        const ImGuiWindow* root = table->OuterWindow->RootWindow;
        if (std::strstr(table->InnerWindow->Name, "/TicketGrid_") != nullptr && root != nullptr &&
            std::strcmp(root->Name, kGridWindow) == 0) {
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

bool FocusGrid(ImGuiTestContext* ctx) {
    return StepUntil(ctx, 300, [] {
        g_ui.requestActiveProjectFocus = true;
        return LiveTicketGrid() != nullptr && !g_ui.gridPanes.empty() && !DrawnKeys().empty();
    });
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
        IM_CHECK_NO_RET(StepUntil(ctx, 600, [&] { return !app->IsStreamingSyncActive(); }));
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

} // namespace

extern "C" void SmatchetRegisterGridHeaderDragTests(ImGuiTestEngine* engine) {
    RegisterGridHeaderDragAcrossScroll(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
