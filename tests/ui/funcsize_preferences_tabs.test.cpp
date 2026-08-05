// funcsize_preferences_tabs.test.cpp — Phase-0 GREEN batch of the
// full-function-size-compliance program (docs/plans/shipped/full-function-size-compliance.md),
// retargeted from the old "PreferencesTabs" tab bar to the category nav rail
// (docs/plans/active/preferences-ia-resegmentation-and-search.md, slice 2a).
//
// PURPOSE — STRUCTURAL coverage for the large Preferences *page* draw
// functions:
//   - DrawWhisperPreferencesTab       (SmatchetPreferencesUi_Whisper.cpp)
//   - DrawAssistantPreferencesTab     (SmatchetPreferencesUi_Assistant.cpp)
//   - DrawEditingPreferencesTab       (SmatchetPreferencesUi_Templates.cpp)
//   - DrawGeneralPreferencesTab       (SmatchetPreferencesUi_General.cpp)
//   - DrawAppearancePreferencesTab    (SmatchetPreferencesUi_Local.cpp)
//   - DrawKeybindingsPreferencesTab   (SmatchetPreferencesUi_Keybindings.cpp)
//   - drawPreferencesConnectionsTab /
//     drawPreferencesAnnotateTab      (SmatchetPreferencesUi.cpp)
//
// Since slice 2a, drawPreferencesWindow dispatches on d.preferencesCategory —
// a page body runs every frame its category is the selected one (no
// BeginTabItem gate any more). Selecting each category in turn ticks the
// page body for several frames; the ImGui Test Engine traps any in-frame
// IM_ASSERT (Begin/End or Push/PopID imbalance) and fails the test.
//
// Selection path: the nav rail Selectables carry stable "###prefsNav*" ids,
// addressed by wildcard ref ("**/…") because they live inside the
// "PrefsNavRail" child window. Below the narrow-width threshold the rail is
// replaced by a combo; in that mode the rail item doesn't exist and the test
// falls back to assigning g_ui.preferencesCategory directly — same body
// coverage, no click.
//
// All pages render config-backed widgets only — ZERO project / ticket /
// field backend is required. Assistant + Whisper are feature-gated
// (SMATCHET_WITH_AI / SMATCHET_WITH_WHISPER); under ninja-ui-test-msvc both
// default ON. No checked-in pixel golden — the live tick IS the coverage.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

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

// Select a category via its nav-rail Selectable (wildcard ref — the rail is a
// child window), or by direct assignment when the narrow-width combo replaced
// the rail. Then YieldUntil the dispatch actually ran on that category for a
// few frames. IM_CHECK_NO_RET makes a selection that never lands fail the
// test rather than silently passing.
bool SelectCategoryAndAssert(ImGuiTestContext* ctx, const char* railRef, PreferencesCategory expected) {
    if (ctx->ItemExists(railRef)) {
        ctx->ItemClick(railRef);
    } else {
        ctx->LogInfo("nav rail item '%s' absent (combo mode) — selecting category directly", railRef);
        g_ui.preferencesCategory = expected;
    }
    const bool selected = YieldUntil(ctx, [&] { return g_ui.preferencesCategory == expected; });
    IM_CHECK_NO_RET(selected);
    if (!selected) {
        return false;
    }
    // Let the page body tick a few frames so an imbalance inside it trips.
    for (int i = 0; i < 5; ++i) {
        ctx->Yield();
    }
    return true;
}

// --- Preferences category cycle ------------------------------------------
// Drives each of the large page draw functions by selecting its category and
// asserting the dispatch landed. Confirms every target body actually ticked.
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

        // DrawGeneralPreferencesTab — Updates / Language & region / Storage /
        // Local database, the sections gathered out of the old Appearance and
        // "Local data" tabs by slice 2b.
        SelectCategoryAndAssert(ctx, "**/General###prefsNavGeneral", PreferencesCategory::General);
        SelectCategoryAndAssert(ctx, "**/Appearance###prefsNavAppearance", PreferencesCategory::Appearance);
        SelectCategoryAndAssert(ctx, "**/Tracker###prefsNavTracker", PreferencesCategory::Tracker);

        // drawPreferencesConnectionsTab — MCP + Perforce + activity sources.
        SelectCategoryAndAssert(ctx, "**/Connections###prefsNavConnections", PreferencesCategory::Connections);

        // DrawEditingPreferencesTab — the five sections that replaced the nested
        // "Fields Inputs" tab bar.
        SelectCategoryAndAssert(ctx, "**/Editing###prefsNavEditing", PreferencesCategory::Editing);

        // DrawKeybindingsPreferencesTab / drawPreferencesAnnotateTab.
        SelectCategoryAndAssert(ctx, "**/Shortcuts###prefsNavShortcuts", PreferencesCategory::Shortcuts);
        SelectCategoryAndAssert(ctx, "**/Annotate###prefsNavAnnotate", PreferencesCategory::Annotate);

#if defined(SMATCHET_WITH_AI) || defined(SMATCHET_WITH_WHISPER) // same guard as the rail row
        // DrawAssistantPreferencesTab + DrawWhisperPreferencesTab now share one
        // category. The rail label may carry a " *" dirty marker, but
        // "###prefsNavAiVoice" keeps the item id stable either way.
        SelectCategoryAndAssert(ctx, "**/AI & Voice###prefsNavAiVoice", PreferencesCategory::AiVoice);
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
