// views_field_selection.test.cpp — bucket-E regression coverage for the Views-editor field-selection
// reseed lifecycle (the #views-field-uncheck bug class). column-view-save-simplification:
// UiDrawSession::selectedFieldSet is gone — the authoritative column/field set is now
// UiDrawSession::viewDraft.Columns, reseeded straight from the active view by LoadBuffersFromView
// whenever the editing view changes (SmatchetViewsDashboardUi.cpp), keyed on viewDraftId (was
// editingViewId) — never re-derived from a truncating CSV buffer. The bug this guards: a
// stale/leaked entry surviving a view-switch (a checkbox staying checked when the newly-loaded
// view does not own that field). We drive the reseed and assert it CLEARS a polluted entry and
// restores EXACTLY the view's Columns.
//
// RENDER VEHICLE: UiMode::Mobile pins drawMobileShell; mobileDrawerOpen=true renders
// drawMobileDrawer -> drawMobileDrawerViews (SmatchetMobileShellUi.cpp), which calls
// LoadBuffersFromView when d.viewDraftId != activeView->Id — the same reseed guard the desktop
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

// Field ids (the "field:<id>" part, "id" excluded) currently on the draft's Columns — the
// v3-shape equivalent of the old selectedFieldSet.
std::unordered_set<std::string> DraftFieldIds() {
    std::unordered_set<std::string> ids;
    for (const auto& col : g_ui.viewDraft.Columns) {
        if (col.Key.compare(0, 6, "field:") == 0) {
            ids.insert(col.Key.substr(6));
        }
    }
    return ids;
}

// Restore the g_ui fields the test mutates so the run leaves state as it found it. Must
// restore viewDraft itself, not just viewDraftId: on a failed run (assertion abort, or the
// ImGui Test Engine's verbose re-run of a just-failed test in the same process), leaving the
// polluted g_ui.viewDraft.Columns behind while viewDraftId is restored to an id that may
// already match the active view lets a LATER pass skip the id-mismatch reload guard entirely
// and inherit the leaked stale-marker column as its own "seeded" baseline.
struct ViewsDrawerStateGuard {
    UiMode uiMode = g_ui.cfg.UiMode;
    std::string tracker = g_ui.cfg.TrackerType;
    bool reachable = g_ui.cfg.BackendHasBeenReachable;
    MobilePage page = g_ui.mobilePage;
    bool drawerOpen = g_ui.mobileDrawerOpen;
    std::string viewDraftId = g_ui.viewDraftId;
    ViewDefinition viewDraft = g_ui.viewDraft;
    ~ViewsDrawerStateGuard() {
        g_ui.mobileDrawerOpen = drawerOpen;
        g_ui.viewDraftId = viewDraftId;
        g_ui.viewDraft = viewDraft;
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
        // Reach the viewDraftId == activeView.Id steady state so the draft's Columns hold exactly
        // the active view's fields (the authoritative seed), not a mid-reseed intermediate.
        ctx->Yield();
        ctx->Yield();
        const std::unordered_set<std::string> seeded = DraftFieldIds(); // S0 == view.Fields

        // Pollute the draft with a stale field column AND force a reseed by moving viewDraftId off
        // the active view id (the exact condition LoadBuffersFromView guards on). The next drawer
        // frame must reload the draft from the view, dropping the stale marker.
        const std::string kMarker = "smatchet_stale_field_marker";
        g_ui.viewDraft.Columns.push_back({"field:" + kMarker, 0.0f});
        g_ui.viewDraftId = "smatchet_force_reseed_sentinel";

        // Re-pin mobileDrawerOpen/mobilePage every iteration rather than a plain YieldUntil: the
        // reload this loop waits for only runs from drawMobileDrawerViews, which is gated on both
        // — and ImGui Test Engine's simulated mouse cursor persists across tests in the same
        // process, so a sibling "Plane"-backend test's trailing ItemClick (e.g.
        // mobile_view_quick_switcher.test.cpp) can leave it positioned over the drawer's full-
        // screen scrim, closing the drawer via the scrim's click-outside handler on the very next
        // frame with no click of our own — silently starving drawMobileDrawerViews for the rest of
        // the wait and making the reload this test exercises look like it never ran.
        bool droppedStale = false;
        for (int i = 0; i < 300 && !droppedStale; ++i) {
            g_ui.mobileDrawerOpen = true;
            g_ui.mobilePage = MobilePage::Views;
            ctx->Yield();
            droppedStale = DraftFieldIds().find(kMarker) == DraftFieldIds().end();
        }
        if (!droppedStale) {
            ctx->LogError("viewDraft.Columns still holds the stale marker after a forced reseed — "
                          "LoadBuffersFromView did not reload the draft from the view");
            IM_CHECK(false);
        }
        // The reseed restored EXACTLY the view's authoritative field set (not a subset/superset).
        IM_CHECK_NO_RET(DraftFieldIds() == seeded);
    };
}

} // namespace

extern "C" void SmatchetRegisterViewsFieldSelectionTests(ImGuiTestEngine* engine) {
    RegisterFieldSetReseedDropsStale(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
