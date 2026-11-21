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
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"
#include "Ui/SmatchetPrefsSearchFocus.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName, ImHashStr — real-window/id probes
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <chrono>
#include <cstring>
#include <future>
#include <nlohmann/json.hpp>
#include <string>

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

// ---- Search keys (INPUT_UX_AUDIT.md § 4.4, finding #79) --------------------------------------------

// The search box sits in the Preferences window itself, above the nav rail and the PrefsBody child.
ImGuiID PrefsSearchId() {
    ImGuiWindow* win = ImGui::FindWindowByName("Preferences");
    return win != nullptr ? win->GetID("##PrefsSearch") : 0;
}

bool PrefsSearchHasCaret() {
    const ImGuiID id = PrefsSearchId();
    return id != 0 && ImGui::GetCurrentContext()->ActiveId == id;
}

bool PreferencesClosed() { return !g_ui.showPreferences && !g_ui.preferencesBuffersLoaded; }

// The command handlers hop to the UI thread and wait for it, so they run on a worker while the test
// keeps stepping frames.
smatchet::cmd::CommandResult RunCommand(ImGuiTestContext* ctx, AppController* app, const std::string& name) {
    std::future<smatchet::cmd::CommandResult> pending = std::async(std::launch::async, [app, name] {
        smatchet::cmd::CommandContext cmdCtx;
        return app->Commands().Dispatch(name, nlohmann::json::object(), cmdCtx);
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        ctx->Yield();
    }
    return pending.get();
}

// What a search-keys case changes and must put back, whichever step failed. The first-run lock keeps
// Preferences open for as long as the backend has never been reachable, so these cases turn it off (as the
// AiPrefs helper does) and the window can really close. A caret left in the search box would keep copying
// its own text over prefsSearchBuf every frame and keep the filter on for whoever runs next: the window's
// other pages stay hidden behind it, and the next case can neither open nor close Preferences.
//
// Closing Preferences for real has a side effect on the dock layout. Preferences, Log and Scripting share
// the bottom-panel node, and in a fresh test home Preferences is its only tenant: ImGui deletes a node when
// its last window leaves, so the slot is gone for every later case (Log then floats, Scripting finds no
// node to dock into). The case therefore keeps the Log window open for as long as it may close Preferences,
// and gives the window back at the end, so a tenant is always there.
class SearchKeysCase {
  public:
    SearchKeysCase()
        : reachable_(g_ui.cfg.BackendHasBeenReachable), category_(g_ui.preferencesCategory),
          vsync_(g_ui.cfg.VsyncEnabled), showLog_(g_ui.showLogWindow) {
        g_ui.cfg.BackendHasBeenReachable = true;
    }
    SearchKeysCase(const SearchKeysCase&) = delete;
    SearchKeysCase& operator=(const SearchKeysCase&) = delete;
    ~SearchKeysCase() {
        g_armPreferences = false;
        ImGui::ClearActiveID();
        g_ui.prefsSearchBuf[0] = '\0';
        g_ui.showPreferences = false;
        g_ui.cfg.BackendHasBeenReachable = reachable_;
        g_ui.preferencesCategory = category_;
        g_ui.cfg.VsyncEnabled = vsync_;
        g_ui.showLogWindow = showLog_; // after the lock is back: Preferences is then drawn again as a tenant
    }

    // Opens the Log window and waits until it is drawn, so the bottom-panel node has a second tenant before
    // anything closes Preferences. Call first in every case.
    bool KeepBottomPanelAlive(ImGuiTestContext* ctx) const {
        g_ui.showLogWindow = true;
        const bool live = YieldUntil(ctx, [] { return WindowIsLive("Log"); }, 120);
        if (!live) {
            ctx->LogError("the Log window did not come up to keep the bottom-panel dock node alive");
        }
        IM_CHECK_NO_RET(live);
        return live;
    }

    // Closes the window and waits until it is closed, so the next case starts from a closed one.
    void Close(ImGuiTestContext* ctx) const {
        g_armPreferences = false;
        ImGui::ClearActiveID();
        g_ui.prefsSearchBuf[0] = '\0';
        g_ui.showPreferences = false;
        YieldUntil(ctx, [] { return PreferencesClosed(); }, 120);
    }

    bool VsyncUnchanged() const { return g_ui.cfg.VsyncEnabled == vsync_; }

  private:
    bool reachable_;
    PreferencesCategory category_;
    bool vsync_;
    bool showLog_;
};

// Opens the window on the Appearance page with an empty query and no caret anywhere.
bool OpenForSearchKeys(ImGuiTestContext* ctx) {
    g_ui.preferencesCategory = PreferencesCategory::Appearance;
    const bool live = OpenPreferences(ctx);
    IM_CHECK_NO_RET(live);
    if (!live) {
        return false;
    }
    ImGui::ClearActiveID();
    SetQuery(ctx, "");
    ctx->SetInputMode(ImGuiInputSource_Keyboard);
    return true;
}

bool FocusSearchWithChord(ImGuiTestContext* ctx) {
    IM_CHECK_NO_RET(!PrefsSearchHasCaret());
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Comma);
    const bool focused = YieldUntil(ctx, [] { return PrefsSearchHasCaret(); }, 60);
    if (!focused) {
        ctx->LogError("Ctrl+Shift+, did not put the caret in the settings search box");
    }
    IM_CHECK_NO_RET(focused);
    return focused;
}

void RegisterPrefsSearchKeys(ImGuiTestEngine* engine) {
    // Ctrl+Shift+, (preferences.search.focus) puts the caret in the box from wherever it is.
    ImGuiTest* t = IM_REGISTER_TEST(engine, "PrefsSearchFilter", "SearchKeys_ChordFocusesTheSearchBox");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { ArmPreferencesFrame(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        SearchKeysCase scope;
        scope.KeepBottomPanelAlive(ctx);
        if (OpenForSearchKeys(ctx)) {
            FocusSearchWithChord(ctx);
        }
        scope.Close(ctx);
    };

    // Esc in the box clears the query and leaves the box.
    t = IM_REGISTER_TEST(engine, "PrefsSearchFilter", "SearchKeys_EscClearsTheQueryAndLeavesTheBox");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { ArmPreferencesFrame(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        SearchKeysCase scope;
        scope.KeepBottomPanelAlive(ctx);
        if (OpenForSearchKeys(ctx) && FocusSearchWithChord(ctx)) {
            ctx->KeyChars("vsy");
            IM_CHECK_NO_RET(YieldUntil(ctx, [] { return g_ui.prefsFilter.Active(); }, 30));
            ctx->KeyPress(ImGuiKey_Escape);
            const bool cleared = YieldUntil(ctx, [] { return g_ui.prefsSearchBuf[0] == '\0'; }, 30);
            if (!cleared) {
                ctx->LogError("Esc in the search box did not clear the query");
            }
            IM_CHECK_NO_RET(cleared);
            ctx->Yield(3); // a deactivated box writes its saved copy back on a later draw if that copy was kept
            IM_CHECK_NO_RET(g_ui.prefsSearchBuf[0] == '\0');
            IM_CHECK_NO_RET(!PrefsSearchHasCaret());
        }
        scope.Close(ctx);
    };

    // Enter in the box sends the keyboard to the first matching setting. It must not change that setting: the
    // matched widget is focused, never pressed. Where ImGui's navigation lands depends on how a test drives
    // window focus, so this asserts the decision (the hook ran) and the effect that matters (nothing changed).
    t = IM_REGISTER_TEST(engine, "PrefsSearchFilter", "SearchKeys_EnterFocusesTheFirstMatchWithoutChangingIt");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { ArmPreferencesFrame(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        SearchKeysCase scope;
        scope.KeepBottomPanelAlive(ctx);
        if (OpenForSearchKeys(ctx) && FocusSearchWithChord(ctx)) {
            ctx->KeyChars("vsy");
            IM_CHECK_NO_RET(YieldUntil(ctx, [] { return g_ui.prefsFilter.Active(); }, 30));
            IM_CHECK_NO_RET(g_ui.prefsFilter.MatchCount() == 1);
            const int before = smatchet::ui::PrefsSearchFirstMatchFocusCountForTest();
            ctx->KeyPress(ImGuiKey_Enter);
            const bool focused = YieldUntil(
                ctx, [before] { return smatchet::ui::PrefsSearchFirstMatchFocusCountForTest() > before; }, 60);
            if (!focused) {
                ctx->LogError("Enter in the search box did not send the keyboard to the first matching setting");
            }
            IM_CHECK_NO_RET(focused);
            ctx->Yield(3);
            IM_CHECK_NO_RET(scope.VsyncUnchanged());                       // focused, not pressed
            IM_CHECK_NO_RET(std::strcmp(g_ui.prefsSearchBuf, "vsy") == 0); // Enter keeps the query
        }
        scope.Close(ctx);
    };

    // The command opens a closed Preferences window with the caret in the search box (palette, CLI, MCP and
    // Lua reach the same handler).
    t = IM_REGISTER_TEST(engine, "PrefsSearchFilter", "SearchFocusCommand_OpensPreferencesInTheSearchBox");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { ArmPreferencesFrame(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        SearchKeysCase scope;
        scope.KeepBottomPanelAlive(ctx);
        ctx->PopupCloseAll();
        scope.Close(ctx);
        IM_CHECK_NO_RET(PreferencesClosed());
        g_armPreferences = true; // keep the docked tab selected once the command opens it
        const smatchet::cmd::CommandResult r = RunCommand(ctx, app, "preferences.search.focus");
        IM_CHECK_NO_RET(r.Ok);
        IM_CHECK_NO_RET(g_ui.showPreferences);
        const bool focused = YieldUntil(ctx, [] { return PrefsSearchHasCaret(); }, 120);
        if (!focused) {
            ctx->LogError("preferences.search.focus did not put the caret in the settings search box");
        }
        IM_CHECK_NO_RET(focused);
        scope.Close(ctx);
    };

    // A query is about one visit: closing Preferences clears it.
    t = IM_REGISTER_TEST(engine, "PrefsSearchFilter", "ClosingPreferences_ClearsTheQuery");
    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) { ArmPreferencesFrame(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }
        SearchKeysCase scope;
        scope.KeepBottomPanelAlive(ctx);
        if (OpenForSearchKeys(ctx)) {
            SetQuery(ctx, "vsy");
            IM_CHECK_NO_RET(g_ui.prefsFilter.Active());
            // Close without clearing the query ourselves: the window's own close does that.
            g_armPreferences = false;
            g_ui.showPreferences = false;
            IM_CHECK_NO_RET(YieldUntil(ctx, [] { return PreferencesClosed(); }, 120));
            IM_CHECK_NO_RET(g_ui.prefsSearchBuf[0] == '\0');
        }
        scope.Close(ctx);
    };
}

} // namespace

extern "C" void SmatchetRegisterPrefsSearchFilterTests(ImGuiTestEngine* engine) {
    RegisterPrefsSearchFilter(engine);
    RegisterPrefsSearchKeys(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
