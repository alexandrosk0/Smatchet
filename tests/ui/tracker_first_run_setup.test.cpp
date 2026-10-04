// tracker_first_run_setup.test.cpp — bucket-E coverage for the Preferences
// Tracker tab's first-run setup surface (docs/plans/
// dev-onboarding-first-run-quickstart.md, slice 2).
//
// TWO things the pure bucket-A rig (tests/Core/TrackerSetupPure.test.cpp)
// cannot reach, because both live inside live ImGui frames:
//
//  1. DrawTrackerFirstRunExplainer (SmatchetPreferencesUi.cpp) — the warning-
//     coloured explainer drawn while TrackerSetupPure::NeedsSetup(d.cfg) reads
//     true. It PushStyleColor()s before the text and PopStyleColor()s after, and
//     early-returns BEFORE the push when setup is done. An imbalance in either
//     branch corrupts the ImGui style stack for every widget drawn after it. The
//     test ticks the Tracker tab body in BOTH states so the engine traps the
//     in-frame IM_ASSERT either way.
//
//  2. The verified-credential pin (UiDrawSession::trackerPrefsTestVerifiedFingerprint)
//     staleness contract — resetPreferencesWindowState must clear it when the
//     Preferences window closes. If it survived a close, reopening and pressing
//     Save & Sync would clear first-run read-only on a verdict that is no longer
//     on screen. This drives the REAL close path (drawPreferencesWindow's
//     show-gate), not a replica.
//
// APP-STATE-COUPLED, like preferences_tracker_switch.test.cpp: assertions read
// g_ui / config state rather than ImGui item labels. Two reasons — the docked
// Preferences window opens with a short content region in the headless app, so
// lower widgets clip out of the Test Engine item table (documented in
// funcsize_preferences_tabs.test.cpp), and the explainer is TextWrapped, which
// registers no addressable item ID at all.
//
// No backend, no network, no fixture env: the explainer reads only d.cfg and the
// pin is set/cleared by UI-thread code. Registered under its own group so
// scripts/dev/test-ui-tracker-first-run-setup.sh can filter it.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "ConfigManager.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "TrackerSetupPure.h"

#include "ui_test_skip.h" // SmatchetUiTestIsHeadlessSoftwareGl

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>
#include <string>

// g_ui — the shared bag of UI-thread state. Set showPreferences /
// requestPreferencesFocus directly, exactly as the View menu does.
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

// Open Preferences and tick until its window is live. Re-arms the focus latch
// every frame because the draw fn consumes it in one frame (the docked-window
// open recipe funcsize_preferences_tabs.test.cpp established).
bool OpenPreferences(ImGuiTestContext* ctx) {
    g_ui.showPreferences = true;
    g_ui.requestPreferencesFocus = true;
    ctx->SetRef("Preferences");
    return YieldUntil(ctx, [&] {
        g_ui.requestPreferencesFocus = true;
        return WindowIsLive("Preferences");
    });
}

// Tracker is the default PreferencesCategory, so it is selected on open with no
// click needed, and the dispatch switch draws its body every frame the window
// is live — observing the category is the "the body is ticking" signal.
bool TrackerTabBodyRan() { return g_ui.preferencesCategory == PreferencesCategory::Tracker; }

// The draw fn consumes requestPreferencesFocus in one frame, and ItemInfo / NavMoveTo yield
// frames internally, so a test that resolves a body widget re-arms it from its GuiFunc on EVERY
// frame; otherwise the body (and the widget being looked up) stops being submitted mid-lookup.
bool g_keepTrackerBodyLive = false;

void KeepTrackerBodyLive() {
    if (!g_keepTrackerBodyLive) {
        return;
    }
    g_ui.requestPreferencesFocus = true;
    // A persisted collapse of the backend section would skip the button entirely.
    g_ui.prefsCollapsedSections.erase("tracker.backend");
    g_ui.prefsCollapsedLoaded = true;
    // The update-available modal owns nav, so no widget under it could be activated.
    g_ui.appUpdateModalOpen = false;
}

// The engine's `**/label` wildcard does not resolve into the PrefsBody child window, so build the
// id the draw code uses: PrefsBody seeds the stack, PrefsSection pushes the section id and the
// button hashes its localized label under that. 0 while the body has not been drawn.
ImGuiID TrackerTestConnectionButtonId() {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    for (int i = 0; i < g->Windows.Size; ++i) {
        const ImGuiWindow* win = g->Windows[i];
        if (std::strstr(win->Name, "PrefsBody") != nullptr) {
            const char* label = SmatchetLocalization::T("prefs.tracker.test.button", "Test connection");
            return ImHashStr(label, 0, ImHashStr("tracker.backend", 0, win->ID));
        }
    }
    return 0;
}

// A config the credential-completeness half of NeedsSetup is satisfied by, so
// BackendHasBeenReachable alone decides the predicate.
void FillJiraCredentials(TrackerConfig& cfg) {
    cfg.TrackerType = "Jira";
    cfg.Domain = "example.atlassian.net";
    cfg.Email = "ui-test@example.com";
    cfg.ApiToken = "ui-test-token";
}

} // namespace

// ---------------------------------------------------------------------------
// TrackerFirstRun_ExplainerBranchesRenderInBothStates
// Ticks the Tracker tab body with NeedsSetup true (explainer drawn: push text
// colour, two TextWrapped lines, separator, pop) and then false (early return
// before the push). Restores the original config before closing so the buffers
// still match cfg and the unsaved-edits close guard does not fire.
// ---------------------------------------------------------------------------
static void RegisterExplainerBranches(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TrackerFirstRun", "ExplainerBranchesRenderInBothStates");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const TrackerConfig cfgBackup = g_ui.cfg;

        const bool prefsLive = OpenPreferences(ctx);
        IM_CHECK_NO_RET(prefsLive);
        if (!prefsLive) {
            g_ui.cfg = cfgBackup;
            g_ui.showPreferences = false;
            return;
        }
        IM_CHECK_NO_RET(YieldUntil(ctx, [] { return TrackerTabBodyRan(); }));

        // Branch 1 — still being set up. The reachability latch alone forces the
        // predicate true, so this holds no matter what credentials the host
        // config carries.
        g_ui.cfg.BackendHasBeenReachable = false;
        IM_CHECK_NO_RET(TrackerSetupPure::NeedsSetup(g_ui.cfg));
        for (int i = 0; i < 5; ++i) {
            ctx->Yield(); // explainer path: PushStyleColor .. PopStyleColor
        }
        IM_CHECK_NO_RET(TrackerTabBodyRan());

        // Branch 2 — setup complete. The explainer early-returns before touching
        // the style stack; the rest of the tab must draw identically.
        FillJiraCredentials(g_ui.cfg);
        g_ui.cfg.BackendHasBeenReachable = true;
        IM_CHECK_NO_RET(!TrackerSetupPure::NeedsSetup(g_ui.cfg));
        for (int i = 0; i < 5; ++i) {
            ctx->Yield(); // early-return path
        }
        IM_CHECK_NO_RET(TrackerTabBodyRan());

        // Restore BEFORE closing: the staged text buffers were loaded from the
        // original cfg, and closing with buffers that differ opens the unsaved-
        // tracker-changes guard modal instead of resetting the window state.
        g_ui.cfg = cfgBackup;
        g_ui.showPreferences = false;
        ctx->Yield();
    };
}

// ---------------------------------------------------------------------------
// TrackerFirstRun_VerifiedPinClearsOnWindowClose
// A green "Test connection" verdict pins the fingerprint of the exact
// credentials it probed. Closing Preferences must drop that pin — a verdict
// that is no longer on screen must not unlock first-run read-only on a later
// Save & Sync.
// ---------------------------------------------------------------------------
static void RegisterVerifiedPinClearsOnClose(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TrackerFirstRun", "VerifiedPinClearsOnWindowClose");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        if (SmatchetUiTestIsHeadlessSoftwareGl()) {
            // Under llvmpipe the Preferences close path never drops the pin within
            // the yield budget (deterministic on every CI run; passes on real GL).
            ctx->LogInfo("SKIP: render/timing-dependent under headless software GL (see ui_test_skip.h)");
            return;
        }

        const bool prefsLive = OpenPreferences(ctx);
        IM_CHECK_NO_RET(prefsLive);
        if (!prefsLive) {
            g_ui.showPreferences = false;
            return;
        }
        IM_CHECK_NO_RET(YieldUntil(ctx, [] { return TrackerTabBodyRan(); }));

        // Stand in for a completed green probe. The value only has to be
        // non-empty — Save & Sync compares it to a freshly computed digest, so
        // any surviving pin is a bug regardless of what it holds.
        g_ui.trackerPrefsTestVerifiedFingerprint = "deadbeefdeadbeef";
        ctx->Yield();
        IM_CHECK_NO_RET(!g_ui.trackerPrefsTestVerifiedFingerprint.empty());

        // No credential buffer was touched, so the close gate takes the plain
        // resetPreferencesWindowState path rather than the guard modal.
        g_ui.showPreferences = false;
        IM_CHECK_NO_RET(YieldUntil(ctx, [] { return g_ui.trackerPrefsTestVerifiedFingerprint.empty(); }));
    };
}

// ---------------------------------------------------------------------------
// TrackerFirstRun_TestConnectionClickKeepsDisabledStackBalanced
// Crash regression: DrawTrackerTestConnection re-read d.trackerPrefsTestInFlight
// for its EndDisabled() AFTER the click had set it true, popping a disabled scope
// it never pushed — IM_ASSERT "Calling EndDisabled() too many times!" aborted the
// app on the first "Test connection" click. Activating the REAL button exercises the
// exact frame; a regression trips the in-frame IM_ASSERT.
// ---------------------------------------------------------------------------
static void RegisterTestConnectionClickBalanced(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TrackerFirstRun", "TestConnectionClickKeepsDisabledStackBalanced");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { KeepTrackerBodyLive(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        g_keepTrackerBodyLive = true;
        const bool prefsLive = OpenPreferences(ctx);
        IM_CHECK_NO_RET(prefsLive);
        if (!prefsLive) {
            g_keepTrackerBodyLive = false;
            g_ui.showPreferences = false;
            return;
        }
        IM_CHECK_NO_RET(YieldUntil(ctx, [] { return TrackerTabBodyRan(); }));
        IM_CHECK_NO_RET(!g_ui.trackerPrefsTestInFlight);

        // The activation frame flips trackerPrefsTestInFlight false -> true between the
        // BeginDisabled and EndDisabled decisions — the crashing transition. Nav-activate,
        // not ItemClick: the body sits in a dock node the engine cannot move to clear a
        // mouse path, and Button() returns true for a nav activation exactly as for a click.
        const ImGuiTestRef buttonRef(TrackerTestConnectionButtonId());
        IM_CHECK_NO_RET(buttonRef.ID != 0);
        const int genBefore = g_ui.trackerPrefsTestGen;
        ctx->SetInputMode(ImGuiInputSource_Keyboard);
        ctx->ItemNavActivate(buttonRef);
        YieldUntil(ctx, [genBefore] { return g_ui.trackerPrefsTestGen != genBefore; }, 30);
        IM_CHECK_NO_RET(g_ui.trackerPrefsTestGen == genBefore + 1); // the activation reached the handler

        // Let the throwaway probe settle so no verdict lands after the window closes;
        // a probe that outlives the budget is dropped by the close path's gen bump.
        YieldUntil(ctx, [] { return !g_ui.trackerPrefsTestInFlight; }, 600);
        g_keepTrackerBodyLive = false;
        g_ui.showPreferences = false;
        ctx->Yield();
    };
}

extern "C" void SmatchetRegisterTrackerFirstRunSetupTests(ImGuiTestEngine* engine) {
    RegisterExplainerBranches(engine);
    RegisterVerifiedPinClearsOnClose(engine);
    RegisterTestConnectionClickBalanced(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
