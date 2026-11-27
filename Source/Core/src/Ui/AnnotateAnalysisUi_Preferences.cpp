#include "AnnotateAnalysisUi_Internal.h"

#include "CallstackParser.h" // ApplyPathRemaps — the remap Preview
#include "ConfigManager.h"
#include "Interfaces/IAppTicketMutations.h"
#include "Logger.h"
#include "Ui/PreferencesFilter.h"
#include "SmatchetFieldRender.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"
#include "Tracker/TrackerInputNormalisePure.h"
#include "Ui/SmatchetInlineError.h"
#include "Ui/SmatchetInput.h"

#include <algorithm>
#include <memory>
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

// A path or URL is replaced whole and never carries edge whitespace: a click selects it and the
// commit trims it. A command template keeps its spaces (`trim` false).
SmatchetInput::TextOptions SettingFieldOptions(bool trim) {
    SmatchetInput::TextOptions opts;
    opts.selectAllOnFocus = true;
    opts.trimOnCommit = trim;
    return opts;
}

// One settings row: the shared text field over the form's std::string, persisted to `cfgField`
// when an edit ends with a changed value (Enter, Tab, a click away; Esc reverts and saves nothing).
void DrawSettingTextField(const char* label, std::string& text, std::string& cfgField, const char* reason, bool trim) {
    if (SmatchetInput::Text(label, text, SettingFieldOptions(trim)).committed && text != cfgField) {
        cfgField = text;
        PersistAnnotateCfg(reason);
    }
}

// A path or link field the user pastes into (#82, #128): the shared text field over the form's string. When
// the edit ends, the quotes "Copy as path" adds and stray spaces come off the text; text that cannot be used
// is explained under the field and is not saved, so the config keeps the last value that worked.
template <typename NormaliseFn>
void DrawCheckedSettingField(const char* label, std::string& text, std::string& cfgField, const char* reason,
                             NormaliseFn normalise) {
    const bool committed = SmatchetInput::Text(label, text, SettingFieldOptions(true)).committed;
    const TrackerInputNormalise::PathParse parsed = normalise(text);
    if (!parsed.Ok) {
        smatchet::ui::DrawInlineParseError(parsed.Message);
        return;
    }
    if (!committed) {
        return;
    }
    if (parsed.Rewritten) {
        text = parsed.Value;
    }
    if (text != cfgField) {
        cfgField = text;
        PersistAnnotateCfg(reason);
    }
}

void DrawJiraFieldCombo(const std::vector<TrackerField>& availableFields, const IAppTicketMutations& ticketMutations,
                        const AnnotateUiThemeColors& theme, const char* label, const char* tooltip,
                        std::string& cfgField, const char* idSuffix, const char* reason) {
    std::string preview = "(none)";
    if (!cfgField.empty()) {
        const std::shared_ptr<const TrackerField> mf = ticketMutations.FindFieldSnapshotById(cfgField);
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

// Defined below; drawn as the tail of the Perforce section. `filtering` = a Preferences search
// is narrowing the pane, so the leading rule (section chrome) is dropped.
void DrawAnnotatePathRemaps(bool filtering);

// Annotate > Analysis body: run limits, ignore list, changelist cache, AI hand-off URL.
// Perforce executables + command templates live in Connections > Perforce.
void DrawAnnotateAnalysisFields(PreferencesFilter& filter) {
    auto& cfg = State().annotateCfg;

    if (filter.ShowSetting("annotate.analysis.max_frames")) {
        ImGui::InputInt("Max frames", &State().maxFramesVal);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            State().maxFramesVal = std::max(1, std::min(500, State().maxFramesVal));
            cfg.DefaultMaxFrames = State().maxFramesVal;
            PersistAnnotateCfg("edit_max_frames");
        }
    }

    if (filter.ShowSetting("annotate.analysis.ignore_keywords")) {
        ImGui::InputTextMultiline("Ignore keywords (comma or newline)", State().ignoreBuf.data(),
                                  State().ignoreBuf.size(), ImVec2(-1, 60));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            cfg.DefaultIgnoreKeywords = SplitIgnoreKeywords(std::string(State().ignoreBuf.data()));
            PersistAnnotateCfg("edit_ignore");
        }
    }

    if (filter.ShowSetting("annotate.analysis.cache_size")) {
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
    }

    if (filter.ShowSetting("annotate.analysis.ai_chat_url")) {
        DrawCheckedSettingField("AI chat URL (optional)", State().aiUrl, cfg.AiChatUrl, "edit_aiurl",
                                TrackerInputNormalise::ValidateHttpUrl);
    }
}

// Connections > Perforce body: p4/p4vc executables, the two command templates, and the
// path-remap rules that resolve callstack frames against a local workspace.
void DrawAnnotatePerforceFields(PreferencesFilter& filter) {
    auto& cfg = State().annotateCfg;

    if (filter.ShowSetting("connections.perforce.p4_exe")) {
        DrawCheckedSettingField("p4 executable", State().p4Exe, cfg.P4Executable, "edit_p4exe",
                                TrackerInputNormalise::NormaliseExecutablePath);
    }

    if (filter.ShowSetting("connections.perforce.p4vc_exe")) {
        DrawCheckedSettingField("p4vc executable", State().p4vcExe, cfg.P4VcExecutable, "edit_p4vcexe",
                                TrackerInputNormalise::NormaliseExecutablePath);
    }

    if (filter.ShowSetting("connections.perforce.timelapse_cmd")) {
        DrawSettingTextField("Timelapse command (optional)", State().timeTpl, cfg.TimelapseCommandTemplate,
                             "edit_timelapse", false);
        ImGui::TextDisabled("Placeholders: {file} {line} {cl}");
    }

    if (filter.ShowSetting("connections.perforce.change_cmd")) {
        DrawSettingTextField("Changelist command (optional)", State().changeTpl, cfg.ChangeCommandTemplate,
                             "edit_change", false);
    }

    // One descriptor covers the whole remap-rule editor; its per-row from/to inputs are not
    // individually filterable.
    if (filter.ShowSetting("connections.perforce.path_remaps")) {
        DrawAnnotatePathRemaps(filter.Active());
    }
}

// Annotate > Tracker field mapping body: the Jira-field source combos (callstack,
// before-changelist, last-occurrences date).
void DrawAnnotateJiraFieldCombos(const std::vector<TrackerField>& availableFields,
                                 const IAppTicketMutations& ticketMutations, const AnnotateUiThemeColors& theme,
                                 PreferencesFilter& filter) {
    auto& cfg = State().annotateCfg;

    // Each combo keeps its own caption + explainer inside the gate — they describe that one
    // mapping, so a narrowed pane would be unreadable without them. Only the rule between the
    // first two blocks is section chrome.
    if (filter.ShowSetting("annotate.field_mapping.callstack_field")) {
        ImGui::TextUnformatted("Callstack from Jira");
        ImGui::TextDisabled("When set, the callstack buffer is filled from this field for the selected issue when "
                            "Annotate is shown, when the selected issue changes, or when you open Annotate for an "
                            "issue from the grid.");
        DrawJiraFieldCombo(availableFields, ticketMutations, theme, "Callstack source field",
                           "Choose which Jira field supplies callstack text for the selected issue whenever Annotate "
                           "is shown or the selection changes (including opening Annotate from the grid).",
                           cfg.CallstackTrackerFieldId, "callstacksrc", "edit_callstackfield");
    }

    if (!filter.Active()) {
        ImGui::Separator();
    }
    if (filter.ShowSetting("annotate.field_mapping.before_cl_field")) {
        ImGui::TextUnformatted("Before changelist (Jira)");
        ImGui::TextDisabled("When set, opening Annotate on an issue fills \"Before changelist\" from this field "
                            "(digits only). Autoselect matches a field named \"Last Found CL\".");
        DrawJiraFieldCombo(availableFields, ticketMutations, theme, "Before-changelist field",
                           "Jira field whose value pre-fills Before changelist when Annotate opens on an issue.",
                           cfg.LastFoundClTrackerFieldId, "lastfoundcl", "edit_lastfoundcl");
    }

    if (filter.ShowSetting("annotate.field_mapping.last_occurrences_field")) {
        ImGui::TextUnformatted("Last occurrences date (Jira)");
        ImGui::TextDisabled("When set, opening Annotate on an issue pre-fills the \"or day\" date from this field "
                            "(ISO or parseable date). Otherwise the date stays empty. Autoselect matches \"Last "
                            "Occurrences\" or \"Last Occurances\".");
        DrawJiraFieldCombo(availableFields, ticketMutations, theme, "Last-occurrences date field",
                           "Jira date field used to seed the Before-changelist day picker when Annotate opens.",
                           cfg.LastOccurrencesTrackerFieldId, "lastoccurrences", "edit_lastocc");
    }
}

namespace {

// The remap rules tried on a sample path (#131): what a crash-report frame with that path would resolve to.
// The sample is not saved. An unchanged path says so, since that is how a rule that never applies shows.
void DrawAnnotateRemapPreview(const std::vector<PathRemapRule>& remaps) {
    ImGui::SetNextItemWidth(220.f);
    SmatchetInput::Text(SmatchetLocalization::T("annotate.remap.preview", "Preview"), State().remapPreviewSample,
                        SettingFieldOptions(false));
    std::string& shown = State().remapPreviewShown;
    shown.clear();
    // A path pasted from a log may carry edge whitespace that a parsed frame path does not.
    const std::string sample = TrimCopyAsciiWhitespace(State().remapPreviewSample);
    if (sample.empty()) {
        return;
    }
    const std::string remapped = ApplyPathRemaps(sample, remaps);
    shown =
        remapped != sample
            ? std::string("-> ") + remapped
            : std::string(SmatchetLocalization::T("annotate.remap.preview_no_change", "No rule changes this path."));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", shown.c_str());
}

} // namespace

// Path-remap rules editor (per-row from/to + add/remove). Keeps the per-row edit buffers in
// lockstep with the rule vector. Drawn as the tail of Connections > Perforce.
void DrawAnnotatePathRemaps(bool filtering) {
    auto& cfg = State().annotateCfg;
    if (!filtering) {
        ImGui::Separator();
    }
    ImGui::TextUnformatted("Path remaps");
    ImGui::TextDisabled("Rewrite source-path prefixes before resolving callstack frames. Longest matching "
                        "prefix wins; add rules in any order.");
    auto& remaps = cfg.PathRemaps;
    // Keep the per-row edit buffers in lockstep with the rule vector — this seeds them after
    // a config load (buffers empty, rules non-empty) and re-syncs after add/remove.
    if (State().remapBufs.size() != remaps.size()) {
        State().remapBufs.assign(remaps.size(), RemapEditBuf{});
        for (size_t i = 0; i < remaps.size(); ++i) {
            State().remapBufs[i].from = remaps[i].FromPrefix;
            State().remapBufs[i].to = remaps[i].ToPrefix;
        }
    }
    // Row whose "from" field takes the caret this frame: the row Add path remap just added, or the next row
    // Enter in a filled last "to" field adds. UI thread only, like the rest of this editor.
    static int s_focusFromRow = -1;
    int removeIdx = -1;
    bool addRow = false;
    for (size_t i = 0; i < remaps.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(220.f);
        if (s_focusFromRow == static_cast<int>(i)) {
            ImGui::SetKeyboardFocusHere();
            s_focusFromRow = -1;
        }
        // Edge whitespace is trimmed on commit (ApplyPathRemaps ignores it anyway); a trailing separator is kept.
        const bool fromDirty =
            SmatchetInput::Text("from", State().remapBufs[i].from, SettingFieldOptions(true)).committed &&
            State().remapBufs[i].from != remaps[i].FromPrefix;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.f);
        const SmatchetInput::TextResult toEdit =
            SmatchetInput::Text("to", State().remapBufs[i].to, SettingFieldOptions(true));
        const bool toDirty = toEdit.committed && State().remapBufs[i].to != remaps[i].ToPrefix;
        // Enter in the last row's "to", with both halves filled, starts the next rule.
        if (toEdit.enterCommitted && i + 1 == remaps.size() && !State().remapBufs[i].from.empty() &&
            !State().remapBufs[i].to.empty()) {
            addRow = true;
        }
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
        addRow = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", SmatchetLocalization::T("annotate.remap.enter_adds_hint",
                                                        "Enter in the last row's \"to\" field adds the next rule"));
    }
    if (addRow) {
        remaps.push_back(PathRemapRule{});
        State().remapBufs.push_back(RemapEditBuf{});
        s_focusFromRow = static_cast<int>(remaps.size()) - 1;
        PersistAnnotateCfg("add_remap");
    }
    DrawAnnotateRemapPreview(remaps);
}

#if defined(SMATCHET_BUILD_UI_TESTS)
namespace {
std::vector<PathRemapRule> g_uiTestRemapStash;
} // namespace
#endif

// Annotate > Colors body. The owning PrefsSection draws the header, so the rows sit bare.
void DrawAnnotateColors(PreferencesFilter& filter) {
    auto& cfg = State().annotateCfg;
    auto colorRow = [&filter](const char* settingId, const char* label, float* c) {
        if (!filter.ShowSetting(settingId)) {
            return;
        }
        ImGui::ColorEdit4(label, c, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // Off-thread the write like every sibling persist in this file (#565): the
            // synchronous ConfigManager::SaveAnnotateAnalysis was the lone UI-thread blocker
            // here. PersistAnnotateCfg routes through the coalescing ConfigSaveWorker.
            PersistAnnotateCfg("edit_color");
        }
    };
    colorRow("annotate.colors.status_info", "Status info", cfg.UiColors.StatusInfo);
    colorRow("annotate.colors.status_error", "Status error", cfg.UiColors.StatusError);
    colorRow("annotate.colors.status_warning", "Status warning", cfg.UiColors.StatusWarning);
    colorRow("annotate.colors.find_highlight", "Find highlight", cfg.UiColors.FindHighlight);
    colorRow("annotate.colors.text_disabled", "Text disabled", cfg.UiColors.TextDisabled);
    colorRow("annotate.colors.import_existing", "Import existing", cfg.UiColors.ImportExisting);
    colorRow("annotate.colors.cl_tooltip_title", "CL tooltip title", cfg.UiColors.ClTooltipTitle);
}

} // namespace AnnotateInternal

#if defined(SMATCHET_BUILD_UI_TESTS)
// Bucket-E seams for the path-remap keyboard flow (tests/ui/gesture_keys.test.cpp). Begin stashes the live
// rules and empties the editor; End puts them back and saves, so a test run leaves the config as it found it.
extern "C" void SmatchetUiTestDrawAnnotatePathRemaps() { AnnotateInternal::DrawAnnotatePathRemaps(false); }

extern "C" void SmatchetUiTestAnnotateRemapsBegin() {
    AnnotateInternal::g_uiTestRemapStash = State().annotateCfg.PathRemaps;
    State().annotateCfg.PathRemaps.clear();
    State().remapBufs.clear();
}

extern "C" void SmatchetUiTestAnnotateRemapsEnd() {
    State().annotateCfg.PathRemaps = AnnotateInternal::g_uiTestRemapStash;
    State().remapBufs.clear(); // re-seeded from the rules on the next draw
    State().remapPreviewSample.clear();
    AnnotateInternal::g_uiTestRemapStash.clear();
    AnnotateInternal::PersistAnnotateCfg("ui_test_restore");
}

extern "C" int SmatchetUiTestAnnotateRemapCount() { return static_cast<int>(State().annotateCfg.PathRemaps.size()); }

extern "C" const char* SmatchetUiTestAnnotateRemapPreview() { return State().remapPreviewShown.c_str(); }

extern "C" const char* SmatchetUiTestAnnotateRemapTo(int i) {
    const auto& remaps = State().annotateCfg.PathRemaps;
    return i >= 0 && static_cast<std::size_t>(i) < remaps.size() ? remaps[static_cast<std::size_t>(i)].ToPrefix.c_str()
                                                                 : "";
}

extern "C" const char* SmatchetUiTestAnnotateRemapFrom(int i) {
    const auto& remaps = State().annotateCfg.PathRemaps;
    return i >= 0 && static_cast<std::size_t>(i) < remaps.size()
               ? remaps[static_cast<std::size_t>(i)].FromPrefix.c_str()
               : "";
}
#endif
