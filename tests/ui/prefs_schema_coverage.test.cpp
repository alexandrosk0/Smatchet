// prefs_schema_coverage.test.cpp — drift guard for the Preferences settings
// descriptor table (docs/plans/shipped/preferences-ia-resegmentation-and-search.md,
// slice 3).
//
// PROBLEM: PreferencesSchema.cpp is a hand-maintained table. Nothing in the
// compiler ties a descriptor row to the widget that draws it, so the table rots
// in both directions — a deleted widget leaves a phantom row that the search
// still offers, and a new widget without a row is unreachable by search (it
// fails OPEN, so it is silently always-visible instead).
//
// GUARD: PreferencesFilter records every id passed to ShowSetting. This test
// walks all 8 categories with an EMPTY query (everything draws) and diffs the
// observed set against the descriptor table:
//   - observed  \ descriptors  => a widget gating on an unknown id. Always a bug.
//   - descriptors \ observed   => a row nothing draws. A bug UNLESS the row is
//                                 marked ConditionalDraw (per-backend /
//                                 per-provider rows that legitimately sit out a
//                                 frame).
//
// No input is driven: categories are selected by assigning
// g_ui.preferencesCategory, exactly like the nav-rail combo mode does, so the
// test does not depend on hit-testing. Collapsed sections are cleared every
// frame — a section the user collapsed in a previous run never runs its body,
// which would otherwise read as missing coverage.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"
#include "Ui/PreferencesSchema.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

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

// Same docked-window open recipe as funcsize_preferences_tabs: re-arm the focus
// latch every frame until the docked tab activates.
bool OpenPreferences(ImGuiTestContext* ctx) {
    g_ui.showPreferences = true;
    g_ui.requestPreferencesFocus = true;
    ctx->SetRef("Preferences");
    return YieldUntil(ctx, [&] {
        g_ui.requestPreferencesFocus = true;
        return WindowIsLive("Preferences");
    });
}

/// Force every section open for this frame. The collapsed set is persisted
/// (cfg.PreferencesCollapsedSections), so a developer's own collapsed section
/// would otherwise hide its settings from the walk. Setting the loaded latch
/// stops the lazy re-parse from putting them back.
void ForceAllSectionsExpanded() {
    g_ui.prefsCollapsedSections.clear();
    g_ui.prefsCollapsedLoaded = true;
}

/// Tick `category` for enough frames that every section body runs (list editors
/// and combos seed lazily on their first frame).
void TickCategory(ImGuiTestContext* ctx, PreferencesCategory category) {
    g_ui.preferencesCategory = category;
    for (int i = 0; i < 8; ++i) {
        // Preferences is a docked window: a sibling tab stealing selection makes
        // Begin() return false and the whole body never runs, which would read
        // as "no setting drew anything".
        g_ui.requestPreferencesFocus = true;
        ForceAllSectionsExpanded();
        ctx->Yield();
    }
}

/// Tick a category and report how many ids it contributed — a category that
/// contributes zero points at a broken walk (window not drawing), not at 20
/// individually-missing descriptors.
void WalkCategory(ImGuiTestContext* ctx, PreferencesCategory category, const char* name) {
    const std::size_t before = g_ui.prefsFilter.ObservedSettingIds().size();
    TickCategory(ctx, category);
    const std::size_t after = g_ui.prefsFilter.ObservedSettingIds().size();
    ctx->LogInfo("category %s contributed %d setting id(s)", name, (int)(after - before));
}

std::string JoinIds(const std::vector<std::string>& ids) {
    std::string out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += ids[i];
    }
    return out;
}

void RegisterPrefsSchemaCoverage(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "PrefsSchemaCoverage", "DescriptorTableMatchesDrawnSettings");
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

        // Empty query: ShowSetting returns true for everything, which is exactly the
        // walk this guard needs. Id recording is off on the production draw path
        // (allocator traffic per drawn setting per frame), so switch it on for the
        // walk and back off afterwards.
        std::memset(g_ui.prefsSearchBuf, 0, sizeof(g_ui.prefsSearchBuf));
        ctx->Yield();
        g_ui.prefsFilter.SetRecordObservedSettingIds(true);
        g_ui.prefsFilter.ResetObservedSettingIds();

        WalkCategory(ctx, PreferencesCategory::General, "General");
        WalkCategory(ctx, PreferencesCategory::Appearance, "Appearance");
        WalkCategory(ctx, PreferencesCategory::Tracker, "Tracker");
        WalkCategory(ctx, PreferencesCategory::Connections, "Connections");
        WalkCategory(ctx, PreferencesCategory::Editing, "Editing");
        WalkCategory(ctx, PreferencesCategory::Shortcuts, "Shortcuts");
        WalkCategory(ctx, PreferencesCategory::Annotate, "Annotate");
#if defined(SMATCHET_WITH_AI) || defined(SMATCHET_WITH_WHISPER)
        WalkCategory(ctx, PreferencesCategory::AiVoice, "AiVoice");
#endif

        const std::set<std::string> observed = g_ui.prefsFilter.ObservedSettingIds();
        g_ui.prefsFilter.SetRecordObservedSettingIds(false);

        std::size_t settingCount = 0;
        const SmatchetPrefsSchema::PrefsSettingDesc* settings = SmatchetPrefsSchema::Settings(settingCount);
        std::set<std::string> declared;
        std::vector<std::string> missing; // declared, never drawn, not ConditionalDraw
        for (std::size_t i = 0; i < settingCount; ++i) {
            declared.insert(settings[i].Id);
            if (!settings[i].ConditionalDraw && observed.count(settings[i].Id) == 0) {
                missing.push_back(settings[i].Id);
            }
        }
        std::vector<std::string> unknown; // drawn, but no descriptor row
        for (std::set<std::string>::const_iterator it = observed.begin(); it != observed.end(); ++it) {
            if (declared.count(*it) == 0) {
                unknown.push_back(*it);
            }
        }

        if (!unknown.empty()) {
            ctx->LogError("ShowSetting called with %d id(s) absent from PreferencesSchema: %s", (int)unknown.size(),
                          JoinIds(unknown).c_str());
        }
        if (!missing.empty()) {
            ctx->LogError("%d always-drawn descriptor(s) were never drawn (add ConditionalDraw or fix the call "
                          "site): %s",
                          (int)missing.size(), JoinIds(missing).c_str());
        }
        IM_CHECK_NO_RET(unknown.empty());
        IM_CHECK_NO_RET(missing.empty());
        ctx->LogInfo("observed %d setting id(s) across all categories; table declares %d", (int)observed.size(),
                     (int)declared.size());

        g_ui.showPreferences = false;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterPrefsSchemaCoverageTests(ImGuiTestEngine* engine) {
    RegisterPrefsSchemaCoverage(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
