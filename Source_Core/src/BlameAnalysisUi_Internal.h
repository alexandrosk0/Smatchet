#pragma once

// Private implementation header for BlameAnalysisUi — shared by:
//   BlameAnalysisUi.cpp, BlameAnalysisUi_Config.cpp, BlameAnalysisUi_Worker.cpp,
//   BlameAnalysisUi_Launch.cpp, BlameAnalysisUi_Modals.cpp,
//   BlameAnalysisUi_Preferences.cpp, BlameAnalysisUi_Window.cpp.
//
// Do NOT include this from any other translation unit; it pulls in the file-scope
// `BlameRow`, `DetailPack`, `WorkerState` types and the full `BlameState`
// definition, plus the `#define ImGui SmatchetLocalizedImGui` alias.
//
// Cross-TU helpers that were previously in an anonymous namespace are exposed
// inside `namespace BlameInternal { ... }` with external linkage so the linker
// resolves them across the new TUs without ODR collisions against any other
// file's anon namespace.

#include "BlameAnalysisUi.h"

#include "CallstackParser.h"
#include "ConfigManager.h"
#include "P4Blame.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <atomic>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AppController;

// ---------------------------------------------------------------------------
// File-scope types — used by BlameAnalysisUi::BlameState defined below.
// Not in an anon namespace so BlameState (a nested type) can name them.
// ---------------------------------------------------------------------------

struct BlameRow {
    ParsedCallstackFrame Parsed;
    std::string PathForP4;
    P4LineBlame Blame;
};

struct DetailPack {
    std::vector<P4AnnotatedLine> Lines;
    std::string Error;
};

struct WorkerState {
    std::mutex Mutex;
    std::vector<BlameRow> Rows;
    std::atomic<bool> Cancel{false};
    std::atomic<bool> Running{false};
    std::atomic<int> Progress{0};
    std::atomic<size_t> Total{0};
    std::atomic<bool> PendingPublish{false};
    std::thread Thread;
    BlameAnalysisConfig Cfg;
    std::string AtChangelist;
    std::unique_ptr<P4ChangelistDescribeCache> Cache;
};

/// Pimpl state for BlameAnalysisUi. Owned by the class instance so it survives Unreal hot-reload.
/// Previously a Meyers singleton; all helpers reach it via State() which indirects through
/// s_stateInstance set by the constructor/destructor.
struct BlameAnalysisUi::BlameState {
    WorkerState worker;
    P4ChangelistDescribeCache tooltipClCache{512};

    char callstackBuf[65536]{};
    std::vector<char> ignoreBuf = std::vector<char>(4096, '\0');
    char atClBuf[64]{};
    std::string beforeDateIso;
    int maxFramesVal = 64;
    char remapFrom[512]{};
    char remapTo[512]{};
    char p4Exe[260]{};
    char p4vcExe[260]{};
    char timeTpl[1024]{};
    char changeTpl[512]{};
    char aiUrl[512]{};
    bool showRaw = false;
    BlameAnalysisConfig blameCfg;

    std::string loggedP4Exe;
    std::string loggedP4vcExe;

    std::mutex displayMutex;
    std::vector<BlameRow> displayRows;
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
    /// Pillar 2 — finding #5: profile + assign-modal HTTP runs on a worker. These gates
    /// suppress duplicate dispatches while a fetch is in flight, and the modal renders a
    /// "Loading..." status until the post-back populates the fields.
    bool profileInFlight = false;
    bool assignInFlight = false;
    bool assignCommitInFlight = false;

    std::string lastUiStatus;
    int pendingSelectEntryIndex = -1;

    std::string clHoverCl;
    std::shared_future<P4ChangelistDetails> clHoverFut;
    std::vector<std::shared_future<P4ChangelistDetails>> detachedClHoverFuts;

    std::string assignTitle;
    std::string assignAccountId;
    bool assignHasJiraAccount = false;
    BlameRow assignRow;

    /** Opened via grid "Annotate…" with callstack; auto-process + compact UI until raw view. */
    bool blameStreamlinedFromGrid = false;
    bool blamePendingAutoProcess = false;

    std::string lastCallstackIssueKey;
    bool blameCfgDiskHydrated = false;

    // Signal cancel + join the worker before any other member is destroyed.
    ~BlameState() {
        worker.Cancel.store(true, std::memory_order_release);
        if (worker.Thread.joinable()) {
            worker.Thread.join();
        }
    }
};

extern BlameAnalysisUi::BlameState* s_stateInstance;
inline BlameAnalysisUi::BlameState& State() { return *s_stateInstance; }

namespace BlameInternal {

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

// --- Config-side helpers (BlameAnalysisUi_Config.cpp) ---
void LogBlameP4PathsIfChanged(const char* reason);
void HydrateBlameCfgDiskOnce();
void MaybeAutoselectCallstackTrackerField(const AppController& app);
void MaybeAutoselectLastFoundClTrackerField(const AppController& app);
void MaybeAutoselectLastOccurrencesTrackerField(const AppController& app);
void TryFillBeforeChangelistAndDateFromJira(const AppController& app, const std::string& issueKey);
void TryFillCallstackFromJira(const AppController& app, const std::string& issueKey);
std::vector<std::string> SplitIgnoreKeywords(const std::string& multi);

// --- Worker / detail-poll helpers (BlameAnalysisUi_Worker.cpp) ---
void JoinWorkerIfNeeded();
void MirrorWorkerToDisplay();
void ResetDetailAfterRunComplete();
void WorkerThreadMain(size_t n);
void StartWorker(std::vector<BlameRow> rows, BlameAnalysisConfig cfg, std::string atCl);
void RunBlameProcessFromBuffers();
void EnsureDetailLoading(size_t idx, const BlameAnalysisConfig& cfg, const std::string& atCl);
void PollDetails();

// --- Launch helpers (BlameAnalysisUi_Launch.cpp) ---
bool LaunchP4VcLike(const BlameAnalysisConfig& cfg, const std::string& timelapseTemplate,
                    const std::string& changeTemplate, bool isTimelapse, const std::string& file, int line,
                    const std::string& cl);

// --- Modal / themed-button helpers (BlameAnalysisUi_Modals.cpp) ---
ImVec4 ThCol(const float* c);
ImVec4 BlameLinkText(const BlameUiThemeColors& theme);
void PushBlameLinkButtonColors(const BlameUiThemeColors& theme);
void PopBlameLinkButtonColors();
void PushBlameLinkTextOnly(const BlameUiThemeColors& theme);
void PopBlameLinkTextOnly();
bool ResolveP4UserForAssign(const AppController& app, const std::string& p4User, std::string& accountId,
                            std::string& err);
void CloseBlameModal(bool* pOpen);
void OpenTrackerUserProfileForP4User(const AppController& app, const std::string& p4User);
void PrepareAssignModal(const AppController& app, const BlameRow& row, const std::string& p4UserCell);
void DrawClTooltipAsync(const std::string& cl, const BlameAnalysisConfig& cfg, const BlameUiThemeColors& theme);
std::string NormalizeDateDisplay(const std::string& raw);
std::string ShortenPathForDisplay(const std::string& path, float maxWidthPx);
std::string BuildAiExport();
std::string CsvEscape(const std::string& s);
std::string BuildBlameExportCsv();
std::string BuildBlameExportJson();
std::string BuildCallstackRowTsv(const BlameRow& row, size_t displayIndex);
std::string BuildAnnotatedRowTsv(const P4AnnotatedLine& ln);
std::string BuildBlameQuickCommentTemplate(const std::string& issueKey, const std::string& templateId,
                                           const BlameRow& row, const std::vector<CommentTemplate>& templates);

// --- Preferences-form helper (BlameAnalysisUi_Preferences.cpp) ---
void DrawBlamePersistedOptionsForm(const AppController& app, const BlameUiThemeColors& theme);

} // namespace BlameInternal
