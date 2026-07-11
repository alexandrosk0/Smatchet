// grid_pane_windows.test.cpp — bucket-E smoke for multi-grid-tabs Slice 2
// (docs/plans/multi-grid-tabs.md plan items 14-16, ADR-0018).
//
// Boot-open-assert coverage for the dockable grid-pane windows:
//   1. new   — click the "+" pane button in the primary grid window; assert a
//              second GridPane materialises and its window goes live.
//   2. split — DockInto the new pane window to the RIGHT of the primary window;
//              assert BOTH windows are Active the same frame (side-by-side, two
//              visible re-entrant grid renders per frame).
//   3. focus — the new pane window holds focus; assert focusedPaneId follows
//              (the Slice-2 single-live-context hand-over trigger).
//   4. close — WindowClose the new pane; assert the pane is removed and focus
//              falls back to the surviving pane (min-1 invariant).
//
// Follows the funcsize_grid_render.test.cpp fixture-coupled recipe: fixture-gate,
// boot-check, sync, data-check, focus latch, bare-ref widget probes. Registered
// behind the spawn-warmup gate like every other bucket-E test.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — docked-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>
#include <string>

extern UiDrawSession g_ui;

namespace {

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

bool FixtureEnvSet() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const bool set = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE") != nullptr;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return set;
}

} // namespace

// ---------------------------------------------------------------------------
// GridPanes / PaneNewSplitFocusClose
// ---------------------------------------------------------------------------
static void RegisterGridPaneNewSplitFocusClose(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridPanes", "PaneNewSplitFocusClose");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — fixture backend absent");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        app->SyncWithBackend();
        const bool syncDone = YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); });
        IM_CHECK_NO_RET(syncDone);
        IM_CHECK_NO_RET(!app->GetActiveTickets().empty());

        // Primary pane window live + focused (same recipe as funcsize_grid_render).
        const char* kPrimary = "Smatchet - Active Project";
        ctx->SetRef(kPrimary);
        const bool primaryLive = YieldUntil(ctx, [&] {
            g_ui.requestActiveProjectFocus = true;
            return WindowIsLive(kPrimary);
        });
        IM_CHECK_NO_RET(primaryLive);
        if (!primaryLive) {
            return;
        }
        IM_CHECK_NO_RET(g_ui.gridPanesLoaded);
        IM_CHECK_EQ_NO_RET(static_cast<int>(g_ui.gridPanes.size()), 1);

        // 1. NEW — click the "+" pane button (submitted right after the window Begin).
        // NOTE: the driver pre-seeds whisper_setup_completed=true — on a fresh
        // profile the ##WhisperSetupBanner floats over this header row and
        // swallows the click.
        ctx->ItemClick("+##PaneAdd");
        const bool paneAdded = YieldUntil(ctx, [&] { return g_ui.gridPanes.size() == 2; });
        IM_CHECK_NO_RET(paneAdded);
        if (!paneAdded) {
            return;
        }

        // The new pane's window name is cached during its first draw.
        const bool nameReady = YieldUntil(ctx, [&] { return !g_ui.gridPanes[1].cachedWindowName.empty(); });
        IM_CHECK_NO_RET(nameReady);
        if (!nameReady) {
            return;
        }
        const std::string paneTwoName = g_ui.gridPanes[1].cachedWindowName; // "<title>###GridPane:pane-2"
        const std::string paneTwoId = g_ui.gridPanes[1].id;
        const bool paneTwoLive = YieldUntil(ctx, [&] { return WindowIsLive(paneTwoName.c_str()); });
        IM_CHECK_NO_RET(paneTwoLive);

        // 3. FOCUS — the freshly opened pane window takes ImGui focus; the host must
        // flip focusedPaneId (the Slice-2 live-context hand-over trigger).
        const bool focusFollowed = YieldUntil(ctx, [&] { return g_ui.focusedPaneId == paneTwoId; });
        IM_CHECK_NO_RET(focusFollowed);

        // 2. SPLIT — drag pane 2 to the RIGHT of the primary window; both grids must
        // then be visible (Active) in the same frame: two re-entrant renders/frame.
        bool splitDone = false;
        if (paneTwoLive) {
            // Absolute "//" refs — the test is under SetRef(kPrimary), and DockInto
            // resolves bare names relative to that (which dangles for window names).
            const std::string paneTwoAbsRef = std::string("//") + paneTwoName;
            const std::string primaryAbsRef = std::string("//") + kPrimary;
            ctx->DockInto(paneTwoAbsRef.c_str(), primaryAbsRef.c_str(), ImGuiDir_Right);
            const bool bothVisible =
                YieldUntil(ctx, [&] { return WindowIsLive(kPrimary) && WindowIsLive(paneTwoName.c_str()); });
            IM_CHECK_NO_RET(bothVisible);
            splitDone = bothVisible;
        }

        // 3b. SORT-CLICK INTO A NOT-YET-FOCUSED PANE — a
        // header sort click lands on the SAME frame ImGui moves window focus; the
        // view mirror must run off the live focus report, not last frame's
        // pane.focused. Focus the PRIMARY pane first, then click pane 2's "ID"
        // header and require the session view-sort dirty flags to flip (the mirror
        // marks dirty only when the view's sort rules actually changed).
        if (splitDone) {
            // Programmatic window focus — the requestActiveProjectFocus latch only
            // applies to the session-FOCUSED pane, which is pane 2 right now.
            ctx->WindowFocus("//Smatchet - Active Project");
            const bool primaryRefocused = YieldUntil(ctx, [&] { return g_ui.focusedPaneId == "main"; });
            IM_CHECK_NO_RET(primaryRefocused);
            if (primaryRefocused) {
                g_ui.viewSortDirty = false;
                g_ui.viewsDirty = false;
                const std::string paneTwoTableRef = std::string("//") + paneTwoName + "/TicketGrid";
                ctx->TableClickHeader(paneTwoTableRef.c_str(), "ID");
                const bool sortMirrored = YieldUntil(ctx, [&] { return g_ui.viewSortDirty && g_ui.viewsDirty; });
                IM_CHECK_NO_RET(sortMirrored); // HIGH-3: mirror survives the focus-click frame
                const bool focusFollowedClick = YieldUntil(ctx, [&] { return g_ui.focusedPaneId == paneTwoId; });
                IM_CHECK_NO_RET(focusFollowedClick);
            }
        }

        // Record the survivor's saved view BEFORE the close so we can assert the
        // focused-close hand-over never rebinds it.
        const std::string survivorViewIdBeforeClose = g_ui.gridPanes.front().viewId;

        // 4. CLOSE — flip the pane's open flag exactly as the tab X writes it (the X
        // is stock ImGui; ImGuiTestContext::WindowClose can't reach a docked tab's
        // close button by window ref). The host sweep must remove the pane, keep the
        // min-1 invariant, and fall focus back to the survivor.
        if (GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId)) {
            paneTwo->open = false;
        }
        const bool paneClosed = YieldUntil(ctx, [&] { return g_ui.gridPanes.size() == 1; });
        IM_CHECK_NO_RET(paneClosed);
        if (paneClosed) {
            IM_CHECK_EQ_NO_RET(g_ui.gridPanes.front().open, true);
            const bool focusFellBack = YieldUntil(ctx, [&] { return g_ui.focusedPaneId == g_ui.gridPanes.front().id; });
            IM_CHECK_NO_RET(focusFellBack);
            // HIGH-2: the survivor keeps ITS OWN saved view across the host focus
            // reassignment + the following frames' steady-state sync (the bug rebound
            // it to the closed pane's still-active view and persisted the loss).
            ctx->Yield();
            ctx->Yield();
            ctx->Yield();
            IM_CHECK_STR_EQ_NO_RET(g_ui.gridPanes.front().viewId.c_str(), survivorViewIdBeforeClose.c_str());
        }
    };
}

extern "C" void SmatchetRegisterGridPaneWindowsTests(ImGuiTestEngine* engine) {
    RegisterGridPaneNewSplitFocusClose(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
