#include "AnnotateAnalysisUi_Internal.h"

#include "CachedTicketTypes.h"
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

void AnnotateAnalysisUi::DrawAnnotatePrefsSection(AnnotatePrefsSection section,
                                                 const std::vector<TrackerField>& availableFields,
                                                 const IAppTicketMutations& ticketMutations) {
    // Every section call re-runs hydration + autoselect. Both are idempotent latches, so the
    // per-section cost is a bool test once the first drawn section has warmed them.
    ensureSettingsBuffersLoaded();
    MaybeAutoselectCallstackTrackerField(availableFields);
    MaybeAutoselectLastFoundClTrackerField(availableFields);
    MaybeAutoselectLastOccurrencesTrackerField(availableFields);
    switch (section) {
    case AnnotatePrefsSection::Analysis:
        DrawAnnotateAnalysisFields();
        break;
    case AnnotatePrefsSection::FieldMapping:
        DrawAnnotateJiraFieldCombos(availableFields, ticketMutations, State().annotateCfg.UiColors);
        break;
    case AnnotatePrefsSection::Colors:
        DrawAnnotateColors();
        break;
    case AnnotatePrefsSection::Perforce:
        DrawAnnotatePerforceFields();
        break;
    }
}

bool AnnotateRowHasNonEmptyCallstackField(const std::vector<TrackerField>& availableFields,
                                          const CachedTicket& ticket) {
    HydrateAnnotateCfgDiskOnce();
    std::string fid = State().annotateCfg.CallstackTrackerFieldId;
    if (fid.empty()) {
        const auto it = std::find_if(availableFields.begin(), availableFields.end(),
                                     [](const TrackerField& f) { return ToLowerAsciiCopy(f.Name) == "callstack"; });
        if (it == availableFields.end()) {
            return false;
        }
        fid = it->Id;
    }
    return !ticket.GetFieldValue(fid).empty();
}

void OpenAnnotateAnalysisForGridIssue(const std::vector<TrackerField>& availableFields, bool& showAnnotateAnalysis,
                                      SpreadsheetState& gridState, const std::string& issueKey) {
    if (issueKey.empty()) {
        return;
    }
    HydrateAnnotateCfgDiskOnce();
    MaybeAutoselectCallstackTrackerField(availableFields);
    MaybeAutoselectLastFoundClTrackerField(availableFields);
    MaybeAutoselectLastOccurrencesTrackerField(availableFields);
    gridState.SetActiveIssue(issueKey);
    State().annotateStreamlinedFromGrid = true;
    State().annotatePendingAutoProcess = true;
    showAnnotateAnalysis = true;
}
