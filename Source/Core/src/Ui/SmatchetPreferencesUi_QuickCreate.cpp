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

void DrawContextCheckbox(UiDrawSession& d, const char* settingId, const char* key, const char* fallback, bool& member) {
    if (!d.prefsFilter.ShowSetting(settingId)) {
        return;
    }
    if (ImGui::Checkbox(SmatchetLocalization::T(key, fallback), &member)) {
        MarkPrefsDirty(d);
    }
}

void DrawQuickCreateSectionBody(UiDrawSession& d) {
    // Intro + trailing note are section chrome, not settings: they own no descriptor
    // row, so a narrowed query drops them rather than padding the result.
    if (!d.prefsFilter.Active()) {
        const std::string hotkey = BoundHotkeyDisplay(d.cfg.Keybindings.Bindings, "issue.quick_create.open");
        ImGui::TextWrapped("%s", SmatchetLocalization::Format(
                                     "quickcreate.prefs.intro",
                                     "The quick-create issue popup (%s) prefills its description with context from "
                                     "the running Unreal Engine session. Choose which items are included:",
                                     hotkey.empty() ? SmatchetLocalization::T("quickcreate.prefs.unbound", "unbound")
                                                    : hotkey.c_str()));
        ImGui::Spacing();
    }

    DrawContextCheckbox(d, "editing.quick_create.engine_version", "quickcreate.prefs.engine_version", "Engine version",
                        d.cfg.QuickCreateCtxEngineVersion);
    DrawContextCheckbox(d, "editing.quick_create.project", "quickcreate.prefs.project", "Project name and directory",
                        d.cfg.QuickCreateCtxProject);
    DrawContextCheckbox(d, "editing.quick_create.platform", "quickcreate.prefs.platform",
                        "Platform, OS, and build configuration", d.cfg.QuickCreateCtxPlatform);
    DrawContextCheckbox(d, "editing.quick_create.level", "quickcreate.prefs.level", "Current level / map",
                        d.cfg.QuickCreateCtxLevel);
    DrawContextCheckbox(d, "editing.quick_create.pie_state", "quickcreate.prefs.pie_state",
                        "Play-in-editor (PIE) state", d.cfg.QuickCreateCtxPieState);
    DrawContextCheckbox(d, "editing.quick_create.selected_actors", "quickcreate.prefs.selected_actors",
                        "Selected actors", d.cfg.QuickCreateCtxSelectedActors);
    // Drawn inline rather than via DrawContextCheckbox: the privacy help marker belongs
    // to this row and must stay with it when the filter hides its neighbours.
    if (d.prefsFilter.ShowSetting("editing.quick_create.log_tail")) {
        if (ImGui::Checkbox(SmatchetLocalization::T("quickcreate.prefs.log_tail", "Output Log tail"),
                            &d.cfg.QuickCreateCtxLogTail)) {
            MarkPrefsDirty(d);
        }
        if (d.cfg.QuickCreateCtxLogTail) {
            ImGui::SameLine();
            SmatchetHelpMarker::Render("quickcreate.prefs.log_tail.help",
                                       "The most recent Unreal Output Log lines are embedded as a code block. "
                                       "Review the prefilled description before submitting — log lines can "
                                       "contain paths or other details you may not want in the tracker.");
        }
    }

    // ConditionalDraw row: the line count only exists while the log tail is on, so
    // the coverage guard tolerates it never being observed.
    if (d.cfg.QuickCreateCtxLogTail && d.prefsFilter.ShowSetting("editing.quick_create.log_lines")) {
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

    if (!d.prefsFilter.Active()) {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("quickcreate.prefs.note",
                                          "Items missing from the engine snapshot are skipped even when enabled."));
    }
}

} // namespace

void DrawQuickCreatePreferencesTab(UiDrawSession& d) {
    SmatchetPreferencesUiDetail::PrefsSection(d, "editing.quick_create", [&] { DrawQuickCreateSectionBody(d); });
}
