// Preferences → "Unreal" tab: which host-engine context fields prefill the
// quick-create issue description, plus the Output Log tail length. The tab is
// surfaced only in the Unreal-embedded build (call site is
// #ifdef SMATCHET_EMBEDDED_IN_UNREAL in SmatchetPreferencesUi.cpp) — the TU
// itself compiles in both targets so the config keys stay dual-target-tested.
//
// docs/plans/quick-create-issue-unreal-context.md.

#include "SmatchetPreferencesUi_detail.h"

#include "KeybindingsConfig.h"
#include "SmatchetHelpMarker.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

namespace {

void DrawContextCheckbox(UiDrawSession& d, const char* key, const char* fallback, bool& member) {
    if (ImGui::Checkbox(SmatchetLocalization::T(key, fallback), &member)) {
        MarkPrefsDirty(d);
    }
}

} // namespace

void DrawQuickCreatePreferencesTab(UiDrawSession& d) {
    if (!ImGui::BeginTabItem(SmatchetLocalization::T("quickcreate.prefs.tabTitle", "Unreal"))) {
        return;
    }
    d.preferencesActiveTab = PreferencesActiveTab::QuickCreate;

    const std::string hotkey = BoundHotkeyDisplay(d.cfg.Keybindings.Bindings, "issue.quick_create.open");
    ImGui::TextWrapped(
        "%s", SmatchetLocalization::Format(
                  "quickcreate.prefs.intro",
                  "The quick-create issue popup (%s) prefills its description with context from "
                  "the running Unreal Engine session. Choose which items are included:",
                  hotkey.empty() ? SmatchetLocalization::T("quickcreate.prefs.unbound", "unbound") : hotkey.c_str()));
    ImGui::Spacing();

    DrawContextCheckbox(d, "quickcreate.prefs.engine_version", "Engine version", d.cfg.QuickCreateCtxEngineVersion);
    DrawContextCheckbox(d, "quickcreate.prefs.project", "Project name and directory", d.cfg.QuickCreateCtxProject);
    DrawContextCheckbox(d, "quickcreate.prefs.platform", "Platform, OS, and build configuration",
                        d.cfg.QuickCreateCtxPlatform);
    DrawContextCheckbox(d, "quickcreate.prefs.level", "Current level / map", d.cfg.QuickCreateCtxLevel);
    DrawContextCheckbox(d, "quickcreate.prefs.pie_state", "Play-in-editor (PIE) state", d.cfg.QuickCreateCtxPieState);
    DrawContextCheckbox(d, "quickcreate.prefs.selected_actors", "Selected actors", d.cfg.QuickCreateCtxSelectedActors);
    DrawContextCheckbox(d, "quickcreate.prefs.log_tail", "Output Log tail", d.cfg.QuickCreateCtxLogTail);

    if (d.cfg.QuickCreateCtxLogTail) {
        ImGui::SameLine();
        SmatchetHelpMarker::Render("quickcreate.prefs.log_tail.help",
                                   "The most recent Unreal Output Log lines are embedded as a code block. "
                                   "Review the prefilled description before submitting — log lines can "
                                   "contain paths or other details you may not want in the tracker.");
        ImGui::SetNextItemWidth(120.0f);
        int lines = d.cfg.QuickCreateCtxLogLines;
        if (ImGui::InputInt(SmatchetLocalization::T("quickcreate.prefs.log_lines", "Log lines"), &lines)) {
            if (lines < 1) {
                lines = 1;
            }
            if (lines > 300) {
                lines = 300;
            }
            d.cfg.QuickCreateCtxLogLines = lines;
            MarkPrefsDirty(d);
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "%s", SmatchetLocalization::T("quickcreate.prefs.note",
                                      "Items missing from the engine snapshot are skipped even when enabled."));

    ImGui::EndTabItem();
}
