// funcsize_grid_render.test.cpp — Phase-0 YELLOW pilot of the
// full-function-size-compliance program (docs/plans/shipped/full-function-size-compliance.md).
//
// PURPOSE — STRUCTURAL coverage for the DATA-DEPENDENT ImGui window-draw
// functions slated for decomposition in Phase 5. Unlike the green-pilot
// no-backend windows (Log/Audit/Preferences in funcsize_window_render_smoke.test.cpp),
// the active-project grid renders nothing meaningful WITHOUT loaded tickets:
// the header toolbar runs, but the ticket table and cell editors short-circuit
// on an empty `tickets` snapshot. A no-backend test would only tick the
// empty-state path — coverage theater. This pilot proves the WITH-BACKEND
// recipe: boot with the deterministic Jira fixture, sync, wait for SMAT-1/2 to
// land in GetActiveTickets(), then assert the grid window's REAL draw body
// ticked on real data.
//
// Like the green pilot there is deliberately NO checked-in pixel golden — the
// coverage is the live tick: if a future decomposition of DrawGridHeaderToolbar
// (447L) / drawActiveProjectWindow introduces a Begin/End or PushID/PopID
// imbalance, the ImGui Test Engine traps the assertion inside the running frame
// loop and the test fails.
//
// REQUIRES the fixture env var SMATCHET_TEST_JIRA_BACKEND_FIXTURE (set by the
// scripts/dev/test-ui-funcsize-grid-render.sh driver). Without it the fixture
// factory is absent, no tickets ever load, and the test SKIPS with an
// informational log rather than asserting on an empty grid.
//
// REUSABLE FIXTURE-COUPLED PATTERN (copy this for the remaining yellow windows
// whose body needs loaded tickets/fields — drawViewsDashboardWindow,
// RenderNewIssueDraftRow, the ticket-grid rows, the cell editors):
//   1. fixture-gate  — FixtureEnvSet(); skip-with-log if unset.
//   2. boot-check    — SmatchetActiveUiTestAppController(); skip-with-log if null.
//   3. sync          — app->SyncWithBackend(); YieldUntil(!IsStreamingSyncActive()).
//   4. data-check    — IM_CHECK(!GetActiveTickets().empty()) (proves fixture loaded).
//   5. focus         — re-arm requestActiveProjectFocus each frame until the
//                      window is live (it docks alongside Views, so it can open
//                      as an inactive tab where Begin() returns false).
//   6. assert-widget — ctx->ItemExists("<robust toolbar/row label>") with a
//                      BARE ref (no "<window>/" prefix — it double-prefixes).
//
// Window covered: drawActiveProjectWindow / DrawGridHeaderToolbar
//   - Title: "Smatchet - Active Project" (NoTitleBar, always drawn — no g_ui
//     visibility flag gates it; it ticks every frame unconditionally).
//   - Robust probe: "##GridFilter" — the quick-filter InputTextWithHint
//     submitted unconditionally by DrawGridHeaderToolbar immediately after the
//     window's Begin(), independent of whether an active view exists. Its
//     presence proves the toolbar body ran past Begin() on real ticket data.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>

// g_ui — the shared bag of UI-thread state. The active-project grid reads its
// requestActiveProjectFocus latch to bring the docked window forward, exactly
// like the View menu does.
extern UiDrawSession g_ui;

namespace {

// Yield frames until predicate returns true or the frame budget is exhausted.
// Returns true if the predicate became true within the budget.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True once the active-project window exists and is Active (Begin() returned
// true this frame, so its toolbar children were submitted). FindWindowByName
// resolves docked windows by plain title where ctx->WindowInfo() does not, and
// returns nullptr on a miss so the polling loop doesn't mark the context errored.
bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// True if the fixture env var was set — tests skip without it (no tickets load).
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
// FuncSizeGrid / ActiveProjectGrid_RendersToolbarOnFixtureData
// Boot with the basic-grid fixture, sync, wait for SMAT-1/2 to land in the
// active-tickets vector, focus the always-drawn active-project window, then
// assert the header toolbar's quick-filter input exists — proving
// DrawGridHeaderToolbar ticked its real body on loaded ticket data.
// ---------------------------------------------------------------------------
static void RegisterActiveProjectGridRendersToolbar(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizeGrid", "ActiveProjectGrid_RendersToolbarOnFixtureData");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!FixtureEnvSet()) {
            ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — fixture backend absent, no tickets load");
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // Drive a sync through the fixture-backed factory and wait for completion.
        app->SyncWithBackend();
        const bool syncDone = YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); });
        IM_CHECK_NO_RET(syncDone);

        // Proves the fixture loaded — SMAT-1/SMAT-2 are now in the grid model.
        const auto tickets = app->GetActiveTickets();
        IM_CHECK_NO_RET(!tickets.empty());

        // The active-project window is drawn unconditionally every frame (no
        // visibility flag), but it docks alongside Views & Queries, so it can
        // come up as an inactive tab where Begin() returns false. Re-arm the
        // focus latch each frame until it activates — same recipe the View menu
        // uses to bring a docked panel forward.
        const char* kWindowTitle = "Smatchet - Active Project";
        ctx->SetRef(kWindowTitle);
        const bool visible = YieldUntil(ctx, [&] {
            g_ui.requestActiveProjectFocus = true;
            return WindowIsLive(kWindowTitle);
        });
        IM_CHECK_NO_RET(visible);

        // The real DrawGridHeaderToolbar body has now ticked for several frames
        // on loaded ticket data. "##GridFilter" is the InputTextWithHint
        // submitted unconditionally right after Begin() — its presence confirms
        // the toolbar ran past Begin() without an ImGui assertion firing
        // mid-draw (the engine would have trapped one). Bare ref: it resolves
        // relative to the SetRef(kWindowTitle) above.
        if (visible) {
            const bool filterPresent = ctx->ItemExists("##GridFilter");
            IM_CHECK_NO_RET(filterPresent);
        }
    };
}

extern "C" void SmatchetRegisterFuncSizeGridRenderTests(ImGuiTestEngine* engine) {
    RegisterActiveProjectGridRendersToolbar(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
