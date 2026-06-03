// funcsize_preferences_tabs.test.cpp — Phase-0 GREEN batch of the
// full-function-size-compliance program (docs/plans/shipped/full-function-size-compliance.md).
//
// PURPOSE — STRUCTURAL coverage for the four large Preferences *tab* draw
// functions slated for decomposition in Phase 5:
//   - DrawWhisperPreferencesTab            (SmatchetPreferencesUi_Whisper.cpp,  781L)
//   - DrawAssistantPreferencesTab          (SmatchetPreferencesUi_Assistant.cpp, 533L)
//   - DrawTemplatePreferencesTabs          (SmatchetPreferencesUi_Templates.cpp, 510L)
//   - DrawLocalAndAppearancePreferencesTabs(SmatchetPreferencesUi_Local.cpp,     264L)
//
// These four functions are *called* every frame, but each does its real work
// only INSIDE an `if (ImGui::BeginTabItem("<label>"))` body — that body runs
// only when the tab is the active one. A no-backend smoke test that merely
// opens Preferences (the pilot's PreferencesWindow test) ticks the function
// shells (their BeginTabItem/EndTabItem balance) but NOT the per-tab bodies.
// To exercise the bodies — and therefore catch a Begin/End or Push/PopID
// imbalance a future decomposition might introduce inside them — this test
// boots the real app, opens Preferences, and ItemClicks each target tab in
// turn, YieldUntil-asserting a stable child widget unique to that tab is
// present. Each click activates the tab → its body runs for several frames →
// the ImGui Test Engine traps any in-frame IM_ASSERT and fails the test.
//
// All four tabs render config-backed widgets only — ZERO project / ticket /
// field backend is required (green class per the plan's pilot findings). The
// Assistant + Whisper tabs are feature-gated (SMATCHET_WITH_AI /
// SMATCHET_WITH_WHISPER); under ninja-ui-test-msvc both default ON, so all
// four tabs compile and render. The per-tab assertions are guarded to match.
//
// No checked-in pixel golden — the live tick IS the coverage.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdio>  // std::snprintf — builds the "PreferencesTabs/<label>" header ref
#include <cstring> // std::strcmp — compares the selected-tab name

// g_ui — the shared bag of UI-thread visibility flags (see the pilot file for
// the rationale). We set showPreferences / requestPreferencesFocus directly,
// exactly as the View menu / view.toggle command does.
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

// True once the "PreferencesTabs" tab bar inside the Preferences window has
// `tabLabel` as its selected tab. This is the structural-coverage signal we
// assert after clicking a tab: a selected tab means BeginTabItem(tabLabel)
// returned true this frame → the tab's body draw function actually ran (and
// any Begin/End or Push/PopID imbalance it introduces would have tripped an
// ImGui IM_ASSERT trapped by the engine). It does NOT depend on an in-tab
// child widget being unclipped — the docked Preferences window opens with a
// short content region in the headless app, which clips lower widgets out of
// the Test Engine item table, so ItemExists on tab content is unreliable here.
bool PrefsTabSelected(const char* tabLabel) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* win = ImGui::FindWindowByName("Preferences");
    if (win == nullptr) {
        return false;
    }
    const ImGuiID tabBarId = win->GetID("PreferencesTabs");
    ImGuiTabBar* tabBar = g.TabBars.GetByKey(tabBarId);
    if (tabBar == nullptr) {
        return false;
    }
    ImGuiTabItem* selected = ImGui::TabBarFindTabByID(tabBar, tabBar->SelectedTabId);
    if (selected == nullptr) {
        return false;
    }
    const char* name = ImGui::TabBarGetTabName(tabBar, selected);
    return name != nullptr && std::strcmp(name, tabLabel) == 0;
}

// Open Preferences and tick until the "Preferences" window is live (Active).
// Mirrors the pilot's docked-window open recipe: re-arm the focus latch every
// frame until the docked tab activates, since the draw fn consumes it in one
// frame. Returns true once the window is rendering its body.
bool OpenPreferences(ImGuiTestContext* ctx) {
    g_ui.showPreferences = true;
    g_ui.requestPreferencesFocus = true;
    ctx->SetRef("Preferences");
    return YieldUntil(ctx, [&] {
        g_ui.requestPreferencesFocus = true;
        return WindowIsLive("Preferences");
    });
}

// Click a tab by its label, then YieldUntil that tab is the selected tab in the
// Preferences tab bar — the signal that BeginTabItem(tabLabel) returned true and
// its body draw function ran. A tab *header* is addressed by the
// tab-bar-qualified path "<TabBarID>/<TabLabel>" — the real Preferences tab bar
// is ImGui::BeginTabBar("PreferencesTabs"), so the header ref is
// "PreferencesTabs/<TabLabel>" relative to SetRef("Preferences") (the same
// convention ai_assistant_preferences_docking uses for "##PrefsTabs/Assistant").
// IM_CHECK_NO_RET makes the assertion BITE — a missing tab header, or a tab that
// never becomes selected (so its body never ran), fails the test rather than
// silently passing. Returns true if the tab became selected.
bool ClickTabAndAssertSelected(ImGuiTestContext* ctx, const char* tabLabel) {
    char tabHeaderRef[160];
    std::snprintf(tabHeaderRef, sizeof(tabHeaderRef), "PreferencesTabs/%s", tabLabel);
    const bool tabExists = ctx->ItemExists(tabHeaderRef);
    IM_CHECK_NO_RET(tabExists);
    if (!tabExists) {
        return false;
    }
    ctx->ItemClick(tabHeaderRef);
    const bool selected = YieldUntil(ctx, [&] { return PrefsTabSelected(tabLabel); });
    IM_CHECK_NO_RET(selected);
    return selected;
}

// --- Preferences tab cycle ----------------------------------------------
// Drives each of the four large tab draw functions by selecting its tab and
// asserting a stable in-body widget. Confirms every target body actually
// ticked (not just the function shell).
void RegisterPreferencesTabsRenderSmoke(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FuncSizePreferencesTabs", "AllTargetTabsRenderBodies");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const bool prefsLive = OpenPreferences(ctx);
        IM_CHECK_NO_RET(prefsLive);
        if (!prefsLive) {
            g_ui.showPreferences = false;
            return;
        }

        // Each click activates the named tab → its BeginTabItem body draw
        // function runs for several frames. PrefsTabSelected confirms the body
        // ticked; a Begin/End or Push/PopID imbalance introduced by a future
        // decomposition would trip an ImGui IM_ASSERT trapped by the engine.

        // DrawLocalAndAppearancePreferencesTabs — owns BOTH "Local data" and
        // "Appearance" tab items; selecting each ticks the respective body.
        ClickTabAndAssertSelected(ctx, "Local data");
        ClickTabAndAssertSelected(ctx, "Appearance");

        // DrawTemplatePreferencesTabs — its first tab is "Grid".
        ClickTabAndAssertSelected(ctx, "Grid");

#if defined(SMATCHET_WITH_AI)
        // DrawAssistantPreferencesTab — "Assistant" tab.
        ClickTabAndAssertSelected(ctx, "Assistant");
#endif
#if defined(SMATCHET_WITH_WHISPER)
        // DrawWhisperPreferencesTab — "Whisper" tab.
        ClickTabAndAssertSelected(ctx, "Whisper");
#endif

        g_ui.showPreferences = false;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterFuncSizePreferencesTabsTests(ImGuiTestEngine* engine) {
    RegisterPreferencesTabsRenderSmoke(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
