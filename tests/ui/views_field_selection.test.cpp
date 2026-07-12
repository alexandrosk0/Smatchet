// views_field_selection.test.cpp — bucket-E regression coverage for the Views-editor field-selection
// reseed lifecycle (the #views-field-uncheck bug class). The authoritative selection set
// (UiDrawSession::selectedFieldSet) is reseeded straight from the active view's Fields by
// LoadBuffersFromView whenever the editing view changes (SmatchetViewsDashboardUi.cpp) — never
// re-derived from the truncating CSV buffer. The bug this guards: a stale/leaked entry surviving a
// view-switch (a checkbox staying checked when the newly-loaded view does not own that field). We
// drive the reseed and assert it CLEARS a polluted entry and restores EXACTLY the view's Fields.
//
// RENDER VEHICLE: UiMode::Mobile pins drawMobileShell; mobileDrawerOpen=true renders
// drawMobileDrawer -> drawMobileDrawerViews (SmatchetMobileShellUi.cpp), which calls
// LoadBuffersFromView when d.editingViewId != activeView->Id — the same reseed guard the desktop
// dashboard uses, reachable here through a single g_ui flag. A fresh backend key (cfg.TrackerType
// flip) bootstraps a non-empty default workspace so GetActiveView() is non-null.
//
// This covers the reseed/clear half of the lifecycle deterministically via g_ui. The toggle ->
// Apply-persist -> switch -> switch-back round-trip additionally needs the "Apply & Sync" UI flow
// (ViewState.UpdateActive) and is left as a residual (see the backlog entry).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiModeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>
#include <unordered_set>

// g_ui — the shared UI-thread state bag; the Views editor's closures read/write these same fields.
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
    const ImGuiWindow* w = ImGui::FindWindowByName(title);
    return w != nullptr && w->Active;
}

// Restore the g_ui fields the test mutates so the run leaves state as it found it.
struct ViewsDrawerStateGuard {
    UiMode uiMode = g_ui.cfg.UiMode;
    std::string tracker = g_ui.cfg.TrackerType;
    bool reachable = g_ui.cfg.BackendHasBeenReachable;
    MobilePage page = g_ui.mobilePage;
    bool drawerOpen = g_ui.mobileDrawerOpen;
    std::string editingViewId = g_ui.editingViewId;
    ~ViewsDrawerStateGuard() {
        g_ui.mobileDrawerOpen = drawerOpen;
        g_ui.editingViewId = editingViewId;
        g_ui.mobilePage = page;
        g_ui.cfg.UiMode = uiMode;
        g_ui.cfg.TrackerType = tracker;
        g_ui.cfg.BackendHasBeenReachable = reachable;
    }
};

void RegisterFieldSetReseedDropsStale(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "ViewsFieldSelection", "ReseedDropsStaleAndRestoresViewFields");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        ViewsDrawerStateGuard guard;
        // Pin Mobile + bootstrap a non-empty workspace + open the drawer so drawMobileDrawerViews
        // renders and LoadBuffersFromView seeds selectedFieldSet from the active view.
        g_ui.cfg.UiMode = UiMode::Mobile;
        g_ui.cfg.TrackerType = "Plane"; // fresh backend key -> default workspace, non-null active view
        g_ui.cfg.BackendHasBeenReachable = true;
        g_ui.mobilePage = MobilePage::Views;
        g_ui.mobileDrawerOpen = true;

        const bool drawerLive = YieldUntil(ctx, [] { return WindowIsLive("##MobileDrawerPanel"); });
        IM_CHECK_NO_RET(drawerLive);
        if (!drawerLive) {
            return;
        }
        // Reach the editingViewId == activeView.Id steady state so selectedFieldSet holds exactly the
        // active view's Fields (the authoritative seed), not a mid-reseed intermediate.
        ctx->Yield();
        ctx->Yield();
        const std::unordered_set<std::string> seeded = g_ui.selectedFieldSet; // S0 == view.Fields

        // Pollute the authoritative set with a stale entry AND force a reseed by moving editingViewId
        // off the active view id (the exact condition LoadBuffersFromView guards on). The next drawer
        // frame must clear + refill selectedFieldSet from the view, dropping the stale marker.
        const std::string kMarker = "smatchet_stale_field_marker";
        g_ui.selectedFieldSet.insert(kMarker);
        g_ui.editingViewId = "smatchet_force_reseed_sentinel";

        const bool droppedStale =
            YieldUntil(ctx, [&] { return g_ui.selectedFieldSet.find(kMarker) == g_ui.selectedFieldSet.end(); });
        if (!droppedStale) {
            ctx->LogError("selectedFieldSet still holds the stale marker after a forced reseed — "
                          "LoadBuffersFromView did not clear + refill from the view's Fields");
            IM_CHECK(false);
        }
        // The reseed restored EXACTLY the view's authoritative Fields (not a subset/superset).
        IM_CHECK_NO_RET(g_ui.selectedFieldSet == seeded);
    };
}

} // namespace

extern "C" void SmatchetRegisterViewsFieldSelectionTests(ImGuiTestEngine* engine) {
    RegisterFieldSetReseedDropsStale(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
