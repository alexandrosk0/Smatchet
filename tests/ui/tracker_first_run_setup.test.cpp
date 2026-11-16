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
//  3. Keyboard entry into the tab (docs/audits/INPUT_UX_AUDIT.md #72, #73): an
//     asked-for open puts the keyboard in the first blank credential field (or the
//     settings search box once the tracker is set up), a merely restored window
//     takes nothing, and a bare Enter in a credential field runs Test connection.
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
#include "Ui/SmatchetSecretInput.h" // SmatchetTextInputCommittedWithEnter — the real Enter-commit signal

#include "ui_test_skip.h" // SmatchetUiTestIsHeadlessSoftwareGl

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdio>
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
// id the draw code uses: PrefsBody seeds the stack, PrefsSection pushes the section id and each
// widget hashes its label under that. 0 while the body has not been drawn.
ImGuiID TrackerBackendSectionItemId(const char* label) {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    for (int i = 0; i < g->Windows.Size; ++i) {
        const ImGuiWindow* win = g->Windows[i];
        if (std::strstr(win->Name, "PrefsBody") != nullptr) {
            return ImHashStr(label, 0, ImHashStr("tracker.backend", 0, win->ID));
        }
    }
    return 0;
}

ImGuiID TrackerTestConnectionButtonId() {
    return TrackerBackendSectionItemId(SmatchetLocalization::T("prefs.tracker.test.button", "Test connection"));
}

// A credential text row. `sourceLabel` is the English label the draw code passes; the localized
// InputText wrapper turns it into the submitted label the same way.
ImGuiID TrackerCredentialFieldId(const char* sourceLabel) {
    return TrackerBackendSectionItemId(SmatchetLocalization::LabelFromSource(sourceLabel));
}

// The settings search box, the first widget of the Preferences window itself.
ImGuiID PrefsSearchBoxId() {
    const ImGuiWindow* win = ImGui::FindWindowByName("Preferences");
    return win != nullptr ? ImHashStr(SmatchetLocalization::LabelFromSource("##PrefsSearch"), 0, win->ID) : 0;
}

// Closes Preferences through the real close gate and waits for resetPreferencesWindowState, so
// the next open is a fresh open edge. Sets the reachability latch first: without it the first-run
// lock re-opens the window on every frame. The staged buffers must still match cfg, or the gate
// raises the unsaved-changes modal instead of closing.
bool ClosePreferences(ImGuiTestContext* ctx) {
    g_ui.cfg.BackendHasBeenReachable = true;
    g_ui.showPreferences = false;
    return YieldUntil(ctx, [] { return !g_ui.showPreferences && !g_ui.preferencesBuffersLoaded; });
}

// Blank Jira credentials: with the reachability latch set, NeedsSetup still reads true (a
// required field is blank) and the first blank field is Domain.
void BlankJiraCredentials(TrackerConfig& cfg) {
    cfg.TrackerType = "Jira";
    cfg.JiraBackends.clear();
    cfg.Domain.clear();
    cfg.Email.clear();
    cfg.ApiToken.clear();
}

// Shared teardown for the open-focus tests: release any text field still being edited, undo
// what the test typed, close through the gate while the buffers still match the test's cfg, and
// only then put the original cfg back (restoring first would re-arm the first-run lock, or make
// the buffers read dirty). Every caller clears JiraBackends, so cfg.Domain is what the Domain
// buffer was loaded from.
void CloseAndRestore(ImGuiTestContext* ctx, const TrackerConfig& cfgBackup, PreferencesCategory categoryBackup) {
    g_keepTrackerBodyLive = false;
    ImGui::ClearActiveID();
    ctx->Yield();
    g_ui.prefsSearchBuf[0] = '\0';
    std::snprintf(g_ui.domainBuf, sizeof(g_ui.domainBuf), "%s", g_ui.cfg.Domain.c_str());
    // A buffer left differing from cfg would raise the unsaved-changes modal here instead of
    // closing, and leave it up for every test that follows.
    const bool closedClean = ClosePreferences(ctx);
    IM_CHECK_NO_RET(closedClean);
    g_ui.cfg = cfgBackup;
    g_ui.preferencesCategory = categoryBackup;
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
        // resetPreferencesWindowState path rather than the guard modal. The reachability
        // latch has to be set for the close to go through at all: on a fresh profile the
        // first-run lock puts the window back on every frame.
        const bool reachableBackup = g_ui.cfg.BackendHasBeenReachable;
        g_ui.cfg.BackendHasBeenReachable = true;
        g_ui.showPreferences = false;
        IM_CHECK_NO_RET(YieldUntil(ctx, [] { return g_ui.trackerPrefsTestVerifiedFingerprint.empty(); }));
        g_ui.cfg.BackendHasBeenReachable = reachableBackup;
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
        // Opening Preferences puts the keyboard in a text field (the first blank credential, or
        // the search box). A text field that is being edited keeps the activation key for
        // itself, so release it first — what a user does by pressing Tab or clicking away.
        ImGui::ClearActiveID();
        ctx->Yield();
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

// ---------------------------------------------------------------------------
// TrackerFirstRun_OpenFocusesBlankCredentialAndEnterRunsTest
// Opening Preferences while the tracker still needs setting up puts the keyboard in the first
// blank credential field, so the user can type without clicking. Enter in a credential field
// then runs Test connection. Both used to need the mouse: nothing took focus on open, and Enter
// in the fields did nothing.
// ---------------------------------------------------------------------------
static void RegisterOpenFocusesBlankCredential(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TrackerFirstRun", "OpenFocusesBlankCredentialAndEnterRunsTest");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { KeepTrackerBodyLive(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const TrackerConfig cfgBackup = g_ui.cfg;
        const PreferencesCategory categoryBackup = g_ui.preferencesCategory;
        const bool closed = ClosePreferences(ctx);
        IM_CHECK_NO_RET(closed);
        if (!closed) {
            g_ui.cfg = cfgBackup;
            return;
        }

        BlankJiraCredentials(g_ui.cfg);
        g_ui.preferencesCategory = PreferencesCategory::Tracker;
        g_ui.prefsSearchBuf[0] = '\0';
        g_keepTrackerBodyLive = true;
        const bool opened = OpenPreferences(ctx) && YieldUntil(ctx, [] { return g_ui.preferencesBuffersLoaded; });
        IM_CHECK_NO_RET(opened);

        // No click: the open itself must have activated the Domain field.
        const bool domainFocused = YieldUntil(
            ctx,
            [] {
                const ImGuiID domainId = TrackerCredentialFieldId("Domain");
                return domainId != 0 && ImGui::GetActiveID() == domainId;
            },
            60);
        IM_CHECK_NO_RET(domainFocused);
        if (domainFocused) {
            ctx->KeyChars("zq");
            ctx->Yield();
            IM_CHECK_NO_RET(std::strcmp(g_ui.domainBuf, "zq") == 0); // typed keys land in the field

            // Blank it again so the probe below runs on the same empty credentials the click
            // test uses (no host name to resolve), and the buffers match cfg at close.
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
            ctx->KeyPress(ImGuiKey_Delete);
            ctx->Yield();
            IM_CHECK_NO_RET(g_ui.domainBuf[0] == '\0');

            const int genBefore = g_ui.trackerPrefsTestGen;
            ctx->KeyPress(ImGuiKey_Enter);
            YieldUntil(ctx, [genBefore] { return g_ui.trackerPrefsTestGen != genBefore; }, 30);
            IM_CHECK_NO_RET(g_ui.trackerPrefsTestGen == genBefore + 1); // Enter ran exactly one probe
            YieldUntil(ctx, [] { return !g_ui.trackerPrefsTestInFlight; }, 600);
        }

        CloseAndRestore(ctx, cfgBackup, categoryBackup);
    };
}

// ---------------------------------------------------------------------------
// TrackerFirstRun_OpenFocusesSearchBoxOnceSetUp
// With the tracker set up there is no blank credential to fill, so an explicit open puts the
// keyboard in the settings search box: typing narrows the settings straight away.
// ---------------------------------------------------------------------------
static void RegisterOpenFocusesSearchBox(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TrackerFirstRun", "OpenFocusesSearchBoxOnceSetUp");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { KeepTrackerBodyLive(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const TrackerConfig cfgBackup = g_ui.cfg;
        const PreferencesCategory categoryBackup = g_ui.preferencesCategory;
        const bool closed = ClosePreferences(ctx);
        IM_CHECK_NO_RET(closed);
        if (!closed) {
            g_ui.cfg = cfgBackup;
            return;
        }

        g_ui.cfg.JiraBackends.clear();
        FillJiraCredentials(g_ui.cfg);
        IM_CHECK_NO_RET(!TrackerSetupPure::NeedsSetup(g_ui.cfg));
        // The Tracker page is showing, so a blank credential WOULD be the target; none is blank.
        g_ui.preferencesCategory = PreferencesCategory::Tracker;
        g_ui.prefsSearchBuf[0] = '\0';
        g_keepTrackerBodyLive = true;
        const bool opened = OpenPreferences(ctx) && YieldUntil(ctx, [] { return g_ui.preferencesBuffersLoaded; });
        IM_CHECK_NO_RET(opened);

        const bool searchFocused = YieldUntil(
            ctx,
            [] {
                const ImGuiID searchId = PrefsSearchBoxId();
                return searchId != 0 && ImGui::GetActiveID() == searchId;
            },
            60);
        IM_CHECK_NO_RET(searchFocused);
        if (searchFocused) {
            ctx->KeyChars("zzqq");
            ctx->Yield();
            IM_CHECK_NO_RET(std::strcmp(g_ui.prefsSearchBuf, "zzqq") == 0);
        }

        CloseAndRestore(ctx, cfgBackup, categoryBackup);
    };
}

// ---------------------------------------------------------------------------
// TrackerFirstRun_RestoredOpenLeavesKeyboardAlone
// A Preferences window that is merely shown — restored with the saved layout at startup, with no
// one asking for it — must not take the keyboard: the user's first keys belong to the grid.
// ---------------------------------------------------------------------------
static void RegisterRestoredOpenLeavesKeyboardAlone(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TrackerFirstRun", "RestoredOpenLeavesKeyboardAlone");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const TrackerConfig cfgBackup = g_ui.cfg;
        const PreferencesCategory categoryBackup = g_ui.preferencesCategory;
        const bool closed = ClosePreferences(ctx);
        IM_CHECK_NO_RET(closed);
        if (!closed) {
            g_ui.cfg = cfgBackup;
            return;
        }

        // Set up, so an ASKED-FOR open would focus the search box (the sibling test above).
        g_ui.cfg.JiraBackends.clear();
        FillJiraCredentials(g_ui.cfg);
        g_ui.prefsSearchBuf[0] = '\0';
        ImGui::ClearActiveID();
        g_ui.requestPreferencesFocus = false;
        g_ui.showPreferences = true; // shown, not requested: what the startup layout restore does

        const bool drawn = YieldUntil(ctx, [] { return g_ui.preferencesBuffersLoaded; }, 120);
        if (!drawn) {
            // Docked behind a sibling tab with nothing selecting it: the body never draws, so
            // there is no open edge to observe on this host.
            ctx->LogInfo("SKIP: the restored Preferences tab never became visible on this host");
        } else {
            // Nothing in the window may activate: not the search box, and not a credential field
            // either. Scoped to the Preferences window, so an unrelated active item elsewhere in
            // the app is not read as a steal.
            bool prefsTookKeyboard = false;
            for (int i = 0; i < 10; ++i) {
                ctx->Yield();
                const ImGuiContext& g = *GImGui;
                const ImGuiWindow* prefs = ImGui::FindWindowByName("Preferences");
                prefsTookKeyboard = prefsTookKeyboard || (g.ActiveId != 0 && g.ActiveIdWindow != nullptr &&
                                                          prefs != nullptr && g.ActiveIdWindow->RootWindow == prefs);
            }
            IM_CHECK_NO_RET(!prefsTookKeyboard);
        }

        CloseAndRestore(ctx, cfgBackup, categoryBackup);
    };
}

// ---------------------------------------------------------------------------
// TrackerFirstRun_CredentialEnterCommit_BareEnterOnly
// Drives the REAL Enter-commit signal the credential rows feed into Test connection, in a
// self-contained window (the docked Preferences body clips its lower rows out of the engine's
// item table). Only a bare Enter is a commit: Enter with Ctrl, Shift or Alt held also deactivates
// a single-line input, and so do Esc and clicking away, but none of them may start a probe.
// ---------------------------------------------------------------------------
namespace {

struct EnterProbeState {
    char plain[64];
    char secret[64];
    int plainEnters;
    int secretEnters;
};

EnterProbeState g_enterProbe;

} // namespace

static void RegisterCredentialEnterCommit(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TrackerFirstRun", "CredentialEnterCommit_BareEnterOnly");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) {
        ImGui::SetNextWindowSize(ImVec2(520, 140), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::CredentialEnter", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::InputText("##PlainProbe", g_enterProbe.plain, sizeof(g_enterProbe.plain));
            if (SmatchetTextInputCommittedWithEnter()) {
                ++g_enterProbe.plainEnters;
            }
            bool secretEnter = false;
            SmatchetSecretInputText("##SecretProbe", g_enterProbe.secret, sizeof(g_enterProbe.secret), nullptr,
                                    &secretEnter);
            if (secretEnter) {
                ++g_enterProbe.secretEnters;
            }
            ImGui::Button("Sink##CredentialEnter"); // focus target: click to leave a field
        }
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_enterProbe = EnterProbeState{};
        ctx->SetRef("SmatchetTest::CredentialEnter");
        ctx->Yield(2);

        ctx->ItemClick("##SecretProbe");
        ctx->KeyChars("abc");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        IM_CHECK_EQ(g_enterProbe.secretEnters, 1);
        IM_CHECK_STR_EQ(g_enterProbe.secret, "abc"); // the commit keeps the typed value
        IM_CHECK_EQ(g_enterProbe.plainEnters, 0);    // reported for the field that was committed only

        ctx->ItemClick("##SecretProbe");
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        ctx->Yield();
        ctx->ItemClick("##SecretProbe");
        ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_Enter);
        ctx->Yield();
        ctx->ItemClick("##SecretProbe");
        ctx->KeyPress(ImGuiMod_Alt | ImGuiKey_Enter);
        ctx->Yield();
        ctx->ItemClick("##SecretProbe");
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield();
        ctx->ItemClick("##SecretProbe");
        ctx->ItemClick("Sink##CredentialEnter");
        ctx->Yield();
        IM_CHECK_EQ(g_enterProbe.secretEnters, 1); // none of those is a commit

        ctx->ItemClick("##SecretProbe");
        ctx->KeyPress(ImGuiKey_KeypadEnter);
        ctx->Yield();
        IM_CHECK_EQ(g_enterProbe.secretEnters, 2);

        ctx->ItemClick("##PlainProbe");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        IM_CHECK_EQ(g_enterProbe.plainEnters, 1);
        IM_CHECK_EQ(g_enterProbe.secretEnters, 2);
    };
}

extern "C" void SmatchetRegisterTrackerFirstRunSetupTests(ImGuiTestEngine* engine) {
    RegisterExplainerBranches(engine);
    RegisterVerifiedPinClearsOnClose(engine);
    RegisterTestConnectionClickBalanced(engine);
    RegisterOpenFocusesBlankCredential(engine);
    RegisterOpenFocusesSearchBox(engine);
    RegisterRestoredOpenLeavesKeyboardAlone(engine);
    RegisterCredentialEnterCommit(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
