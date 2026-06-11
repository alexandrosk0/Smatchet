// mobile_views_confirm_modal.test.cpp — bucket-E regression coverage for #1117.
//
// PURPOSE — drive the REAL Mobile-mode shell render path and prove the
// "Discard changes?" view-switch confirm modal renders at SHELL level even
// after the drawer that initiated the switch has closed. This is the exact
// regression #1117 fixed: pre-fix the modal was nested in the drawer body, so a
// dirty view-switch (which closes the drawer) latched viewsShowDiscardConfirm to
// a modal that never rendered — the switch became unresolvable. The fix moved the
// modal renderer (drawMobileViewsModals) to drawMobileShell, called every frame
// AFTER the shell window's End() and AFTER the drawer, so the popup renders at
// root scope regardless of drawer state.
//
// DRIVE SEAM — drawResolveUiMode (SmatchetMobileShellUi.cpp) pins Mobile when
// cfg.UiMode == UiMode::Mobile with NO viewport-width dependency, so setting that
// flag forces drawMobileShell under the test's window size. drawMobileViewsModals
// is called unconditionally each frame from drawMobileShell (line ~162).
//
// SEEDING SEAM — under the minimal UiTestScenario boot ViewState is empty, and
// drawMobileViewsModals early-returns when GetActiveView() is null. Views::
// ApplyTrackerFromConfig re-bootstraps a default workspace (always >=1 view with
// ActiveViewId set, via MakeDefaultViewWorkspaceForBackend) whenever the backend
// key CHANGES. Flipping cfg.TrackerType to a fresh key ("Plane") therefore forces
// a non-null active view deterministically. (This writes the default workspace to
// disk via SavePersistentViewsToDisk — a pure local write — so teardown restores
// the original TrackerType.)
//
// PROBE — the modal is opened by setting the same latch the drawer's
// requestActivate closure sets on a dirty switch (viewsShowDiscardConfirm +
// viewsPendingActivateId) while forcing mobileDrawerOpen = false (the drawer-closed
// state the #1117 guard produces). WindowIsLive("Discard changes?") being true with
// the drawer closed is the regression assertion: it can only be true if the
// shell-level renderer consumed the latch independent of the drawer.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiModeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

// g_ui — the shared bag of UI-thread state; ViewToggleCommands.cpp flips these
// same fields. We set them directly to avoid threading a command dispatch through
// the test (same pattern as data_dependent_windows_smoke.test.cpp).
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

// True once a top-level window with `title` exists and is Active (Begin()
// returned true this frame). FindWindowByName resolves by the full title buffer
// and returns nullptr on a miss without erroring the context.
bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// ============================================================================
// Mobile view-switch discard-confirm modal renders at shell level (#1117).
// ============================================================================
void RegisterMobileViewsConfirmModalRendersAtShellLevel(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "MobileViewsConfirmModal", "DiscardConfirm_RendersAtShellLevel_WithDrawerClosed");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // Save state we mutate so the test leaves g_ui as it found it.
        const UiMode origUiMode = g_ui.cfg.UiMode;
        const std::string origTracker = g_ui.cfg.TrackerType;
        const bool origReachable = g_ui.cfg.BackendHasBeenReachable;
        const MobilePage origPage = g_ui.mobilePage;

        // Pin Mobile mode (no viewport-width dependency) + flip the backend key to a
        // fresh value so ViewState re-bootstraps a non-null default active view.
        g_ui.cfg.UiMode = UiMode::Mobile;
        g_ui.cfg.TrackerType = "Plane";
        g_ui.cfg.BackendHasBeenReachable = true;
        g_ui.mobilePage = MobilePage::Views;

        const char* kShellId = "##MobileShell";
        ctx->SetRef(kShellId);
        const bool shellLive = YieldUntil(ctx, [&] { return WindowIsLive(kShellId); });
        IM_CHECK_NO_RET(shellLive);

        if (shellLive) {
            // Reproduce the drawer's dirty-switch latch: requestActivate sets
            // viewsShowDiscardConfirm + viewsPendingActivateId and closes the drawer
            // (the #1117 guard keeps it open only while the confirm is pending — here
            // we force the closed state to prove the shell-level renderer is what
            // surfaces the modal, not the drawer body).
            g_ui.viewsDirty = true;
            g_ui.viewsPendingActivateId = "smatchet_test_other_view";
            g_ui.viewsShowDiscardConfirm = true;
            g_ui.mobileDrawerOpen = false;

            // The regression assertion: the modal renders at shell level even with
            // the drawer closed. Pre-#1117 this window never materialised.
            const char* kModalTitle = "Discard changes?";
            const bool modalLive = YieldUntil(ctx, [&] { return WindowIsLive(kModalTitle); });
            IM_CHECK_NO_RET(modalLive);

            // The drawer stayed closed throughout — the modal is NOT drawer-nested.
            IM_CHECK_NO_RET(!g_ui.mobileDrawerOpen);

            // Teardown: dismiss via Cancel if it resolved, else clear the latch by hand.
            if (modalLive && ctx->ItemExists("Cancel")) {
                ctx->ItemClick("Cancel");
                ctx->Yield();
            }
        }

        // Restore every mutated field. Clearing the latch + the confirm flag and
        // flipping UiMode back stops drawMobileViewsModals, which auto-closes any
        // still-open popup on the next frame.
        g_ui.viewsShowDiscardConfirm = false;
        g_ui.viewsPendingActivateId.clear();
        g_ui.viewsDirty = false;
        g_ui.mobileDrawerOpen = false;
        g_ui.mobilePage = origPage;
        g_ui.cfg.UiMode = origUiMode;
        g_ui.cfg.TrackerType = origTracker;
        g_ui.cfg.BackendHasBeenReachable = origReachable;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterMobileViewsConfirmModalTests(ImGuiTestEngine* engine) {
    RegisterMobileViewsConfirmModalRendersAtShellLevel(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
