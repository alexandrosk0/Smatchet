#include "BlameAnalysisUi_Internal.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <string>

namespace BlameInternal {

namespace {

void PersistBlameCfg(const char* reason) {
    ConfigManager::SaveBlameAnalysis(State().blameCfg);
    LogBlameP4PathsIfChanged(reason);
    SetCallstackFieldIdHint(State().blameCfg.CallstackTrackerFieldId);
}

template <size_t N> bool CommitTextField(const char (&buf)[N], std::string& cfgField, const char* reason) {
    if (!ImGui::IsItemDeactivatedAfterEdit()) {
        return false;
    }
    cfgField.assign(buf);
    PersistBlameCfg(reason);
    return true;
}

void DrawJiraFieldCombo(const AppController& app, const BlameUiThemeColors& theme, const char* label,
                        const char* tooltip, std::string& cfgField, const char* idSuffix, const char* reason) {
    std::string preview = "(none)";
    if (!cfgField.empty()) {
        const TrackerField* mf = app.FindFieldById(cfgField);
        preview = (mf && !mf->Name.empty()) ? (mf->Name + " (" + mf->Id + ")") : cfgField;
    }
    const std::string comboId = std::string(label) + "##" + idSuffix;
    PushBlameLinkButtonColors(theme);
    const bool open = ImGui::BeginCombo(comboId.c_str(), preview.c_str());
    PopBlameLinkButtonColors();
    if (!open) {
        if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", tooltip);
        }
        return;
    }
    PushBlameLinkTextOnly(theme);
    if (ImGui::Selectable("(none)", cfgField.empty())) {
        cfgField.clear();
        PersistBlameCfg(reason);
    }
    PopBlameLinkTextOnly();
    for (const auto& f : app.GetAvailableFields()) {
        const bool sel = (f.Id == cfgField);
        const std::string lbl = f.Name.empty() ? f.Id : (f.Name + " (" + f.Id + ")");
        const std::string lblWithId = lbl + "##" + idSuffix + "_" + f.Id;
        PushBlameLinkTextOnly(theme);
        if (ImGui::SelectableRaw(lblWithId.c_str(), sel)) {
            cfgField = f.Id;
            PersistBlameCfg(reason);
        }
        PopBlameLinkTextOnly();
    }
    ImGui::EndCombo();
}

} // namespace

void DrawBlamePersistedOptionsForm(const AppController& app, const BlameUiThemeColors& theme) {
    auto& cfg = State().blameCfg;

    ImGui::InputInt("Max frames", &State().maxFramesVal);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        State().maxFramesVal = std::max(1, std::min(500, State().maxFramesVal));
        cfg.DefaultMaxFrames = State().maxFramesVal;
        PersistBlameCfg("edit_max_frames");
    }

    ImGui::InputTextMultiline("Ignore keywords (comma or newline)", State().ignoreBuf.data(), State().ignoreBuf.size(),
                              ImVec2(-1, 60));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        cfg.DefaultIgnoreKeywords = SplitIgnoreKeywords(std::string(State().ignoreBuf.data()));
        PersistBlameCfg("edit_ignore");
    }

    ImGui::InputText("P4 executable", State().p4Exe, sizeof(State().p4Exe));
    CommitTextField(State().p4Exe, cfg.P4Executable, "edit_p4exe");

    ImGui::InputText("p4vc executable", State().p4vcExe, sizeof(State().p4vcExe));
    CommitTextField(State().p4vcExe, cfg.P4VcExecutable, "edit_p4vcexe");

    ImGui::InputText("Timelapse cmd (optional)", State().timeTpl, sizeof(State().timeTpl));
    CommitTextField(State().timeTpl, cfg.TimelapseCommandTemplate, "edit_timelapse");
    ImGui::TextDisabled("Placeholders: {file} {line} {cl}");

    ImGui::InputText("Changelist cmd (optional)", State().changeTpl, sizeof(State().changeTpl));
    CommitTextField(State().changeTpl, cfg.ChangeCommandTemplate, "edit_change");

    ImGui::InputText("AI chat URL (optional)", State().aiUrl, sizeof(State().aiUrl));
    CommitTextField(State().aiUrl, cfg.AiChatUrl, "edit_aiurl");

    ImGui::Separator();
    ImGui::TextUnformatted("Callstack from Jira");
    ImGui::TextDisabled("When set, the callstack buffer is filled from this field for the selected issue when "
                        "Annotate is shown, when the selected issue changes, or when you open Annotate for an "
                        "issue from the grid.");
    DrawJiraFieldCombo(app, theme, "Jira field",
                       "Choose which Jira field supplies callstack text for the selected issue whenever Annotate "
                       "is shown or the selection changes (including opening Annotate from the grid).",
                       cfg.CallstackTrackerFieldId, "callstacksrc", "edit_callstackfield");

    ImGui::Separator();
    ImGui::TextUnformatted("Before changelist (Jira)");
    ImGui::TextDisabled("When set, opening Annotate on an issue fills \"Before changelist\" from this field (digits "
                        "only). Autoselect matches a field named \"Last Found CL\".");
    DrawJiraFieldCombo(app, theme, "Jira field",
                       "Jira field whose value pre-fills Before changelist when Annotate opens on an issue.",
                       cfg.LastFoundClTrackerFieldId, "lastfoundcl", "edit_lastfoundcl");

    ImGui::TextUnformatted("Last occurrences date (Jira)");
    ImGui::TextDisabled("When set, opening Annotate on an issue pre-fills the \"or day\" date from this field (ISO "
                        "or parseable date). Otherwise the date stays empty. Autoselect matches \"Last "
                        "Occurrences\" or \"Last Occurances\".");
    DrawJiraFieldCombo(app, theme, "Jira field",
                       "Jira date field used to seed the Before-changelist day picker when Annotate opens.",
                       cfg.LastOccurrencesTrackerFieldId, "lastoccurrences", "edit_lastocc");

    ImGui::InputText("Path remap from", State().remapFrom, sizeof(State().remapFrom));
    const bool remapFromDirty = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::InputText("Path remap to", State().remapTo, sizeof(State().remapTo));
    const bool remapToDirty = ImGui::IsItemDeactivatedAfterEdit();
    if (remapFromDirty || remapToDirty) {
        cfg.PathRemaps.clear();
        if (State().remapFrom[0] != '\0') {
            cfg.PathRemaps.push_back({State().remapFrom, State().remapTo});
        }
        PersistBlameCfg("edit_remap");
    }
}

} // namespace BlameInternal
