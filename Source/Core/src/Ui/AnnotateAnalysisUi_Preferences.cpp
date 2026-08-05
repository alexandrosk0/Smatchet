#include "AnnotateAnalysisUi_Internal.h"

#include "ConfigManager.h"
#include "Interfaces/IAppTicketMutations.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <string>

namespace AnnotateInternal {

namespace {

void PersistAnnotateCfg(const char* reason) {
    // Off-thread the file write (JSON encode + atomic replace) so prefs edits never block the
    // UI thread; the UI-thread-only side-effects (path-change log + callstack-field hint) stay sync.
    ScheduleAnnotateConfigSaveDetached(State().annotateCfg);
    LogAnnotateP4PathsIfChanged(reason);
    SetCallstackFieldIdHint(State().annotateCfg.CallstackTrackerFieldId);
}

template <size_t N> bool CommitTextField(const char (&buf)[N], std::string& cfgField, const char* reason) {
    if (!ImGui::IsItemDeactivatedAfterEdit()) {
        return false;
    }
    cfgField.assign(buf);
    PersistAnnotateCfg(reason);
    return true;
}

void DrawJiraFieldCombo(const std::vector<TrackerField>& availableFields, const IAppTicketMutations& ticketMutations,
                        const AnnotateUiThemeColors& theme, const char* label, const char* tooltip,
                        std::string& cfgField, const char* idSuffix, const char* reason) {
    std::string preview = "(none)";
    if (!cfgField.empty()) {
        const TrackerField* mf = ticketMutations.FindFieldById(cfgField);
        preview = (mf && !mf->Name.empty()) ? (mf->Name + " (" + mf->Id + ")") : cfgField;
    }
    const std::string comboId = std::string(label) + "##" + idSuffix;
    PushAnnotateLinkButtonColors(theme);
    const bool open = ImGui::BeginCombo(comboId.c_str(), preview.c_str());
    PopAnnotateLinkButtonColors();
    if (!open) {
        if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", tooltip);
        }
        return;
    }
    PushAnnotateLinkTextOnly(theme);
    if (ImGui::Selectable("(none)", cfgField.empty())) {
        cfgField.clear();
        PersistAnnotateCfg(reason);
    }
    PopAnnotateLinkTextOnly();
    for (const auto& f : availableFields) {
        const bool sel = (f.Id == cfgField);
        const std::string lbl = f.Name.empty() ? f.Id : (f.Name + " (" + f.Id + ")");
        const std::string lblWithId = lbl + "##" + idSuffix + "_" + f.Id;
        PushAnnotateLinkTextOnly(theme);
        if (ImGui::SelectableRaw(lblWithId.c_str(), sel)) {
            cfgField = f.Id;
            PersistAnnotateCfg(reason);
        }
        PopAnnotateLinkTextOnly();
    }
    ImGui::EndCombo();
}

} // namespace

// Defined below; drawn as the tail of the Perforce section.
void DrawAnnotatePathRemaps();

// Annotate > Analysis body: run limits, ignore list, changelist cache, AI hand-off URL.
// Perforce executables + command templates live in Connections > Perforce.
void DrawAnnotateAnalysisFields() {
    auto& cfg = State().annotateCfg;

    ImGui::InputInt("Max frames", &State().maxFramesVal);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        State().maxFramesVal = std::max(1, std::min(500, State().maxFramesVal));
        cfg.DefaultMaxFrames = State().maxFramesVal;
        PersistAnnotateCfg("edit_max_frames");
    }

    ImGui::InputTextMultiline("Ignore keywords (comma or newline)", State().ignoreBuf.data(), State().ignoreBuf.size(),
                              ImVec2(-1, 60));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        cfg.DefaultIgnoreKeywords = SplitIgnoreKeywords(std::string(State().ignoreBuf.data()));
        PersistAnnotateCfg("edit_ignore");
    }

    ImGui::InputInt("Changelist cache size", &State().clCacheVal);
    const bool clCacheDirty = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Max changelist-describe entries cached during annotate (clamped 16..8192).");
    }
    if (clCacheDirty) {
        State().clCacheVal = std::max(16, std::min(8192, State().clCacheVal));
        cfg.ChangelistCacheMaxEntries = State().clCacheVal;
        PersistAnnotateCfg("edit_clcache");
    }

    ImGui::InputText("AI chat URL (optional)", State().aiUrl, sizeof(State().aiUrl));
    CommitTextField(State().aiUrl, cfg.AiChatUrl, "edit_aiurl");
}

// Connections > Perforce body: p4/p4vc executables, the two command templates, and the
// path-remap rules that resolve callstack frames against a local workspace.
void DrawAnnotatePerforceFields() {
    auto& cfg = State().annotateCfg;

    ImGui::InputText("p4 executable", State().p4Exe, sizeof(State().p4Exe));
    CommitTextField(State().p4Exe, cfg.P4Executable, "edit_p4exe");

    ImGui::InputText("p4vc executable", State().p4vcExe, sizeof(State().p4vcExe));
    CommitTextField(State().p4vcExe, cfg.P4VcExecutable, "edit_p4vcexe");

    ImGui::InputText("Timelapse command (optional)", State().timeTpl, sizeof(State().timeTpl));
    CommitTextField(State().timeTpl, cfg.TimelapseCommandTemplate, "edit_timelapse");
    ImGui::TextDisabled("Placeholders: {file} {line} {cl}");

    ImGui::InputText("Changelist command (optional)", State().changeTpl, sizeof(State().changeTpl));
    CommitTextField(State().changeTpl, cfg.ChangeCommandTemplate, "edit_change");

    DrawAnnotatePathRemaps();
}

// Annotate > Tracker field mapping body: the Jira-field source combos (callstack,
// before-changelist, last-occurrences date).
void DrawAnnotateJiraFieldCombos(const std::vector<TrackerField>& availableFields,
                                 const IAppTicketMutations& ticketMutations, const AnnotateUiThemeColors& theme) {
    auto& cfg = State().annotateCfg;

    ImGui::TextUnformatted("Callstack from Jira");
    ImGui::TextDisabled("When set, the callstack buffer is filled from this field for the selected issue when "
                        "Annotate is shown, when the selected issue changes, or when you open Annotate for an "
                        "issue from the grid.");
    DrawJiraFieldCombo(availableFields, ticketMutations, theme, "Callstack source field",
                       "Choose which Jira field supplies callstack text for the selected issue whenever Annotate "
                       "is shown or the selection changes (including opening Annotate from the grid).",
                       cfg.CallstackTrackerFieldId, "callstacksrc", "edit_callstackfield");

    ImGui::Separator();
    ImGui::TextUnformatted("Before changelist (Jira)");
    ImGui::TextDisabled("When set, opening Annotate on an issue fills \"Before changelist\" from this field (digits "
                        "only). Autoselect matches a field named \"Last Found CL\".");
    DrawJiraFieldCombo(availableFields, ticketMutations, theme, "Before-changelist field",
                       "Jira field whose value pre-fills Before changelist when Annotate opens on an issue.",
                       cfg.LastFoundClTrackerFieldId, "lastfoundcl", "edit_lastfoundcl");

    ImGui::TextUnformatted("Last occurrences date (Jira)");
    ImGui::TextDisabled("When set, opening Annotate on an issue pre-fills the \"or day\" date from this field (ISO "
                        "or parseable date). Otherwise the date stays empty. Autoselect matches \"Last "
                        "Occurrences\" or \"Last Occurances\".");
    DrawJiraFieldCombo(availableFields, ticketMutations, theme, "Last-occurrences date field",
                       "Jira date field used to seed the Before-changelist day picker when Annotate opens.",
                       cfg.LastOccurrencesTrackerFieldId, "lastoccurrences", "edit_lastocc");
}

// Path-remap rules editor (per-row from/to + add/remove). Keeps the per-row edit buffers in
// lockstep with the rule vector. Drawn as the tail of Connections > Perforce.
void DrawAnnotatePathRemaps() {
    auto& cfg = State().annotateCfg;
    ImGui::Separator();
    ImGui::TextUnformatted("Path remaps");
    ImGui::TextDisabled("Rewrite source-path prefixes before resolving callstack frames. Longest matching "
                        "prefix wins; add rules in any order.");
    auto& remaps = cfg.PathRemaps;
    // Keep the per-row edit buffers in lockstep with the rule vector — this seeds them after
    // a config load (buffers empty, rules non-empty) and re-syncs after add/remove.
    if (State().remapBufs.size() != remaps.size()) {
        State().remapBufs.assign(remaps.size(), RemapEditBuf{});
        for (size_t i = 0; i < remaps.size(); ++i) {
            CopyToBuffer(State().remapBufs[i].from, remaps[i].FromPrefix);
            CopyToBuffer(State().remapBufs[i].to, remaps[i].ToPrefix);
        }
    }
    int removeIdx = -1;
    for (size_t i = 0; i < remaps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(220.f);
        ImGui::InputText("from", State().remapBufs[i].from, sizeof(State().remapBufs[i].from));
        const bool fromDirty = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.f);
        ImGui::InputText("to", State().remapBufs[i].to, sizeof(State().remapBufs[i].to));
        const bool toDirty = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        const bool remove = ImGui::Button("Remove");
        ImGui::PopID();
        if (fromDirty || toDirty) {
            remaps[i].FromPrefix = State().remapBufs[i].from;
            remaps[i].ToPrefix = State().remapBufs[i].to;
            PersistAnnotateCfg("edit_remap");
        }
        if (remove) {
            removeIdx = static_cast<int>(i);
        }
    }
    if (removeIdx >= 0) {
        remaps.erase(remaps.begin() + removeIdx);
        State().remapBufs.erase(State().remapBufs.begin() + removeIdx);
        PersistAnnotateCfg("remove_remap");
    }
    if (ImGui::Button("Add path remap")) {
        remaps.push_back(PathRemapRule{});
        State().remapBufs.push_back(RemapEditBuf{});
        PersistAnnotateCfg("add_remap");
    }
}

// Annotate > Colors body. The owning PrefsSection draws the header, so the rows sit bare.
void DrawAnnotateColors() {
    auto& cfg = State().annotateCfg;
    auto colorRow = [](const char* label, float* c) {
        ImGui::ColorEdit4(label, c, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // Off-thread the write like every sibling persist in this file (#565): the
            // synchronous ConfigManager::SaveAnnotateAnalysis was the lone UI-thread blocker
            // here. PersistAnnotateCfg routes through the coalescing ConfigSaveWorker.
            PersistAnnotateCfg("edit_color");
        }
    };
    colorRow("Status info", cfg.UiColors.StatusInfo);
    colorRow("Status error", cfg.UiColors.StatusError);
    colorRow("Status warning", cfg.UiColors.StatusWarning);
    colorRow("Find highlight", cfg.UiColors.FindHighlight);
    colorRow("Text disabled", cfg.UiColors.TextDisabled);
    colorRow("Import existing", cfg.UiColors.ImportExisting);
    colorRow("CL tooltip title", cfg.UiColors.ClTooltipTitle);
}

} // namespace AnnotateInternal
