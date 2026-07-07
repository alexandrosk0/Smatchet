#include "AnnotateAnalysisUi_Internal.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetHelpMarker.h"
#include "SpreadsheetState.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <memory>
#include <string>

using namespace AnnotateInternal;

// Single definition for the extern declared in AnnotateAnalysisUi_Internal.h.
AnnotateAnalysisUi::AnnotateState* s_stateInstance = nullptr;

AnnotateAnalysisUi::AnnotateAnalysisUi() : state_(std::make_unique<AnnotateState>()) { s_stateInstance = state_.get(); }
AnnotateAnalysisUi::~AnnotateAnalysisUi() {
    // DR8: the worker thread (WorkerThreadMain) dereferences State() -> s_stateInstance on every
    // loop iteration. Cancel and join it while s_stateInstance still points at live state, BEFORE
    // clearing the pointer, so an in-flight worker never derefs null during teardown. The later
    // ~AnnotateState join then finds the thread already joined and is a no-op.
    if (state_) {
        state_->worker.Cancel.store(true, std::memory_order_release);
        if (state_->worker.Thread.joinable()) {
            state_->worker.Thread.join();
        }
    }
    s_stateInstance = nullptr;
}

void AnnotateAnalysisUi::SetAnnotatePanelOpen(bool open) { annotatePanelOpen_ = open; }

void AnnotateAnalysisUi::ServiceBackground() {
    JoinWorkerIfNeeded();
    if (!annotatePanelOpen_) {
        annotateOpenPrev_ = false;
    }
    MirrorWorkerToDisplay();
    ResetDetailAfterRunComplete();
    PollDetails();
}

void AnnotateAnalysisUi::ensureSettingsBuffersLoaded() {
    if (cfgLoaded_) {
        return;
    }
    HydrateAnnotateCfgDiskOnce();
    State().maxFramesVal = State().annotateCfg.DefaultMaxFrames;
    State().clCacheVal = State().annotateCfg.ChangelistCacheMaxEntries;
    CopyToBuffer(State().p4Exe, State().annotateCfg.P4Executable);
    CopyToBuffer(State().p4vcExe, State().annotateCfg.P4VcExecutable);
    CopyToBuffer(State().timeTpl, State().annotateCfg.TimelapseCommandTemplate);
    CopyToBuffer(State().changeTpl, State().annotateCfg.ChangeCommandTemplate);
    CopyToBuffer(State().aiUrl, State().annotateCfg.AiChatUrl);
    size_t off = 0;
    for (const auto& kw : State().annotateCfg.DefaultIgnoreKeywords) {
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
    // Path-remap edit buffers are seeded lazily by the multi-rule editor in
    // AnnotateAnalysisUi_Preferences.cpp (it re-syncs from cfg.PathRemaps whenever the
    // row count changes), so no single-rule seed is needed here.
    LogAnnotateP4PathsIfChanged("initial_load");
    cfgLoaded_ = true;
}

void AnnotateAnalysisUi::DrawAnnotatePreferencesTab(const AppController& app) {
    ensureSettingsBuffersLoaded();
    MaybeAutoselectCallstackTrackerField(app);
    MaybeAutoselectLastFoundClTrackerField(app);
    MaybeAutoselectLastOccurrencesTrackerField(app);
    ImGui::TextUnformatted("Annotate configuration (stored in smatchet_config.json).");
    ImGui::SameLine();
    SmatchetHelpMarker::RenderText("Perforce paths, ignore list, and Jira callstack source used by Annotate "
                                   "(stored in smatchet_config.json).");
    ImGui::Spacing();
    const AnnotateUiThemeColors& theme = State().annotateCfg.UiColors;
    DrawAnnotatePersistedOptionsForm(app, theme);
}

bool AnnotateRowHasNonEmptyCallstackField(const AppController& app, const CachedTicket& ticket) {
    HydrateAnnotateCfgDiskOnce();
    std::string fid = State().annotateCfg.CallstackTrackerFieldId;
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

void OpenAnnotateAnalysisForGridIssue(AppController& app, bool& showAnnotateAnalysis, SpreadsheetState& gridState,
                                      const std::string& issueKey) {
    if (issueKey.empty()) {
        return;
    }
    HydrateAnnotateCfgDiskOnce();
    MaybeAutoselectCallstackTrackerField(app);
    MaybeAutoselectLastFoundClTrackerField(app);
    MaybeAutoselectLastOccurrencesTrackerField(app);
    gridState.SetActiveIssue(issueKey);
    State().annotateStreamlinedFromGrid = true;
    State().annotatePendingAutoProcess = true;
    showAnnotateAnalysis = true;
}
