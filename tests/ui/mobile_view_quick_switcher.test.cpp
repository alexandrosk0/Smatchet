// mobile_view_quick_switcher.test.cpp — bucket-E regression coverage for the mobile touch view
// quick-switcher band (drawMobileViewQuickSwitcher, SmatchetViewsDashboardUi.cpp), which P1.2
// (#1549) shipped under a tests-out-of-band Test-delta dismissal with only on-emulator screenshot
// verification. Closes the deferred test obligation (categories/test/2026-06-23-mobile-view-switcher
// -bucket-e.md) now that the bucket-E harness runs locally.
//
// DRIVE SEAM (same as mobile_views_confirm_modal.test.cpp): drawResolveUiMode pins Mobile when
// cfg.UiMode == UiMode::Mobile with no viewport-width dependency, so setting that flag forces
// drawMobileShell. The band is rendered by drawMobileShell only on the Grid mobile page
// (gridPage == d.mobilePage == MobilePage::Grid), so mobilePage selects band presence.
//
// SEEDING SEAM: under the minimal UiTestScenario boot ViewState is empty; flipping cfg.TrackerType
// to a fresh key ("Plane") re-bootstraps a default workspace (one "Default Plane View", ActiveViewId
// set) via ApplyTrackerFromConfig -> MakeDefaultViewWorkspaceForBackend. A second view is created by
// clicking the band's own "+##MobileNewView" button (viewsCreateNewView), which makes the first view
// non-active and tappable.
//
// PROBES: item presence via ctx->ItemExists (band renders the "+" button + a Button per view), and
// the g_ui.viewsShowDiscardConfirm field. Cases:
//   (3) band renders on Grid, absent on a non-Grid page (the gridPage gate).
//   (1) a CLEAN tab tap (viewsDirty == false) switches without latching the discard-confirm modal.
// The backlog's case (2) — a DIRTY tab tap latching the shell-level discard modal — is NOT re-driven
// here: viewsDirty is recomputed to false each frame by the grid draw path unless a real edit is
// pending, so a synthetic flag can't reach the band's requestActivate; the dirty -> modal
// end-behavior is already covered deterministically by mobile_views_confirm_modal.test.cpp (#1117).

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

// g_ui — the shared UI-thread state bag; the band's closures read/write these same fields.
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

// RAII-style restore of the g_ui fields the tests mutate, so the run leaves state as it found it.
struct SwitcherStateGuard {
    UiMode uiMode = g_ui.cfg.UiMode;
    std::string tracker = g_ui.cfg.TrackerType;
    bool reachable = g_ui.cfg.BackendHasBeenReachable;
    MobilePage page = g_ui.mobilePage;
    ~SwitcherStateGuard() {
        g_ui.viewsShowDiscardConfirm = false;
        g_ui.viewsPendingActivateId.clear();
        g_ui.viewsDirty = false;
        g_ui.mobileDrawerOpen = false;
        g_ui.mobilePage = page;
        g_ui.cfg.UiMode = uiMode;
        g_ui.cfg.TrackerType = tracker;
        g_ui.cfg.BackendHasBeenReachable = reachable;
    }
};

// Pin Mobile + Grid and re-bootstrap a default workspace; returns true once the shell is live.
bool EnterMobileGridShell(ImGuiTestContext* ctx) {
    g_ui.cfg.UiMode = UiMode::Mobile;
    g_ui.cfg.TrackerType = "Plane"; // fresh backend key -> re-bootstrap a non-empty workspace
    g_ui.cfg.BackendHasBeenReachable = true;
    g_ui.mobilePage = MobilePage::Grid;
    g_ui.viewsDirty = false;
    g_ui.viewsShowDiscardConfirm = false;
    ctx->SetRef("##MobileShell");
    return YieldUntil(ctx, [&] {
        const ImGuiWindow* w = ImGui::FindWindowByName("##MobileShell");
        return w != nullptr && w->Active;
    });
}

// ============================================================================
// (3) The band renders on the Grid page and is absent on a non-Grid page.
// ============================================================================
void RegisterBandGatedToGridPage(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "MobileViewQuickSwitcher", "Band_GatedToGridPage");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        SwitcherStateGuard guard;
        if (!EnterMobileGridShell(ctx)) {
            IM_CHECK_NO_RET(false); // shell never went live
            return;
        }
        // On the Grid page the band renders its "+" new-view button.
        const bool bandOnGrid = YieldUntil(ctx, [&] { return ctx->ItemExists("**/+##MobileNewView"); });
        IM_CHECK_NO_RET(bandOnGrid);

        // Switch to a non-Grid page: the band (and its "+" button) must disappear.
        g_ui.mobilePage = MobilePage::Views;
        const bool bandGoneOffGrid = YieldUntil(ctx, [&] { return !ctx->ItemExists("**/+##MobileNewView"); });
        IM_CHECK_NO_RET(bandGoneOffGrid);
    };
}

// ============================================================================
// (1) Tapping a non-active tab drives a CLEAN switch (no discard prompt).
//
// NOTE on the dirty-switch case (backlog case 2): viewsRequestActivate latches
// viewsShowDiscardConfirm when d.viewsDirty is set, but viewsDirty is RECOMPUTED to
// false every frame by the grid draw path (SmatchetActiveProjectGridUi.cpp) unless a
// real edit is pending — a synthetic g_ui.viewsDirty = true cannot survive to the
// band's requestActivate. The dirty-switch -> shell-level discard modal end-behavior
// is already covered deterministically by mobile_views_confirm_modal.test.cpp (it sets
// the latch directly and asserts the modal renders), so it is not re-driven here.
// ============================================================================
void RegisterTabTapCleanSwitch(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "MobileViewQuickSwitcher", "TabTap_CleanSwitchDoesNotPrompt");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        SwitcherStateGuard guard;
        if (!EnterMobileGridShell(ctx)) {
            IM_CHECK_NO_RET(false);
            return;
        }
        // The bootstrap gives one "Default Plane View" (active). Create a second via the band's "+"
        // button; viewsCreateNewView activates the new one, so "Default Plane View" becomes tappable.
        if (!YieldUntil(ctx, [&] { return ctx->ItemExists("**/+##MobileNewView"); })) {
            IM_CHECK_NO_RET(false);
            return;
        }
        ctx->ItemClick("**/+##MobileNewView");
        const bool secondTabExists = YieldUntil(ctx, [&] { return ctx->ItemExists("**/Default Plane View"); });
        IM_CHECK_NO_RET(secondTabExists);
        if (!secondTabExists) {
            return;
        }

        // A CLEAN switch: not dirty -> tapping the non-active tab ACTIVATES it (viewsActivateView ->
        // LoadBuffersFromView copies the now-active view's Name into d.viewNameBuf) WITHOUT raising
        // the shell-level discard-confirm modal. Assert BOTH: the active view actually changed — a
        // silently-dropped / no-op tap would leave viewNameBuf on "New View" and pass the old
        // modal-only check vacuously (#1789) — AND no modal.
        g_ui.viewsDirty = false;
        g_ui.viewsShowDiscardConfirm = false;
        const std::string beforeName(g_ui.viewNameBuf);      // "New View" — the just-created active view
        IM_CHECK_NO_RET(beforeName != "Default Plane View"); // precondition: we start on the other view
        ctx->ItemClick("**/Default Plane View");
        const bool switched = YieldUntil(ctx, [] { return std::string(g_ui.viewNameBuf) == "Default Plane View"; });
        if (!switched) {
            ctx->LogError("Clean tab-tap did not switch the active view — viewNameBuf stayed '%s' (the "
                          "tap was silently dropped or the band's clean-switch path no longer activates)",
                          g_ui.viewNameBuf);
            IM_CHECK(false);
        }
        IM_CHECK_NO_RET(!g_ui.viewsShowDiscardConfirm);
    };
}

} // namespace

extern "C" void SmatchetRegisterMobileViewQuickSwitcherTests(ImGuiTestEngine* engine) {
    RegisterBandGatedToGridPage(engine);
    RegisterTabTapCleanSwitch(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
