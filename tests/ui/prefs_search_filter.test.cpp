// prefs_search_filter.test.cpp — end-to-end proof that the Preferences search
// filters LIVE widgets in place rather than rendering a read-only result list
// (docs/plans/shipped/preferences-ia-resegmentation-and-search.md, slice 3).
//
// Typing `vsy` must leave exactly one setting standing — "Enable vsync" — and
// that survivor must be the real checkbox, still bound to cfg.VsyncEnabled and
// still toggleable where it sits. The sibling setting in the same section
// ("Ask before layout-resetting changes") is the negative control: if it is
// still on screen, the filter is decorative.
//
// The query is set by writing g_ui.prefsSearchBuf directly. drawPreferencesWindow
// feeds that buffer to PreferencesFilter::Update every frame, so this drives the
// exact production path without depending on keystroke injection.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName, ImHashStr — real-window/id probes
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>

extern UiDrawSession g_ui;

namespace {

const char* const kDisplaySection = "appearance.display";
const char* const kVsyncLabel = "Enable vsync";
const char* const kControlLabel = "Ask before layout-resetting changes";

// Preferences is a docked window. Its tab loses selection unless
// g_ui.requestPreferencesFocus is re-armed, and beginPreferencesWindow consumes
// the latch in a single frame — one un-armed frame makes Begin() return false
// and the whole body, every widget under test, stops being submitted.
//
// Re-arming from the test coroutine is NOT enough: ItemExists/ItemClick yield
// frames internally, and those frames would run un-armed, so the widget is
// never re-submitted while the engine's info task is pending and no ref can
// ever resolve. GuiFunc runs on EVERY frame while the test is active, so arming
// there keeps the body alive across engine-internal yields too.
bool g_armPreferences = false;

void ArmPreferencesFrame() {
    if (!g_armPreferences) {
        return;
    }
    g_ui.requestPreferencesFocus = true;
    // A section collapsed in an earlier run is persisted, and a collapsed
    // section never runs its body — that would read as "the filter hid it".
    g_ui.prefsCollapsedSections.clear();
    g_ui.prefsCollapsedLoaded = true;
    // The update-available modal grabs nav and inhibits hovering everywhere
    // else, so no widget under it can be activated. BeginPopupModal closes on a
    // false p_open, so clearing the flag dismisses it for the run.
    g_ui.appUpdateModalOpen = false;
}

/// Resolve a settings widget to its raw ImGui id.
///
/// The engine's `**/label` wildcard does not resolve into the Preferences body,
/// so refs are built from the id chain the draw code actually uses: the
/// PrefsBody child window seeds the stack, PrefsSectionBegin pushes the dotted
/// section id, and the widget hashes its label under that.
ImGuiID SettingItemId(const char* sectionId, const char* label) {
    ImGuiContext* g = ImGui::GetCurrentContext();
    for (int i = 0; i < g->Windows.Size; ++i) {
        ImGuiWindow* win = g->Windows[i];
        if (std::strstr(win->Name, "PrefsBody") == nullptr) {
            continue;
        }
        return ImHashStr(label, 0, ImHashStr(sectionId, 0, win->ID));
    }
    return 0;
}

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

bool OpenPreferences(ImGuiTestContext* ctx) {
    g_ui.showPreferences = true;
    g_armPreferences = true;
    ctx->PopupCloseAll(); // a modal left open by an earlier test inhibits every click below it
    ctx->SetRef("Preferences");
    return YieldUntil(ctx, [&] { return WindowIsLive("Preferences"); });
}

void SetQuery(ImGuiTestContext* ctx, const char* query) {
    std::memset(g_ui.prefsSearchBuf, 0, sizeof(g_ui.prefsSearchBuf));
    if (query != nullptr) {
#if defined(_MSC_VER)
        strncpy_s(g_ui.prefsSearchBuf, sizeof(g_ui.prefsSearchBuf), query, _TRUNCATE);
#else
        std::strncpy(g_ui.prefsSearchBuf, query, sizeof(g_ui.prefsSearchBuf) - 1);
#endif
    }
    // Two frames for Update() to reindex, two more for the sections to settle.
    for (int i = 0; i < 4; ++i) {
        ctx->Yield();
    }
}

void RegisterPrefsSearchFilter(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "PrefsSearchFilter", "QueryLeavesOnlyTheLiveMatchingWidget");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { ArmPreferencesFrame(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const bool prefsLive = OpenPreferences(ctx);
        IM_CHECK_NO_RET(prefsLive);
        if (!prefsLive) {
            g_armPreferences = false;
            g_ui.showPreferences = false;
            return;
        }

        const bool originalVsync = g_ui.cfg.VsyncEnabled;
        g_ui.preferencesCategory = PreferencesCategory::Appearance;
        SetQuery(ctx, "");

        const ImGuiTestRef vsyncRef(SettingItemId(kDisplaySection, kVsyncLabel));
        const ImGuiTestRef controlRef(SettingItemId(kDisplaySection, kControlLabel));

        // Baseline: both siblings render when nothing is filtered.
        IM_CHECK_NO_RET(ctx->ItemExists(vsyncRef));
        IM_CHECK_NO_RET(ctx->ItemExists(controlRef));

        SetQuery(ctx, "vsy");
        IM_CHECK_NO_RET(g_ui.prefsFilter.Active());
        // One descriptor matches `vsy`; the "showing N of M" readout the user
        // sees is driven by the same counts.
        IM_CHECK_NO_RET(g_ui.prefsFilter.MatchCount() == 1);
        IM_CHECK_NO_RET(g_ui.prefsFilter.TotalCount() > 1);

        const bool survivorDrawn = ctx->ItemExists(vsyncRef);
        const bool controlHidden = !ctx->ItemExists(controlRef);
        if (!survivorDrawn) {
            ctx->LogError("query 'vsy' hid the matching 'Enable vsync' checkbox");
        }
        if (!controlHidden) {
            ctx->LogError("query 'vsy' left the non-matching 'Ask before layout-resetting changes' checkbox on screen "
                          "— the filter is not filtering");
        }
        IM_CHECK_NO_RET(survivorDrawn);
        IM_CHECK_NO_RET(controlHidden);

        // The survivor must be the LIVE widget, not a rendering of the match:
        // activating it flips the bound config value in place.
        //
        // Nav-activate, not ItemClick: the Preferences body sits inside a dock
        // node the engine cannot re-arrange to clear a click path ("Failed to
        // move window '##DockNode_0A'"), so mouse aiming never reaches the
        // checkbox. Nav activation exercises the same ImGui::Checkbox return
        // path without depending on screen coordinates. It still needs every
        // modal dismissed — a modal owns NavWindow and blocks SetNavId here.
        if (survivorDrawn) {
            ctx->SetInputMode(ImGuiInputSource_Keyboard);
            ctx->ItemNavActivate(vsyncRef);
            const bool toggled = YieldUntil(ctx, [&] { return g_ui.cfg.VsyncEnabled != originalVsync; }, 30);
            if (!toggled) {
                ctx->LogError("filtered 'Enable vsync' did not toggle cfg.VsyncEnabled — survivor is not editable in "
                              "place");
            }
            IM_CHECK_NO_RET(toggled);
            if (toggled) {
                ctx->ItemNavActivate(vsyncRef); // restore the user's setting
                YieldUntil(ctx, [&] { return g_ui.cfg.VsyncEnabled == originalVsync; }, 30);
            }
        }

        SetQuery(ctx, "");
        IM_CHECK_NO_RET(!g_ui.prefsFilter.Active());
        IM_CHECK_NO_RET(ctx->ItemExists(controlRef)); // clearing the query restores the hidden sibling

        g_armPreferences = false;
        g_ui.showPreferences = false;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterPrefsSearchFilterTests(ImGuiTestEngine* engine) {
    RegisterPrefsSearchFilter(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
