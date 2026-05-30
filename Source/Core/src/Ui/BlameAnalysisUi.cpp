#include "BlameAnalysisUi_Internal.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SpreadsheetState.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <memory>
#include <string>

using namespace BlameInternal;

// Single definition for the extern declared in BlameAnalysisUi_Internal.h.
BlameAnalysisUi::BlameState* s_stateInstance = nullptr;

BlameAnalysisUi::BlameAnalysisUi() : state_(std::make_unique<BlameState>()) { s_stateInstance = state_.get(); }
BlameAnalysisUi::~BlameAnalysisUi() { s_stateInstance = nullptr; }

void BlameAnalysisUi::SetBlamePanelOpen(bool open) { blamePanelOpen_ = open; }

void BlameAnalysisUi::ServiceBackground() {
    JoinWorkerIfNeeded();
    if (!blamePanelOpen_) {
        blameOpenPrev_ = false;
    }
    MirrorWorkerToDisplay();
    ResetDetailAfterRunComplete();
    PollDetails();
}

void BlameAnalysisUi::ensureSettingsBuffersLoaded() {
    if (cfgLoaded_) {
        return;
    }
    HydrateBlameCfgDiskOnce();
    State().maxFramesVal = State().blameCfg.DefaultMaxFrames;
    CopyToBuffer(State().p4Exe, State().blameCfg.P4Executable);
    CopyToBuffer(State().p4vcExe, State().blameCfg.P4VcExecutable);
    CopyToBuffer(State().timeTpl, State().blameCfg.TimelapseCommandTemplate);
    CopyToBuffer(State().changeTpl, State().blameCfg.ChangeCommandTemplate);
    CopyToBuffer(State().aiUrl, State().blameCfg.AiChatUrl);
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
    LogBlameP4PathsIfChanged("initial_load");
    cfgLoaded_ = true;
}

void BlameAnalysisUi::DrawBlamePreferencesTab(const AppController& app) {
    ensureSettingsBuffersLoaded();
    MaybeAutoselectCallstackTrackerField(app);
    MaybeAutoselectLastFoundClTrackerField(app);
    MaybeAutoselectLastOccurrencesTrackerField(app);
    ImGui::TextWrapped("Perforce paths, ignore list, and Jira callstack source used by Annotate (stored in "
                       "smatchet_config.json).");
    ImGui::Spacing();
    const BlameUiThemeColors& theme = State().blameCfg.UiColors;
    DrawBlamePersistedOptionsForm(app, theme);
}

bool BlameRowHasNonEmptyCallstackField(const AppController& app, const CachedTicket& ticket) {
    HydrateBlameCfgDiskOnce();
    std::string fid = State().blameCfg.CallstackTrackerFieldId;
    if (fid.empty()) {
        const auto& fields = app.GetAvailableFields();
        const auto it = std::find_if(fields.begin(), fields.end(),
                                     [](const TrackerField& f) { return ToLowerAsciiCopy(f.Name) == "callstack"; });
        if (it == fields.end()) {
            return false;
        }
        fid = it->Id;
    }
    return !ticket.GetFieldValue(fid).empty();
}

void OpenBlameAnalysisForGridIssue(AppController& app, bool& showBlameAnalysis, SpreadsheetState& gridState,
                                   const std::string& issueKey) {
    if (issueKey.empty()) {
        return;
    }
    HydrateBlameCfgDiskOnce();
    MaybeAutoselectCallstackTrackerField(app);
    MaybeAutoselectLastFoundClTrackerField(app);
    MaybeAutoselectLastOccurrencesTrackerField(app);
    gridState.SetActiveIssue(issueKey);
    State().blameStreamlinedFromGrid = true;
    State().blamePendingAutoProcess = true;
    showBlameAnalysis = true;
}
