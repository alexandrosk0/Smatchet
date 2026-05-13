#include "BlameAnalysisUi_Internal.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "TrackerFieldSchema.h"

#include <string>

namespace BlameInternal {

void DrawBlamePersistedOptionsForm(const AppController& app, const BlameUiThemeColors& theme) {
    ImGui::InputInt("Max frames", &State().maxFramesVal);
    if (State().maxFramesVal < 1) {
        State().maxFramesVal = 1;
    }
    if (State().maxFramesVal > 500) {
        State().maxFramesVal = 500;
    }
    ImGui::InputTextMultiline("Ignore keywords (comma or newline)", State().ignoreBuf.data(), State().ignoreBuf.size(),
                              ImVec2(-1, 60));
    ImGui::InputText("P4 executable", State().p4Exe, sizeof(State().p4Exe));
    ImGui::InputText("p4vc executable", State().p4vcExe, sizeof(State().p4vcExe));
    ImGui::InputText("Timelapse cmd (optional)", State().timeTpl, sizeof(State().timeTpl));
    ImGui::TextDisabled("Placeholders: {file} {line} {cl}");
    ImGui::InputText("Changelist cmd (optional)", State().changeTpl, sizeof(State().changeTpl));
    ImGui::InputText("AI chat URL (optional)", State().aiUrl, sizeof(State().aiUrl));
    ImGui::Separator();
    ImGui::TextUnformatted("Callstack from Jira");
    ImGui::TextDisabled("When set, the callstack buffer is filled from this field for the selected issue when you "
                        "open Blame Analysis, when the selected issue changes, or when you open Blame Analysis for an "
                        "issue from the grid.");
    {
        std::string comboPreview = "(none)";
        if (!State().blameCfg.CallstackTrackerFieldId.empty()) {
            const TrackerField* mf = app.FindFieldById(State().blameCfg.CallstackTrackerFieldId);
            if (mf && !mf->Name.empty()) {
                comboPreview = mf->Name + " (" + mf->Id + ")";
            } else {
                comboPreview = State().blameCfg.CallstackTrackerFieldId;
            }
        }
        PushBlameLinkButtonColors(theme);
        const bool TrackerFieldComboOpen = ImGui::BeginCombo("Jira field##callstacksrc", comboPreview.c_str());
        PopBlameLinkButtonColors();
        if (!TrackerFieldComboOpen) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "Choose which Jira field supplies callstack text for the selected issue whenever Blame Analysis "
                    "is shown or the selection changes (including opening blame from the grid).");
            }
        } else {
            PushBlameLinkTextOnly(theme);
            if (ImGui::Selectable("(none)", State().blameCfg.CallstackTrackerFieldId.empty())) {
                State().blameCfg.CallstackTrackerFieldId.clear();
                SyncCallstackTrackerFieldBufFromCfg();
            }
            PopBlameLinkTextOnly();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Do not pull callstack text from Jira.");
            }
            for (const auto& f : app.GetAvailableFields()) {
                const bool sel = (f.Id == State().blameCfg.CallstackTrackerFieldId);
                const std::string lbl = f.Name.empty() ? f.Id : (f.Name + std::string(" (") + f.Id + ")");
                // Stable ## id: display string can duplicate or change when Jira renames fields; never key off it
                // alone.
                const std::string lblWithId = lbl + "##callstack_field_" + f.Id;
                PushBlameLinkTextOnly(theme);
                if (ImGui::SelectableRaw(lblWithId.c_str(), sel)) {
                    State().blameCfg.CallstackTrackerFieldId = f.Id;
                    SyncCallstackTrackerFieldBufFromCfg();
                }
                PopBlameLinkTextOnly();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "Use this Jira field as the callstack source for whichever issue is selected while Blame "
                        "Analysis is open.");
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputText("Callstack field id (optional)", State().callstackTrackerFieldBuf,
                     sizeof(State().callstackTrackerFieldBuf));
    ImGui::TextDisabled("Override or set manually (e.g. customfield_10000). Saved with Save settings.");
    ImGui::Separator();
    ImGui::TextUnformatted("Before changelist (Jira)");
    ImGui::TextDisabled("When set, opening Blame on an issue fills \"Before changelist\" from this field (digits "
                        "only). Autoselect matches a field named \"Last Found CL\".");
    {
        std::string comboPreview = "(none)";
        if (!State().blameCfg.LastFoundClTrackerFieldId.empty()) {
            const TrackerField* mf = app.FindFieldById(State().blameCfg.LastFoundClTrackerFieldId);
            if (mf && !mf->Name.empty()) {
                comboPreview = mf->Name + " (" + mf->Id + ")";
            } else {
                comboPreview = State().blameCfg.LastFoundClTrackerFieldId;
            }
        }
        PushBlameLinkButtonColors(theme);
        const bool clComboOpen = ImGui::BeginCombo("Jira field##lastfoundcl", comboPreview.c_str());
        PopBlameLinkButtonColors();
        if (!clComboOpen) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Jira field whose value pre-fills Before changelist when blame opens on an issue.");
            }
        } else {
            PushBlameLinkTextOnly(theme);
            if (ImGui::Selectable("(none)", State().blameCfg.LastFoundClTrackerFieldId.empty())) {
                State().blameCfg.LastFoundClTrackerFieldId.clear();
                SyncJiraBlameAuxFieldBufsFromCfg();
            }
            PopBlameLinkTextOnly();
            for (const auto& f : app.GetAvailableFields()) {
                const bool sel = (f.Id == State().blameCfg.LastFoundClTrackerFieldId);
                const std::string lbl = f.Name.empty() ? f.Id : (f.Name + std::string(" (") + f.Id + ")");
                const std::string lblWithId = lbl + "##lastfoundcl_field_" + f.Id;
                PushBlameLinkTextOnly(theme);
                if (ImGui::SelectableRaw(lblWithId.c_str(), sel)) {
                    State().blameCfg.LastFoundClTrackerFieldId = f.Id;
                    SyncJiraBlameAuxFieldBufsFromCfg();
                }
                PopBlameLinkTextOnly();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputText("Last Found CL field id (optional)", State().lastFoundClFieldBuf,
                     sizeof(State().lastFoundClFieldBuf));
    ImGui::TextUnformatted("Last occurrences date (Jira)");
    ImGui::TextDisabled("When set, opening Blame on an issue pre-fills the \"or day\" date from this field (ISO or "
                        "parseable date). Otherwise the date stays empty. Autoselect matches \"Last Occurrences\" or "
                        "\"Last Occurances\".");
    {
        std::string comboPreview = "(none)";
        if (!State().blameCfg.LastOccurrencesTrackerFieldId.empty()) {
            const TrackerField* mf = app.FindFieldById(State().blameCfg.LastOccurrencesTrackerFieldId);
            if (mf && !mf->Name.empty()) {
                comboPreview = mf->Name + " (" + mf->Id + ")";
            } else {
                comboPreview = State().blameCfg.LastOccurrencesTrackerFieldId;
            }
        }
        PushBlameLinkButtonColors(theme);
        const bool occComboOpen = ImGui::BeginCombo("Jira field##lastoccurrences", comboPreview.c_str());
        PopBlameLinkButtonColors();
        if (!occComboOpen) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Jira date field used to seed the Before-changelist day picker when blame opens.");
            }
        } else {
            PushBlameLinkTextOnly(theme);
            if (ImGui::Selectable("(none)", State().blameCfg.LastOccurrencesTrackerFieldId.empty())) {
                State().blameCfg.LastOccurrencesTrackerFieldId.clear();
                SyncJiraBlameAuxFieldBufsFromCfg();
            }
            PopBlameLinkTextOnly();
            for (const auto& f : app.GetAvailableFields()) {
                const bool sel = (f.Id == State().blameCfg.LastOccurrencesTrackerFieldId);
                const std::string lbl = f.Name.empty() ? f.Id : (f.Name + std::string(" (") + f.Id + ")");
                const std::string lblWithId = lbl + "##lastocc_field_" + f.Id;
                PushBlameLinkTextOnly(theme);
                if (ImGui::SelectableRaw(lblWithId.c_str(), sel)) {
                    State().blameCfg.LastOccurrencesTrackerFieldId = f.Id;
                    SyncJiraBlameAuxFieldBufsFromCfg();
                }
                PopBlameLinkTextOnly();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputText("Last occurrences field id (optional)", State().lastOccurrencesFieldBuf,
                     sizeof(State().lastOccurrencesFieldBuf));
    ImGui::TextDisabled("Saved with Save settings.");
    ImGui::InputText("Path remap from", State().remapFrom, sizeof(State().remapFrom));
    ImGui::InputText("Path remap to", State().remapTo, sizeof(State().remapTo));
    PushBlameLinkButtonColors(theme);
    if (ImGui::Button("Save settings")) {
        State().blameCfg.P4Executable = State().p4Exe;
        State().blameCfg.P4VcExecutable = State().p4vcExe;
        State().blameCfg.TimelapseCommandTemplate = State().timeTpl;
        State().blameCfg.ChangeCommandTemplate = State().changeTpl;
        State().blameCfg.AiChatUrl = State().aiUrl;
        State().blameCfg.DefaultMaxFrames = State().maxFramesVal;
        State().blameCfg.CallstackTrackerFieldId.assign(State().callstackTrackerFieldBuf);
        State().blameCfg.LastFoundClTrackerFieldId.assign(State().lastFoundClFieldBuf);
        State().blameCfg.LastOccurrencesTrackerFieldId.assign(State().lastOccurrencesFieldBuf);
        State().blameCfg.PathRemaps.clear();
        if (State().remapFrom[0] != '\0') {
            State().blameCfg.PathRemaps.push_back({State().remapFrom, State().remapTo});
        }
        State().blameCfg.DefaultIgnoreKeywords = SplitIgnoreKeywords(std::string(State().ignoreBuf.data()));
        ConfigManager::SaveBlameAnalysis(State().blameCfg);
        SyncCallstackTrackerFieldBufFromCfg();
        SyncJiraBlameAuxFieldBufsFromCfg();
        LogBlameP4PathsIfChanged("save_settings");
        State().lastUiStatus = "Blame settings saved.";
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Write blame options (P4 paths, Jira field, remaps, etc.) to smatchet_config.json.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload settings")) {
        State().blameCfg = ConfigManager::LoadBlameAnalysis();
        CopyToBuffer(State().p4Exe, State().blameCfg.P4Executable);
        CopyToBuffer(State().p4vcExe, State().blameCfg.P4VcExecutable);
        CopyToBuffer(State().timeTpl, State().blameCfg.TimelapseCommandTemplate);
        CopyToBuffer(State().changeTpl, State().blameCfg.ChangeCommandTemplate);
        CopyToBuffer(State().aiUrl, State().blameCfg.AiChatUrl);
        State().maxFramesVal = State().blameCfg.DefaultMaxFrames;
        size_t off = 0;
        for (const auto& kw : State().blameCfg.DefaultIgnoreKeywords) {
            if (off + kw.size() + 2 >= State().ignoreBuf.size()) {
                break;
            }
            if (off > 0) {
                State().ignoreBuf[off++] = '\n';
            }
            for (char c : kw) {
                if (off + 1 >= State().ignoreBuf.size()) {
                    break;
                }
                State().ignoreBuf[off++] = static_cast<char>(c);
            }
        }
        if (off < State().ignoreBuf.size()) {
            State().ignoreBuf[off] = '\0';
        }
        if (State().blameCfg.PathRemaps.empty()) {
            State().remapFrom[0] = '\0';
            State().remapTo[0] = '\0';
        } else {
            CopyToBuffer(State().remapFrom, State().blameCfg.PathRemaps[0].FromPrefix);
            CopyToBuffer(State().remapTo, State().blameCfg.PathRemaps[0].ToPrefix);
        }
        SyncCallstackTrackerFieldBufFromCfg();
        SyncJiraBlameAuxFieldBufsFromCfg();
        LogBlameP4PathsIfChanged("reload_settings");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Discard unsaved edits and reload from smatchet_config.json.");
    }
    PopBlameLinkButtonColors();
}

} // namespace BlameInternal
