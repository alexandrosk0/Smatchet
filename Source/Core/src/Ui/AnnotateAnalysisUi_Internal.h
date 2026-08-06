#pragma once

// Private implementation header for AnnotateAnalysisUi — shared by:
//   AnnotateAnalysisUi.cpp, AnnotateAnalysisUi_Config.cpp, AnnotateAnalysisUi_Worker.cpp,
//   AnnotateAnalysisUi_Modals.cpp, AnnotateAnalysisUi_Preferences.cpp,
//   AnnotateAnalysisUi_Window.cpp.
// Do NOT include this from any other translation unit; it pulls in the file-scope
// `AnnotateRow`, `DetailPack`, `WorkerState` types and the full `AnnotateState`
// definition, plus the `#define ImGui SmatchetLocalizedImGui` alias.
// Cross-TU helpers that were previously in an anonymous namespace are exposed
// inside `namespace AnnotateInternal { ... }` with external linkage so the linker
// resolves them across the new TUs without ODR collisions against any other
// file's anon namespace.

#include "AnnotateAnalysisUi.h"

#include "CallstackParser.h"
#include "ConfigManager.h"
#include "P4Annotate.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <atomic>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class AppController;
class IAppTicketData;
class IAppTicketMutations;
struct TrackerField;

// File-scope types — used by AnnotateAnalysisUi::AnnotateState defined below.
// Not in an anon namespace so AnnotateState (a nested type) can name them.

struct AnnotateRow {
    ParsedCallstackFrame Parsed;
    std::string PathForP4;
    P4LineAnnotate Annotate;
};

/// Per-row edit buffers backing the multi-rule path-remap editor in the
/// Annotate preferences form. Mirror `AnnotateAnalysisConfig::PathRemaps` 1:1.
struct RemapEditBuf {
    char from[512]{};
    char to[512]{};
};

struct DetailPack {
    std::vector<P4AnnotatedLine> Lines;
    std::string Error;
};

struct WorkerState {
    std::mutex Mutex;
    std::vector<AnnotateRow> Rows;
    std::atomic<bool> Cancel{false};
    std::atomic<bool> Running{false};
    std::atomic<int> Progress{0};
    std::atomic<size_t> Total{0};
    std::atomic<bool> PendingPublish{false};
    std::thread Thread;
    AnnotateAnalysisConfig Cfg;
    std::string AtChangelist;
    std::unique_ptr<P4ChangelistDescribeCache> Cache;
};

/// Pimpl state for AnnotateAnalysisUi. Owned by the class instance so it survives Unreal hot-reload.
/// Previously a Meyers singleton; all helpers reach it via State() which indirects through
/// s_stateInstance set by the constructor/destructor.
struct AnnotateAnalysisUi::AnnotateState {
    WorkerState worker;

    char callstackBuf[65536]{};
    std::vector<char> ignoreBuf = std::vector<char>(4096, '\0');
    char atClBuf[64]{};
    std::string beforeDateIso;
    int maxFramesVal = 64;
    int clCacheVal = 512;
    std::vector<RemapEditBuf> remapBufs;
    char p4Exe[260]{};
    char p4vcExe[260]{};
    char timeTpl[1024]{};
    char changeTpl[512]{};
    char aiUrl[512]{};
    bool showRaw = false;
    AnnotateAnalysisConfig annotateCfg;

    std::string loggedP4Exe;
    std::string loggedP4vcExe;

    std::mutex displayMutex;
    std::vector<AnnotateRow> displayRows;
    std::vector<std::shared_future<DetailPack>> detailFuts;
    std::vector<std::shared_future<DetailPack>> detachedDetailFuts;
    std::vector<int> detailPhase;
    std::vector<DetailPack> detailData;
    std::vector<bool> detailScrolled;

    std::string profileName;
    std::string profileEmail;
    std::string profileErr;
    std::vector<std::string> profileGroups;
    bool openProfileModal = false;
    /// DR22 — the assign / quick-comment modal is opened from row cells and the Entry-tab
    /// row menu, both of which live inside a child window. OpenPopup issued there resolves
    /// to a different id than BeginPopupModal at the parent scope, so the modal never
    /// appeared. This flag defers the OpenPopup to the same scope as BeginPopupModal.
    bool openAssignModal = false;
    /// Pillar 2 — finding #5: profile + assign-modal HTTP runs on a worker. These gates
    /// suppress duplicate dispatches while a fetch is in flight, and the modal renders a
    /// "Loading..." status until the post-back populates the fields.
    bool profileInFlight = false;
    bool assignInFlight = false;
    bool assignCommitInFlight = false;

    std::string lastUiStatus;
    int pendingSelectEntryIndex = -1;

    /// Pillar 2 — finding #761: resolving the first-submitted CL for a calendar day runs a
    /// slow server-wide `p4 changes -r -m 1 -s submitted //...@start,end` scan. Off-thread it
    /// (mirror P4ClPreview::DrawClTooltipAsync): launch on confirm, poll every frame, apply on
    /// the UI thread.
    std::shared_future<std::pair<std::string, std::string>> beforeClFut; // {changelist, error}
    bool beforeClResolving = false;
    /// Pillar 2 — issue #1459: re-confirming a calendar day while the first server-wide scan is
    /// still in flight must not destroy the unready std::async future on the UI thread (its
    /// destructor blocks until the task finishes). Detach the pending future here on overwrite and
    /// reap ready ones off the hot path in PollDetails (mirrors P4ClPreview::DetachedHoverFuts).
    std::vector<std::shared_future<std::pair<std::string, std::string>>> detachedBeforeClFuts;

    std::string assignTitle;
    std::string assignAccountId;
    bool assignHasJiraAccount = false;
    AnnotateRow assignRow;

    /** Opened via grid "Annotate…" with callstack; auto-process + compact UI until raw view. */
    bool annotateStreamlinedFromGrid = false;
    bool annotatePendingAutoProcess = false;
    /** Esc pressed while a callstack / results exist: window hides but state survives (P2-H5). */
    bool annotateHidePreservesState = false;

    std::string lastCallstackIssueKey;
    bool annotateCfgDiskHydrated = false;

    // Signal cancel + join the worker before any other member is destroyed.
    ~AnnotateState() {
        worker.Cancel.store(true, std::memory_order_release);
        if (worker.Thread.joinable()) {
            worker.Thread.join();
        }
    }
};

extern AnnotateAnalysisUi::AnnotateState* s_stateInstance;
inline AnnotateAnalysisUi::AnnotateState& State() { return *s_stateInstance; }

/// CPP_CODE_AUDIT.md #23: guard for PostToMainThread callbacks queued by a background
/// task (LaunchBackgroundTask) before ~AnnotateAnalysisUi() nulls s_stateInstance. The
/// destructor doesn't drain the dispatcher or join the launched task, so a callback can
/// still be sitting in the main-thread queue when it runs; without this check it would
/// dereference a null s_stateInstance (teardown) or write into a hot-reloaded instance's
/// unrelated state (Unreal). Call this FIRST in every PostToMainThread lambda that reads
/// State(); return early when false instead of calling State().
inline bool HasLiveStateInstance() { return s_stateInstance != nullptr; }

namespace AnnotateInternal {

// CopyToBuffer — template, defined in header.
template <size_t N> inline void CopyToBuffer(char (&dst)[N], const std::string& src) {
    static_assert(N > 0, "CopyToBuffer requires a non-empty char array");
    std::memset(dst, 0, N);
    const size_t cap = N - 1;
    const size_t n = (std::min)(src.size(), cap);
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
}

// --- Config-side helpers (AnnotateAnalysisUi_Config.cpp) ---
void LogAnnotateP4PathsIfChanged(const char* reason);
void HydrateAnnotateCfgDiskOnce();
/// Persist the Annotate config off the UI thread (value-snapshot + detached worker).
/// Mirrors ScheduleConfigSaveDetached; use instead of ConfigManager::SaveAnnotateAnalysis
/// from any UI-callback save site.
void ScheduleAnnotateConfigSaveDetached(const AnnotateAnalysisConfig& cfg);
void MaybeAutoselectCallstackTrackerField(const std::vector<TrackerField>& availableFields);
void MaybeAutoselectLastFoundClTrackerField(const std::vector<TrackerField>& availableFields);
void MaybeAutoselectLastOccurrencesTrackerField(const std::vector<TrackerField>& availableFields);
void ApplyShowRawCallstack(bool show);
void TryFillBeforeChangelistAndDateFromJira(const IAppTicketData& ticketData, const std::string& issueKey);
void TryFillCallstackFromJira(const IAppTicketData& ticketData, const std::string& issueKey);
std::vector<std::string> SplitIgnoreKeywords(const std::string& multi);

// --- Worker / detail-poll helpers (AnnotateAnalysisUi_Worker.cpp) ---
void JoinWorkerIfNeeded();
void MirrorWorkerToDisplay();
void ResetDetailAfterRunComplete();
void WorkerThreadMain(size_t n);
void StartWorker(std::vector<AnnotateRow> rows, AnnotateAnalysisConfig cfg, std::string atCl);
void RunAnnotateProcessFromBuffers();
void EnsureDetailLoading(size_t idx, const AnnotateAnalysisConfig& cfg, const std::string& atCl);
void PollDetails();

// --- Modal / themed-button helpers (AnnotateAnalysisUi_Modals.cpp) ---
ImVec4 ThCol(const float* c);
ImVec4 AnnotateLinkText(const AnnotateUiThemeColors& theme);
void PushAnnotateLinkButtonColors(const AnnotateUiThemeColors& theme);
void PopAnnotateLinkButtonColors();
void PushAnnotateLinkTextOnly(const AnnotateUiThemeColors& theme);
void PopAnnotateLinkTextOnly();
bool ResolveP4UserForAssign(const AppController& app, const std::string& p4User, std::string& accountId,
                            std::string& err);
void CloseAnnotateModal(bool* pOpen);
void OpenTrackerUserProfileForP4User(const AppController& app, const std::string& p4User);
void PrepareAssignModal(const AppController& app, const AnnotateRow& row, const std::string& p4UserCell);
std::string NormalizeDateDisplay(const std::string& raw);
std::string ShortenPathForDisplay(const std::string& path, float maxWidthPx);
std::string BuildAiExport();
std::string CsvEscape(const std::string& s);
std::string BuildAnnotateExportCsv();
std::string BuildAnnotateExportJson();
std::string BuildCallstackRowTsv(const AnnotateRow& row, size_t displayIndex);
std::string BuildAnnotatedRowTsv(const P4AnnotatedLine& ln);
std::string BuildAnnotateQuickCommentTemplate(const std::string& issueKey, const std::string& templateId,
                                              const AnnotateRow& row, const std::vector<CommentTemplate>& templates);

// --- Preferences-section helpers (AnnotateAnalysisUi_Preferences.cpp) ---
// One body per AnnotateAnalysisUi::AnnotatePrefsSection. The owning Preferences page draws the
// CollapsingHeader chrome, so none of these emit their own section title. Each row asks
// `filter` whether it draws, so a Preferences search narrows these bodies like any other.
void DrawAnnotateAnalysisFields(PreferencesFilter& filter);
void DrawAnnotatePerforceFields(PreferencesFilter& filter);
void DrawAnnotateJiraFieldCombos(const std::vector<TrackerField>& availableFields,
                                 const IAppTicketMutations& ticketMutations, const AnnotateUiThemeColors& theme,
                                 PreferencesFilter& filter);
void DrawAnnotateColors(PreferencesFilter& filter);

// --- DrawContent section helpers (AnnotateAnalysisUi_Window.cpp) ---
// Per-frame context threaded through the section helpers that DrawContent
// orchestrates. Holds references + the once-per-frame snapshots (rows, busy/prog
// gates, theme) so each helper reads them without re-fetching. Each section's
// Begin*/End* pairing stays wholly inside its owning helper — see
// docs/guides/imgui-draw-pattern.md § Positional-ImGui hazards.
struct AnnotateDrawCtx {
    AppController& App;
    bool* WantClose;
    const std::string& SelectedJiraIssueKey;
    const AnnotateUiThemeColors& Theme;
    const std::vector<AnnotateRow>& RowsSnap;
    size_t NRow;
    bool Busy;
    int Prog;
};

/// Title row + Ask AI / Export JSON / Export CSV / Close buttons. Returns true when
/// the user clicked Close (DrawContent then returns early, as the monolith did).
bool DrawAnnotateHeaderToolbar(AnnotateDrawCtx& ctx);
/// Jira connectivity banner + last-status line, each followed by a separator.
void DrawAnnotateBannerAndStatus(AnnotateDrawCtx& ctx);
/// The "Callstack" tab body: frame-count + view toggle, before-CL/day controls,
/// raw/multiline editor, and the 6-column callstack table.
void DrawAnnotateCallstackTab(AnnotateDrawCtx& ctx);
/// One "Entry N" tab body: the per-frame annotated-source table.
void DrawAnnotateEntryTab(AnnotateDrawCtx& ctx, size_t ti);
/// The "annotate_assign" assign/comment popup modal.
void DrawAnnotateAssignModal(AnnotateDrawCtx& ctx);
/// The "Jira user profile" popup modal.
void DrawAnnotateProfileModal(AnnotateDrawCtx& ctx);

} // namespace AnnotateInternal
