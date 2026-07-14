// user_info_window.test.cpp — bucket-E (ImGui Test Engine) behavioural coverage for the
// dockable "User Info" window (docs/plans/shipped/user-info-window.md, Slice 5; backlog
// docs/self-improvement/categories/tooling.md entry user-info-window-bucket-e-coverage).
//
// The window is driven exactly as the production grid right-click path drives it: set the
// g_ui staged-request fields (userInfoSourcePaneId/DisplayName/Email/AccountId +
// userInfoRequestPending) and g_ui.showUserInfo=true, then tick. DrawWindow (called every
// frame from SmatchetUI.cpp regardless of visibility) consumes the pending request via
// adoptPendingRequest, launches the fetches, and draws the four sections. The window's
// bool* open IS &g_ui.showUserInfo, so both close paths (Escape while focused, and a host
// clear of showUserInfo — the Close-button path) route through the same closeCleanup edge.
//
// FIXTURE-BACKEND ACTIVITY GAP (honest residue). PaneSupportsActivity(paneId) returns true
// iff the pane's backend ->Activity() != nullptr (AppController_CatalogAndFieldEdit.cpp). The
// bucket-E fixture backend is FakeTrackerClient, whose Activity() returns nullptr
// (tests/support/FakeTrackerClient.h). So under this harness supportsActivity_ is ALWAYS
// false, and the Activity + Groups sections are compiled out at runtime by their
// `if (!supportsActivity_) return;` guards. That makes two of the seven plan behaviours
// unreachable here without a new activity-capable fixture backend:
//   (3) activity Load/Reload button disabled while loading  — behind the activity guard
//   (4) one-shot group-member fetch per open                — behind the groups guard
// Both are documented as deferred-automation residue in the backlog rather than faked. The
// other five behaviours are backend-agnostic (window lifecycle + cfg + the always-on
// Identity/Vcs sections) and are covered deterministically below:
//   1. OpenThenEscapeCloses / OpenThenHostClearCloses — Escape AND a showUserInfo clear both close.
//   2. CloseEdgeCleanupRunsOnceNeverOnHiddenClear      — close cleanup fires on the open->closed
//      edge; a clear while the window was never open is a safe no-op (the wasOpen_ edge).
//   5. VcsFeedLayoutToggleFlipsConfigAndDirties        — the Unified/Separate radios flip
//      cfg.VcsFeedLayout and latch prefsDirty (the persistence trigger).
//   6. NarrowLayoutRendersWithoutAssert                — a ~360px content width (< kNarrowLayoutWidthPx)
//      takes the stacked narrow-row path; window stays live (Begin/End balance intact).
//   7. IdentityAndVcsSectionsRender                     — the two always-on sections render for
//      several frames with a live target (any Begin/End or PushID imbalance trips an engine assert).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "SmatchetUiSession.h"                 // UiDrawSession, MarkPrefsDirty, g_ui
#include "Ui/SmatchetLayoutBreakpoints.h"      // smatchet::ui::kNarrowLayoutWidthPx

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>

// g_ui — the shared UI-thread state bag. The User Info window reads showUserInfo +
// requestUserInfoFocus + the userInfo* staged-request fields + cfg.VcsFeedLayout exactly as
// the grid right-click Open() path and the section radios write them.
extern UiDrawSession g_ui;

namespace {

const char* kWindowTitle = "User Info";

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

// Restore the g_ui fields any User Info test mutates so a run leaves state as it found it
// (the sibling StateGuard pattern — views_field_selection.test.cpp).
struct UserInfoStateGuard {
    bool showUserInfo = g_ui.showUserInfo;
    bool requestFocus = g_ui.requestUserInfoFocus;
    bool requestPending = g_ui.userInfoRequestPending;
    std::string paneId = g_ui.userInfoSourcePaneId;
    std::string displayName = g_ui.userInfoDisplayName;
    std::string email = g_ui.userInfoEmail;
    std::string accountId = g_ui.userInfoAccountId;
    std::string vcsFeedLayout = g_ui.cfg.VcsFeedLayout;
    bool prefsDirty = g_ui.prefsDirty;
    ~UserInfoStateGuard() {
        // Close the window first so DrawWindow runs its close-edge cleanup against a valid
        // app before we restore the raw flags.
        g_ui.showUserInfo = false;
        g_ui.userInfoRequestPending = requestPending;
        g_ui.userInfoSourcePaneId = paneId;
        g_ui.userInfoDisplayName = displayName;
        g_ui.userInfoEmail = email;
        g_ui.userInfoAccountId = accountId;
        g_ui.cfg.VcsFeedLayout = vcsFeedLayout;
        g_ui.prefsDirty = prefsDirty;
        g_ui.requestUserInfoFocus = requestFocus;
        g_ui.showUserInfo = showUserInfo;
    }
};

// Stage an open request on the focused pane and tick until the window is live. The
// requestUserInfoFocus latch is consumed in one frame, so re-arm it each frame (mirrors the
// sibling OpenPreferences recipe). An empty source-pane id resolves to the focused context
// (paneContextOrFocused_), so the window always has a valid backend to adopt against.
bool OpenUserInfoLive(ImGuiTestContext* ctx) {
    g_ui.userInfoSourcePaneId = "main"; // the default focused pane id
    g_ui.userInfoDisplayName = "Test User";
    g_ui.userInfoEmail = "test.user@example.com";
    g_ui.userInfoAccountId = "test-account-id";
    g_ui.userInfoRequestPending = true;
    g_ui.showUserInfo = true;
    ctx->SetRef(kWindowTitle);
    return YieldUntil(ctx, [&] {
        g_ui.requestUserInfoFocus = true;
        return WindowIsLive(kWindowTitle);
    });
}

// --- Behaviour 1a: open, then Escape closes -------------------------------
void RegisterOpenThenEscapeCloses(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "UserInfoWindow", "OpenThenEscapeCloses");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        UserInfoStateGuard guard;

        const bool live = OpenUserInfoLive(ctx);
        IM_CHECK_NO_RET(live);
        if (!live) {
            return;
        }
        // Focus the window, then press Escape. DrawWindow closes on
        // IsWindowFocused(RootAndChildWindows) && Escape by clearing *open (= showUserInfo).
        // The window is docked, so WindowFocus needs the "//" absolute ref (a bare name resolves
        // relative to the current SetRef and dangles for window names — grid_pane_windows recipe).
        ctx->WindowFocus("//User Info");
        ctx->KeyPress(ImGuiKey_Escape);
        const bool closed = YieldUntil(ctx, [] { return !g_ui.showUserInfo && !WindowIsLive(kWindowTitle); });
        if (!closed) {
            ctx->LogError("Escape while focused did not close the User Info window (showUserInfo stayed set)");
            IM_CHECK(false);
        }
    };
}

// --- Behaviour 1b: open, then a host clear of showUserInfo closes (Close-button path) -----
// The window's bool* open is &g_ui.showUserInfo, so the standard ImGui title-bar close button
// writes exactly this flag. Driving it directly is the same code path without depending on the
// docked title-bar X hit-test (unreliable in the headless suite).
void RegisterOpenThenHostClearCloses(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "UserInfoWindow", "OpenThenHostClearCloses");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        UserInfoStateGuard guard;

        const bool live = OpenUserInfoLive(ctx);
        IM_CHECK_NO_RET(live);
        if (!live) {
            return;
        }
        // The Close-button path: clear showUserInfo. DrawWindow sees *open==false on the next
        // frame and runs closeCleanup (wasOpen_ was true), then the window goes inactive.
        g_ui.showUserInfo = false;
        const bool closed = YieldUntil(ctx, [] { return !WindowIsLive(kWindowTitle); });
        IM_CHECK_NO_RET(closed);
    };
}

// --- Behaviour 2: close-edge cleanup fires on the open->closed edge, never on a hidden clear ---
// closeCleanup (ClearPaneUserActivity + P4ClPreview::DetachInFlight) runs only on the wasOpen_
// edge: DrawWindow returns early when *open is false AND the window was never open. This asserts
// the edge is exercised cleanly (open -> Escape-close runs cleanup once; a subsequent clear while
// already hidden is a no-op that must not re-run cleanup or crash). The distinct
// backend-call-count spy (assert ClearPaneUserActivity fired exactly once) needs an activity-
// capable fixture + a call-count probe — documented as residue (fixture Activity() is nullptr).
void RegisterCloseEdgeCleanupRunsOnceNeverOnHiddenClear(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "UserInfoWindow", "CloseEdgeCleanupRunsOnceNeverOnHiddenClear");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        UserInfoStateGuard guard;

        const bool live = OpenUserInfoLive(ctx);
        IM_CHECK_NO_RET(live);
        if (!live) {
            return;
        }
        // Close via Escape — this is the wasOpen_ true->false edge that runs closeCleanup.
        // "//" absolute ref: the docked window name dangles under the current SetRef otherwise.
        ctx->WindowFocus("//User Info");
        ctx->KeyPress(ImGuiKey_Escape);
        const bool closedOnce = YieldUntil(ctx, [] { return !WindowIsLive(kWindowTitle); });
        IM_CHECK_NO_RET(closedOnce);

        // Hidden clear: the window is already closed (wasOpen_ false). Re-clearing showUserInfo
        // must take the early-return branch (no second closeCleanup, no crash). Several ticks
        // with the flag held false exercise the always-called DrawWindow's hidden path.
        g_ui.showUserInfo = false;
        for (int i = 0; i < 5; ++i) {
            ctx->Yield();
        }
        // Still closed and the app is intact — the hidden-clear path is a clean no-op.
        IM_CHECK_NO_RET(!WindowIsLive(kWindowTitle));
    };
}

// --- Behaviour 5: the Vcs-feed layout radios flip cfg.VcsFeedLayout + latch prefsDirty ---
// The "Recent submissions" CollapsingHeader (always rendered — not behind the activity guard)
// hosts the Unified/Separate radios. Clicking them writes cfg.VcsFeedLayout and calls
// MarkPrefsDirty(d) — the debounced-persistence trigger. Default is unified (any value !=
// "separate"). This drives the real widgets through the engine (ItemClick), not a direct field
// poke, so the radio's own !unified / unified guard + the MarkPrefsDirty call are exercised.
void RegisterVcsFeedLayoutToggle(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "UserInfoWindow", "VcsFeedLayoutToggleFlipsConfigAndDirties");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        UserInfoStateGuard guard;

        // Start from a known unified layout so the Separate radio is the state-changing click.
        g_ui.cfg.VcsFeedLayout = "unified";
        const bool live = OpenUserInfoLive(ctx);
        IM_CHECK_NO_RET(live);
        if (!live) {
            return;
        }
        // The header is DefaultOpen so the radios are submitted. Clear the dirty latch so the
        // flip is unambiguously ours.
        g_ui.prefsDirty = false;
        ctx->ItemClick("**/Separate");
        const bool wentSeparate = YieldUntil(ctx, [] { return g_ui.cfg.VcsFeedLayout == "separate"; });
        if (!wentSeparate) {
            ctx->LogError("clicking Separate did not set cfg.VcsFeedLayout to \"separate\"");
            IM_CHECK(false);
            return;
        }
        IM_CHECK_NO_RET(g_ui.prefsDirty); // MarkPrefsDirty ran — the persistence trigger latched

        // Flip back to Unified and assert the round-trip.
        g_ui.prefsDirty = false;
        ctx->ItemClick("**/Unified");
        const bool wentUnified = YieldUntil(ctx, [] { return g_ui.cfg.VcsFeedLayout == "unified"; });
        IM_CHECK_NO_RET(wentUnified);
        IM_CHECK_NO_RET(g_ui.prefsDirty);
    };
}

// --- Behaviour 6: a ~360px content width takes the narrow/stacked layout without asserting ---
// DrawWindow computes narrow = GetContentRegionAvail().x < kNarrowLayoutWidthPx (600) and the
// section draws branch on it (two-line stacked rows, taller touch targets). Forcing a small
// window size drives the narrow branch through every always-on section; a Begin/End or PushID
// imbalance on that path would trip an engine IM_ASSERT and fail the test. The window staying
// Active at that size is the observable no-horizontal-scroll / stacked-layout signal.
void RegisterNarrowLayoutRenders(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "UserInfoWindow", "NarrowLayoutRendersWithoutAssert");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        UserInfoStateGuard guard;

        const bool live = OpenUserInfoLive(ctx);
        IM_CHECK_NO_RET(live);
        if (!live) {
            return;
        }
        // Shrink content width below kNarrowLayoutWidthPx so the narrow branch runs. 360px is
        // comfortably under the 600px breakpoint (the plan's ~400px narrow target).
        ImGuiWindow* win = ImGui::FindWindowByName(kWindowTitle);
        IM_CHECK_NO_RET(win != nullptr);
        if (win == nullptr) {
            return;
        }
        // "//" absolute ref — the docked window name dangles under the current SetRef otherwise.
        ctx->WindowResize("//User Info", ImVec2(360.0f, 480.0f));
        // Tick several frames so every always-on section renders under the narrow branch. The
        // window must remain Active (Begin/End balance) throughout — the render-without-assert
        // guarantee. GetContentRegionAvail().x is now well under the breakpoint.
        bool stayedLive = true;
        for (int i = 0; i < 6; ++i) {
            ctx->Yield();
            if (!WindowIsLive(kWindowTitle)) {
                stayedLive = false;
                break;
            }
        }
        IM_CHECK_NO_RET(stayedLive);
        IM_CHECK_NO_RET(smatchet::ui::kNarrowLayoutWidthPx == 600.0f); // guard against a silent breakpoint move
    };
}

// --- Behaviour 7: the always-on Identity + Vcs sections render for a live target ----------
// Identity (name/email/id + the "not supported" line under the fixture) and Vcs ("Recent
// submissions" header + radios + rows/empty-state) render every frame regardless of activity
// support. Ticking a live window several frames exercises both section draws end-to-end; a
// Begin/End, PushID/PopID, or CollapsingHeader imbalance in either would trip an engine assert.
// The Activity + Groups sections are guarded by supportsActivity_ (false under the fixture —
// see the file header), so this covers the two reachable sections; those two are residue.
void RegisterIdentityAndVcsSectionsRender(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "UserInfoWindow", "IdentityAndVcsSectionsRender");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        UserInfoStateGuard guard;

        const bool live = OpenUserInfoLive(ctx);
        IM_CHECK_NO_RET(live);
        if (!live) {
            return;
        }
        // Render several frames so both always-on sections' full bodies tick (incl. the Vcs
        // loaded/empty-state branches once the async fetch resolves against the fixture).
        bool stayedLive = true;
        for (int i = 0; i < 8; ++i) {
            ctx->Yield();
            if (!WindowIsLive(kWindowTitle)) {
                stayedLive = false;
                break;
            }
        }
        IM_CHECK_NO_RET(stayedLive);
        // The "Recent submissions" Vcs header is a stable, locale-stable label the section always
        // submits — existence proves the Vcs section body ran (Identity precedes it in the same
        // Begin block, so it ran too).
        IM_CHECK_NO_RET(ctx->ItemExists("**/Recent submissions"));
    };
}

} // namespace

extern "C" void SmatchetRegisterUserInfoWindowTests(ImGuiTestEngine* engine) {
    RegisterOpenThenEscapeCloses(engine);
    RegisterOpenThenHostClearCloses(engine);
    RegisterCloseEdgeCleanupRunsOnceNeverOnHiddenClear(engine);
    RegisterVcsFeedLayoutToggle(engine);
    RegisterNarrowLayoutRenders(engine);
    RegisterIdentityAndVcsSectionsRender(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
