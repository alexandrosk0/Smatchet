#include "BlameAnalysisUi.h"

#include "AppController.h"
#include "LocalCacheManager.h"
#include "BlameSyntaxHighlight.h"
#include "CallstackParser.h"
#include "CompactDateFormat.h"
#include "ConfigManager.h"
#include "JiraClient.h"
#include "Logger.h"
#include "P4Blame.h"
#include "SpreadsheetState.h"
#include "StringUtil.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerFieldSchema.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

// ---------------------------------------------------------------------------
// File-scope types — visible to BlameAnalysisUi::BlameState defined below.
// Not in anonymous namespace so BlameState (a nested class member) can use them.
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

/// Pimpl state for BlameAnalysisUi. Owned by the class instance so it survives Unreal hot-reload
/// (step (b) of CODE_REVIEW.md §3.5 proposal 2). Previously a Meyers singleton
/// (BlameAnalysisUiState); all free helpers below reach it via State() which now indirects through
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

    std::string lastUiStatus;
    int pendingSelectEntryIndex = -1;

    std::string clHoverCl;
    std::shared_future<P4ChangelistDetails> clHoverFut;
    std::vector<std::shared_future<P4ChangelistDetails>> detachedClHoverFuts;

    std::string assignTitle;
    std::string assignAccountId;
    bool assignHasJiraAccount = false;
    BlameRow assignRow;

    char callstackTrackerFieldBuf[260]{};
    char lastFoundClFieldBuf[260]{};
    char lastOccurrencesFieldBuf[260]{};

    /** Opened via grid "Open Source Blame…" with callstack; auto-process + compact UI until raw view. */
    bool blameStreamlinedFromGrid = false;
    bool blamePendingAutoProcess = false;

    // Previously separate file-statics; folded in so they share instance lifetime.
    std::string lastCallstackIssueKey;
    bool blameCfgDiskHydrated = false;

    // Signal cancel + join the worker before any other member is destroyed. Without this,
    // `WorkerState::Thread` (a joinable std::thread member) is destroyed with a running worker
    // still pointed at our soon-to-be-freed state — `std::terminate` is mandated by the standard
    // for that case. This destructor runs synchronously on the UI thread when ~BlameAnalysisUi
    // releases its unique_ptr<BlameState>; the worker treats `Cancel` as a poll-point in every
    // long-running step so the join completes promptly.
    ~BlameState() {
        worker.Cancel.store(true, std::memory_order_release);
        if (worker.Thread.joinable()) {
            worker.Thread.join();
        }
    }
};

static BlameAnalysisUi::BlameState* s_stateInstance = nullptr;
inline BlameAnalysisUi::BlameState& State() { return *s_stateInstance; }

namespace {

static void LogBlameP4PathsIfChanged(const char* reason) {
    auto& st = State();
    if (st.blameCfg.P4Executable != st.loggedP4Exe) {
        LOG_INFO("Blame [%s]: p4_exe \"%s\" -> \"%s\"", reason, st.loggedP4Exe.c_str(),
                 st.blameCfg.P4Executable.c_str());
        st.loggedP4Exe = st.blameCfg.P4Executable;
    }
    if (st.blameCfg.P4VcExecutable != st.loggedP4vcExe) {
        LOG_INFO("Blame [%s]: p4vc_exe \"%s\" -> \"%s\"", reason, st.loggedP4vcExe.c_str(),
                 st.blameCfg.P4VcExecutable.c_str());
        st.loggedP4vcExe = st.blameCfg.P4VcExecutable;
    }
}

template <size_t N> void CopyToBuffer(char (&dst)[N], const std::string& src) {
    static_assert(N > 0, "CopyToBuffer requires a non-empty char array");
    std::memset(dst, 0, N);
    const size_t cap = N - 1;
    const size_t n = (std::min)(src.size(), cap);
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
}

void SyncCallstackTrackerFieldBufFromCfg() { CopyToBuffer(State().callstackTrackerFieldBuf, State().blameCfg.CallstackTrackerFieldId); }

void SyncJiraBlameAuxFieldBufsFromCfg() {
    CopyToBuffer(State().lastFoundClFieldBuf, State().blameCfg.LastFoundClTrackerFieldId);
    CopyToBuffer(State().lastOccurrencesFieldBuf, State().blameCfg.LastOccurrencesTrackerFieldId);
}

static void HydrateBlameCfgDiskOnce() {
    if (State().blameCfgDiskHydrated) {
        return;
    }
    State().blameCfg = ConfigManager::LoadBlameAnalysis();
    SyncCallstackTrackerFieldBufFromCfg();
    SyncJiraBlameAuxFieldBufsFromCfg();
    State().blameCfgDiskHydrated = true;
}

void MaybeAutoselectCallstackTrackerField(const AppController& app) {
    if (!State().blameCfg.CallstackTrackerFieldId.empty()) {
        return;
    }
    const auto& fields = app.GetAvailableFields();
    if (fields.empty()) {
        return;
    }
    auto it = std::find_if(fields.begin(), fields.end(), [](const auto& f) {
        return ToLowerAsciiCopy(f.Name) == "callstack";
    });
    if (it != fields.end()) {
        State().blameCfg.CallstackTrackerFieldId = it->Id;
        SyncCallstackTrackerFieldBufFromCfg();
        ConfigManager::SaveBlameAnalysis(State().blameCfg);
    }
}

void MaybeAutoselectLastFoundClTrackerField(const AppController& app) {
    if (!State().blameCfg.LastFoundClTrackerFieldId.empty()) {
        return;
    }
    const auto& fields = app.GetAvailableFields();
    if (fields.empty()) {
        return;
    }
    const auto it = std::find_if(fields.begin(), fields.end(), [](const TrackerField& f) {
        const std::string n = ToLowerAsciiCopy(f.Name);
        return n == "last found cl" || n == "lastfoundcl" || n == "last_found_cl";
    });
    if (it != fields.end()) {
        State().blameCfg.LastFoundClTrackerFieldId = it->Id;
        SyncJiraBlameAuxFieldBufsFromCfg();
        ConfigManager::SaveBlameAnalysis(State().blameCfg);
    }
}

void MaybeAutoselectLastOccurrencesTrackerField(const AppController& app) {
    if (!State().blameCfg.LastOccurrencesTrackerFieldId.empty()) {
        return;
    }
    const auto& fields = app.GetAvailableFields();
    if (fields.empty()) {
        return;
    }
    const auto it = std::find_if(fields.begin(), fields.end(), [](const TrackerField& f) {
        const std::string n = ToLowerAsciiCopy(f.Name);
        return n == "last occurrences" || n == "last occurances" || n == "last occurences" || n == "last_occurrences";
    });
    if (it != fields.end()) {
        State().blameCfg.LastOccurrencesTrackerFieldId = it->Id;
        SyncJiraBlameAuxFieldBufsFromCfg();
        ConfigManager::SaveBlameAnalysis(State().blameCfg);
    }
}

static std::string SanitizeChangelistDigitsFromField(const std::string& raw) {
    const std::string v = TrimCopy(raw);
    if (v.empty()) {
        return std::string();
    }
    size_t end = v.size();
    const size_t nl = v.find_first_of("\r\n");
    if (nl != std::string::npos) {
        end = nl;
    }
    std::string digits;
    for (size_t i = 0; i < end; ++i) {
        const char c = v[i];
        if (c >= '0' && c <= '9') {
            digits += c;
        } else if (!digits.empty()) {
            break;
        }
    }
    return digits;
}

static std::string NormalizeJiraDateForBeforePicker(const std::string& raw) {
    const std::string v = TrimCopy(raw);
    if (v.empty()) {
        return std::string();
    }
    ParsedJiraDateTime p;
    if (TryParseJiraDateTime(v, p)) {
        return FormatJiraDateOrDateTimeForApi(true, p);
    }
    return std::string();
}

void TryFillBeforeChangelistAndDateFromJira(const AppController& app, const std::string& issueKey) {
    if (issueKey.empty()) {
        State().beforeDateIso.clear();
        return;
    }
    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    for (const auto& t : *ticketsSnap) {
        if (t.id != issueKey) {
            continue;
        }
        if (!State().blameCfg.LastFoundClTrackerFieldId.empty()) {
            const std::string cl = SanitizeChangelistDigitsFromField(t.GetFieldValue(State().blameCfg.LastFoundClTrackerFieldId));
            CopyToBuffer(State().atClBuf, cl);
        }
        if (!State().blameCfg.LastOccurrencesTrackerFieldId.empty()) {
            State().beforeDateIso = NormalizeJiraDateForBeforePicker(t.GetFieldValue(State().blameCfg.LastOccurrencesTrackerFieldId));
        } else {
            State().beforeDateIso.clear();
        }
        return;
    }
    State().beforeDateIso.clear();
    if (!State().blameCfg.LastFoundClTrackerFieldId.empty()) {
        State().atClBuf[0] = '\0';
    }
}

void TryFillCallstackFromJira(const AppController& app, const std::string& issueKey) {
    if (State().blameCfg.CallstackTrackerFieldId.empty() || issueKey.empty()) {
        return;
    }
    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    for (const auto& t : *ticketsSnap) {
        if (t.id != issueKey) {
            continue;
        }
        const std::string v = t.GetFieldValue(State().blameCfg.CallstackTrackerFieldId);
        if (v.empty()) {
            return;
        }
        CopyToBuffer(State().callstackBuf, v);
        return;
    }
}

std::vector<std::string> SplitIgnoreKeywords(const std::string& multi) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : multi) {
        if (c == ',' || c == '\n' || c == '\r') {
            while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) {
                cur.pop_back();
            }
            size_t i = 0;
            while (i < cur.size() && (cur[i] == ' ' || cur[i] == '\t')) {
                ++i;
            }
            if (i < cur.size()) {
                out.push_back(cur.substr(i));
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) {
        cur.pop_back();
    }
    size_t i = 0;
    while (i < cur.size() && (cur[i] == ' ' || cur[i] == '\t')) {
        ++i;
    }
    if (i < cur.size()) {
        out.push_back(cur.substr(i));
    }
    return out;
}

void JoinWorkerIfNeeded() {
    if (State().worker.Thread.joinable() && !State().worker.Running.load()) {
        State().worker.Thread.join();
    }
}

void MirrorWorkerToDisplay() {
    std::lock_guard<std::mutex> lkW(State().worker.Mutex);
    if (State().worker.Rows.empty() && !State().worker.Running.load()) {
        return;
    }
    std::lock_guard<std::mutex> lkD(State().displayMutex);
    State().displayRows = State().worker.Rows;
}

void ResetDetailAfterRunComplete() {
    if (!State().worker.PendingPublish.exchange(false)) {
        return;
    }
    std::lock_guard<std::mutex> lkD(State().displayMutex);
    const size_t n = State().displayRows.size();
    State().detailFuts.assign(n, std::shared_future<DetailPack>());
    State().detailPhase.assign(n, 0);
    State().detailData.assign(n, DetailPack{});
    State().detailScrolled.assign(n, false);
}

void WorkerThreadMain(size_t n) {
    BlameAnalysisConfig cfg = State().worker.Cfg;
    const std::string atCl = State().worker.AtChangelist;
    P4ChangelistDescribeCache* cache = State().worker.Cache.get();
    LOG_INFO("Blame worker: started rows=%zu atCl=\"%s\" p4_exe=\"%s\"", n, atCl.c_str(), cfg.P4Executable.c_str());
    try {
        int failures = 0;
        for (size_t i = 0; i < n; ++i) {
            if (State().worker.Cancel.load()) {
                LOG_INFO("Blame worker: cancelled at row %zu/%zu (failures=%d)", i, n, failures);
                break;
            }
            BlameRow row;
            {
                std::lock_guard<std::mutex> lk(State().worker.Mutex);
                if (i >= State().worker.Rows.size()) {
                    break;
                }
                row = State().worker.Rows[i];
            }
            P4LineBlame b = P4BlameLine(cfg, row.PathForP4, row.Parsed.LineNumber, atCl);
            if (!b.Error.empty()) {
                LOG_WARN("Blame worker: row=%zu path=%s line=%d err=%s", i + 1, row.PathForP4.c_str(),
                         row.Parsed.LineNumber, b.Error.c_str());
                ++failures;
            }
            if (!b.Changelist.empty() && cache) {
                P4ChangelistDetails d = cache->GetOrFetch(cfg, b.Changelist);
                if (!d.Date.empty()) {
                    b.Date = d.Date;
                }
                if (!d.Author.empty() && b.User.empty()) {
                    b.User = d.Author;
                }
            }
            {
                std::lock_guard<std::mutex> lk(State().worker.Mutex);
                if (i < State().worker.Rows.size()) {
                    State().worker.Rows[i].Blame = std::move(b);
                }
            }
            State().worker.Progress = static_cast<int>(i + 1);
        }
        if (!State().worker.Cancel.load()) {
            LOG_INFO("Blame worker: finished rows=%zu failures=%d", n, failures);
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("Blame worker: exception: %s", ex.what());
    } catch (...) {
        LOG_ERROR("Blame worker: unknown exception");
    }
    State().worker.PendingPublish = true;
    State().worker.Running = false;
}

void StartWorker(std::vector<BlameRow> rows, BlameAnalysisConfig cfg, std::string atCl) {
    JoinWorkerIfNeeded();
    LOG_INFO("Blame: StartWorker rows=%zu atCl=\"%s\"", rows.size(), atCl.c_str());
    State().worker.Cancel = false;
    State().worker.Progress = 0;
    State().worker.PendingPublish = false;
    State().worker.Cfg = std::move(cfg);
    State().worker.AtChangelist = std::move(atCl);
    const int cap = State().worker.Cfg.ChangelistCacheMaxEntries > 0 ? State().worker.Cfg.ChangelistCacheMaxEntries : 512;
    State().worker.Cache.reset(new P4ChangelistDescribeCache(cap));
    const size_t n = rows.size();
    State().worker.Total = n;
    {
        std::lock_guard<std::mutex> lk(State().worker.Mutex);
        State().worker.Rows = std::move(rows);
    }
    {
        std::lock_guard<std::mutex> lkW(State().worker.Mutex);
        std::lock_guard<std::mutex> lkD(State().displayMutex);
        State().displayRows = State().worker.Rows;
        State().detailFuts.assign(n, std::shared_future<DetailPack>());
        State().detailPhase.assign(n, 0);
        State().detailData.assign(n, DetailPack{});
        State().detailScrolled.assign(n, false);
    }
    State().worker.Running = true;
    State().worker.Thread = std::thread(WorkerThreadMain, n);
}

/** Same logic as the Process button (parse `State().callstackBuf`, start worker). Caller ensures worker idle. */
static void RunBlameProcessFromBuffers() {
    State().lastUiStatus.clear();
    State().blameCfg.P4Executable = State().p4Exe;
    State().blameCfg.P4VcExecutable = State().p4vcExe;
    State().blameCfg.TimelapseCommandTemplate = State().timeTpl;
    State().blameCfg.ChangeCommandTemplate = State().changeTpl;
    State().blameCfg.AiChatUrl = State().aiUrl;
    State().blameCfg.CallstackTrackerFieldId.assign(State().callstackTrackerFieldBuf);
    State().blameCfg.LastFoundClTrackerFieldId.assign(State().lastFoundClFieldBuf);
    State().blameCfg.LastOccurrencesTrackerFieldId.assign(State().lastOccurrencesFieldBuf);
    State().blameCfg.PathRemaps.clear();
    if (State().remapFrom[0] != '\0') {
        State().blameCfg.PathRemaps.push_back({State().remapFrom, State().remapTo});
    }
    LogBlameP4PathsIfChanged("process");
    std::vector<std::string> keywords = SplitIgnoreKeywords(std::string(State().ignoreBuf.data()));
    if (keywords.empty()) {
        keywords = State().blameCfg.DefaultIgnoreKeywords;
    }
    std::vector<ParsedCallstackFrame> parsed = ParseCallstackText(std::string(State().callstackBuf));
    std::vector<BlameRow> rows;
    for (auto& p : parsed) {
        if (FrameMatchesIgnoreKeywords(p, keywords)) {
            continue;
        }
        BlameRow br;
        br.Parsed = std::move(p);
        br.PathForP4 = ApplyPathRemaps(br.Parsed.FilePath, State().blameCfg.PathRemaps);
        rows.push_back(std::move(br));
        if (static_cast<int>(rows.size()) >= State().maxFramesVal) {
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lk(State().displayMutex);
        State().displayRows.clear();
        State().detailFuts.clear();
        State().detailPhase.clear();
        State().detailData.clear();
        State().detailScrolled.clear();
    }
    if (rows.empty()) {
        State().lastUiStatus = "No stack frames parsed (check format and ignore list).";
    } else {
        StartWorker(std::move(rows), State().blameCfg, std::string(State().atClBuf));
    }
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

void ReplacePlaceholder(std::string& s, const char* key, const std::string& val) {
    const std::string k = key;
    size_t pos = 0;
    while ((pos = s.find(k, pos)) != std::string::npos) {
        s.replace(pos, k.size(), val);
        pos += val.size();
    }
}

bool SplitCommandExecutableAndArgs(const std::string& command, std::string& outExe, std::string& outArgs) {
    const std::string trimmed = TrimCopy(command);
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed[0] == '"') {
        const size_t quoteEnd = trimmed.find('"', 1);
        if (quoteEnd == std::string::npos || quoteEnd <= 1) {
            return false;
        }
        outExe = trimmed.substr(1, quoteEnd - 1);
        outArgs = TrimCopy(trimmed.substr(quoteEnd + 1));
        return !outExe.empty();
    }
    size_t split = 0;
    while (split < trimmed.size() && trimmed[split] != ' ' && trimmed[split] != '\t' && trimmed[split] != '\r' &&
           trimmed[split] != '\n') {
        ++split;
    }
    outExe = trimmed.substr(0, split);
    outArgs = split < trimmed.size() ? TrimCopy(trimmed.substr(split + 1)) : std::string();
    return !outExe.empty();
}

/** Win32 API argument quoting (embedded " -> \"). */
std::wstring QuoteWinArgWide(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\n\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') {
            out += L"\\\"";
        } else {
            out += c;
        }
    }
    out += L"\"";
    return out;
}

std::wstring ResolveP4VcExecutableWide(const BlameAnalysisConfig& cfg) {
    const std::string& exeUtf8 = cfg.P4VcExecutable.empty() ? std::string("p4vc") : cfg.P4VcExecutable;
    std::wstring wexe = Utf8ToWide(exeUtf8);
    if (wexe.find(L'\\') != std::wstring::npos || wexe.find(L'/') != std::wstring::npos) {
        return wexe;
    }
    wchar_t found[MAX_PATH];
    wchar_t* fname = nullptr;
    if (SearchPathW(nullptr, wexe.c_str(), L".exe", MAX_PATH, found, &fname) > 0) {
        return std::wstring(found);
    }
    return wexe;
}

std::wstring WorkingDirWideForFile(const std::string& fileUtf8) {
    const size_t slash = fileUtf8.find_last_of("\\/");
    if (slash == std::string::npos || slash == 0) {
        return std::wstring();
    }
    return Utf8ToWide(fileUtf8.substr(0, slash));
}

std::string WideToUtf8ForLog(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return std::string();
    }
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

bool LaunchP4VcLike(const BlameAnalysisConfig& cfg, const std::string& timelapseTemplate,
                    const std::string& changeTemplate, bool isTimelapse, const std::string& file, int line,
                    const std::string& cl) {
    const std::wstring workDir = WorkingDirWideForFile(file);
    const wchar_t* workDirPtr = workDir.empty() ? nullptr : workDir.c_str();

    bool customCmd = (isTimelapse && !timelapseTemplate.empty()) || (!isTimelapse && !changeTemplate.empty());

    // Custom templates invoke arbitrary programs from config-controlled strings.
    // Require explicit opt-in so a stolen/edited config file cannot auto-exec.
    if (customCmd) {
        const TrackerConfig jiraCfg = ConfigManager::Load();
        if (!jiraCfg.BlameAllowCustomCommands) {
            LOG_WARN("LaunchP4VcLike: custom command template present but blame_allow_custom_commands=false; falling "
                     "back to p4vc.");
            customCmd = false;
        }
    }

    if (!customCmd) {
        const std::wstring app = ResolveP4VcExecutableWide(cfg);
        std::wstring params;
        if (isTimelapse) {
            params = L"timelapse -l " + std::to_wstring(line) + L" " + QuoteWinArgWide(Utf8ToWide(file));
        } else {
            params = L"change " + Utf8ToWide(cl);
        }
        LOG_INFO("LaunchP4VcLike (direct p4vc): %s | CWD=%s",
                 (WideToUtf8ForLog(app) + " " + WideToUtf8ForLog(params)).c_str(),
                 workDirPtr ? WideToUtf8ForLog(workDir).c_str() : "(inherit)");
        SetLastError(0);
        const INT_PTR r = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, nullptr, app.c_str(), params.c_str(), workDirPtr, SW_SHOW));
        if (r <= 32) {
            LOG_WARN("LaunchP4VcLike: ShellExecuteW failed, result=%lld GetLastError=%lu", static_cast<long long>(r),
                     static_cast<unsigned long>(GetLastError()));
        }
        return r > 32;
    }

    std::string cmd;
    if (isTimelapse) {
        cmd = timelapseTemplate;
    } else {
        cmd = changeTemplate;
    }
    ReplacePlaceholder(cmd, "{file}", file);
    ReplacePlaceholder(cmd, "{line}", std::to_string(line));
    ReplacePlaceholder(cmd, "{cl}", cl);
    if (cmd.find('\r') != std::string::npos || cmd.find('\n') != std::string::npos) {
        LOG_WARN("LaunchP4VcLike: custom command rejected because it contains newline characters");
        return false;
    }
    std::string exeUtf8;
    std::string argsUtf8;
    if (!SplitCommandExecutableAndArgs(cmd, exeUtf8, argsUtf8)) {
        LOG_WARN("LaunchP4VcLike: custom command parse failed. Expected: <exe> [args]");
        return false;
    }
    LOG_INFO("LaunchP4VcLike (custom direct launch): exe=\"%s\" args=\"%s\"", exeUtf8.c_str(), argsUtf8.c_str());
    const std::wstring wexe = Utf8ToWide(exeUtf8);
    const std::wstring wargs = Utf8ToWide(argsUtf8);
    SetLastError(0);
    const INT_PTR r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, nullptr, wexe.c_str(), wargs.empty() ? nullptr : wargs.c_str(), workDirPtr, SW_SHOW));
    if (r <= 32) {
        LOG_WARN("LaunchP4VcLike: ShellExecuteW(custom) failed, result=%lld GetLastError=%lu",
                 static_cast<long long>(r), static_cast<unsigned long>(GetLastError()));
    }
    return r > 32;
}
#else
bool LaunchP4VcLike(const BlameAnalysisConfig&, const std::string&, const std::string&, bool, const std::string&, int,
                    const std::string&) {
    return false;
}
#endif

void EnsureDetailLoading(size_t idx, const BlameAnalysisConfig& cfg, const std::string& atCl) {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    if (idx >= State().displayRows.size()) {
        return;
    }
    if (idx >= State().detailPhase.size() || State().detailPhase[idx] != 0) {
        return;
    }
    State().detailPhase[idx] = 1;
    const std::string path = State().displayRows[idx].PathForP4;
    LOG_DEBUG("Blame detail: async load start idx=%zu path=%s", idx, path.c_str());
    std::future<DetailPack> fut = std::async(std::launch::async, [cfg, path, atCl]() {
        DetailPack p;
        p.Lines = P4AnnotateFile(cfg, path, atCl, p.Error);
        for (auto& ln : p.Lines) {
            if (!ln.Changelist.empty()) {
                P4ChangelistDetails d = State().tooltipClCache.GetOrFetch(cfg, ln.Changelist);
                if (!d.Date.empty()) {
                    ln.Date = d.Date;
                }
                if (!d.Author.empty() && ln.User.empty()) {
                    ln.User = d.Author;
                }
            }
        }
        return p;
    });
    State().detailFuts[idx] = fut.share();
}

void PollDetails() {
    for (size_t i = 0; i < State().detachedDetailFuts.size();) {
        if (!State().detachedDetailFuts[i].valid() ||
            State().detachedDetailFuts[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            State().detachedDetailFuts.erase(State().detachedDetailFuts.begin() +
                                       static_cast<std::vector<std::shared_future<DetailPack>>::difference_type>(i));
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < State().detachedClHoverFuts.size();) {
        if (!State().detachedClHoverFuts[i].valid() ||
            State().detachedClHoverFuts[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            State().detachedClHoverFuts.erase(
                State().detachedClHoverFuts.begin() +
                static_cast<std::vector<std::shared_future<P4ChangelistDetails>>::difference_type>(i));
        } else {
            ++i;
        }
    }

    std::lock_guard<std::mutex> lk(State().displayMutex);
    for (size_t i = 0; i < State().detailFuts.size(); ++i) {
        if (i >= State().detailPhase.size() || State().detailPhase[i] != 1) {
            continue;
        }
        if (!State().detailFuts[i].valid()) {
            continue;
        }
        if (State().detailFuts[i].wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            continue;
        }
        std::string pathForLog;
        if (i < State().displayRows.size()) {
            pathForLog = State().displayRows[i].PathForP4;
        }
        try {
            State().detailData[i] = State().detailFuts[i].get();
        } catch (const std::exception& ex) {
            LOG_WARN("Blame detail: idx=%zu path=%s async exception=%s", i, pathForLog.c_str(), ex.what());
            State().detailData[i].Error = std::string("detail load failed: ") + ex.what();
        } catch (...) {
            LOG_WARN("Blame detail: idx=%zu path=%s async unknown exception", i, pathForLog.c_str());
            State().detailData[i].Error = "detail load failed";
        }
        if (!State().detailData[i].Error.empty()) {
            LOG_WARN("Blame detail: idx=%zu path=%s err=%s", i, pathForLog.c_str(), State().detailData[i].Error.c_str());
        }
        State().detailPhase[i] = 2;
    }
}

bool ResolveP4UserForAssign(const AppController& app, const std::string& p4User, std::string& accountId, std::string& err) {
    accountId.clear();
    err.clear();
    if (p4User.empty() || p4User == "-") {
        err = "No Perforce user.";
        return false;
    }
    std::vector<TrackerUser> users;
    if (!app.SearchUsersByQuery(p4User, users, err)) {
        return false;
    }
    const std::string pl = ToLowerAsciiCopy(p4User);
    for (const auto& u : users) {
        size_t at = u.EmailAddress.find('@');
        const std::string local = at == std::string::npos ? u.EmailAddress : u.EmailAddress.substr(0, at);
        if (!local.empty() && ToLowerAsciiCopy(local) == pl) {
            accountId = u.AccountId;
            return true;
        }
    }
    if (!users.empty()) {
        accountId = users.front().AccountId;
        return true;
    }
    err = "No Jira user match.";
    return false;
}

std::string BuildAiExport() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    std::ostringstream oss;
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const BlameRow& r = State().displayRows[i];
        oss << "#" << (i + 1) << " " << r.Parsed.Function << "\n  " << r.PathForP4 << ":" << r.Parsed.LineNumber
            << "\n  User=" << r.Blame.User << " CL=" << r.Blame.Changelist << " Date=" << r.Blame.Date;
        if (r.Blame.Approximate) {
            oss << " [approximate]";
        }
        oss << "\n";
        if (!r.Blame.LineSnippet.empty()) {
            oss << "  " << r.Blame.LineSnippet << "\n";
        }
        if (i < State().detailData.size() && !State().detailData[i].Lines.empty()) {
            const int target = r.Parsed.LineNumber;
            for (const auto& ln : State().detailData[i].Lines) {
                if (std::abs(ln.SourceLine - target) <= 3 && !ln.Code.empty()) {
                    oss << "  L" << ln.SourceLine << " [" << ln.Changelist << "] " << ln.User << ": " << ln.Code
                        << "\n";
                }
            }
        }
        oss << "\n";
    }
    return oss.str();
}

std::string CsvEscape(const std::string& s) {
    bool needsQuotes = std::any_of(s.begin(), s.end(), [](char c) {
        return c == ',' || c == '"' || c == '\n' || c == '\r';
    });
    if (!needsQuotes) {
        return s;
    }
    std::string out;
    out.reserve(s.size() + 4);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string BuildBlameExportCsv() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    std::ostringstream oss;
    oss << "entry,function,path,line,user,changelist,date,approximate,line_snippet\n";
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const BlameRow& r = State().displayRows[i];
        oss << (i + 1) << "," << CsvEscape(r.Parsed.Function) << "," << CsvEscape(r.PathForP4) << ","
            << r.Parsed.LineNumber << "," << CsvEscape(r.Blame.User) << "," << CsvEscape(r.Blame.Changelist) << ","
            << CsvEscape(r.Blame.Date) << "," << (r.Blame.Approximate ? "true" : "false") << ","
            << CsvEscape(r.Blame.LineSnippet) << "\n";
    }
    return oss.str();
}

std::string BuildBlameExportJson() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    nlohmann::json root = nlohmann::json::object();
    root["entries"] = nlohmann::json::array();
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const BlameRow& r = State().displayRows[i];
        nlohmann::json entry = nlohmann::json::object();
        entry["entry"] = static_cast<int>(i + 1);
        entry["function"] = r.Parsed.Function;
        entry["path"] = r.PathForP4;
        entry["line"] = r.Parsed.LineNumber;
        entry["user"] = r.Blame.User;
        entry["changelist"] = r.Blame.Changelist;
        entry["date"] = r.Blame.Date;
        entry["approximate"] = r.Blame.Approximate;
        entry["line_snippet"] = r.Blame.LineSnippet;
        entry["nearby_lines"] = nlohmann::json::array();
        if (i < State().detailData.size() && !State().detailData[i].Lines.empty()) {
            const int target = r.Parsed.LineNumber;
            for (const auto& ln : State().detailData[i].Lines) {
                if (std::abs(ln.SourceLine - target) > 3 || ln.Code.empty()) {
                    continue;
                }
                entry["nearby_lines"].push_back(nlohmann::json{
                    {"line", ln.SourceLine}, {"changelist", ln.Changelist}, {"user", ln.User}, {"code", ln.Code}});
            }
        }
        root["entries"].push_back(std::move(entry));
    }
    return root.dump(2);
}

static std::string ReplaceStringPlaceholder(std::string str, const std::string& placeholder, const std::string& replacement) {
    size_t pos = 0;
    while ((pos = str.find(placeholder, pos)) != std::string::npos) {
        str.replace(pos, placeholder.length(), replacement);
        pos += replacement.length();
    }
    return str;
}

std::string BuildBlameQuickCommentTemplate(const std::string& issueKey, const std::string& templateId,
                                           const BlameRow& row, const std::vector<CommentTemplate>& templates) {
    std::string text;
    bool found = false;
    auto it = std::find_if(templates.begin(), templates.end(), [&templateId](const auto& t) {
        return t.Id == templateId;
    });
    if (it != templates.end()) {
        text = it->Text;
        found = true;
    }
    if (!found) {
        if (templateId == "need_repro") {
            text = "Need repro details for {key} (blame context: {path}:{line}, CL {cl}).";
        } else if (templateId == "need_logs") {
            text = "Please attach logs/diagnostics for {key} to continue triage.\nReference: {function} @ {path}:{line}.";
        } else {
            text = "Triage handoff for {key}:\n- Suggested owner: {user}\n- Suspect location: {function} ({path}:{line})\n- CL: {cl}";
        }
    }

    text = ReplaceStringPlaceholder(text, "{key}", issueKey);
    text = ReplaceStringPlaceholder(text, "{issueKey}", issueKey);
    text = ReplaceStringPlaceholder(text, "{path}", row.PathForP4);
    text = ReplaceStringPlaceholder(text, "{line}", std::to_string(row.Parsed.LineNumber));
    text = ReplaceStringPlaceholder(text, "{cl}", row.Blame.Changelist);
    text = ReplaceStringPlaceholder(text, "{function}", row.Parsed.Function);
    text = ReplaceStringPlaceholder(text, "{user}", row.Blame.User);

    return text;
}

ImVec4 ThCol(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

ImVec4 BlameLinkText(const BlameUiThemeColors& theme) { return ThCol(theme.ImportExisting); }

void PushBlameLinkButtonColors(const BlameUiThemeColors& theme) {
    const ImVec4 link = BlameLinkText(theme);
    ImGui::PushStyleColor(ImGuiCol_Text, link);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(link.x * 0.22f, link.y * 0.28f, link.z * 0.42f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(link.x * 0.34f, link.y * 0.42f, link.z * 0.58f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(link.x * 0.48f, link.y * 0.54f, link.z * 0.72f, 1.f));
}

void PopBlameLinkButtonColors() { ImGui::PopStyleColor(4); }

void PushBlameLinkTextOnly(const BlameUiThemeColors& theme) {
    ImGui::PushStyleColor(ImGuiCol_Text, BlameLinkText(theme));
}

void PopBlameLinkTextOnly() { ImGui::PopStyleColor(1); }

std::string NormalizeDateDisplay(const std::string& raw) {
    if (raw.size() >= 10 && raw[4] == '-' && raw[7] == '-') {
        return raw.substr(0, 4) + "/" + raw.substr(5, 2) + "/" + raw.substr(8, 2);
    }
    return raw;
}

std::string ShortenPathForDisplay(const std::string& path, float maxWidthPx) {
    if (path.empty() || maxWidthPx <= 8.f) {
        return path;
    }
    if (ImGui::CalcTextSize(path.c_str()).x <= maxWidthPx) {
        return path;
    }
    const std::string ell = "...";
    const float ellW = ImGui::CalcTextSize(ell.c_str()).x;
    if (maxWidthPx <= ellW + 4.f) {
        return ell;
    }
    const int n = static_cast<int>(path.size());
    std::string best = ell;
    for (int use = n; use >= 2; --use) {
        for (int pre = 1; pre < use; ++pre) {
            const int suf = use - pre;
            std::string trial =
                path.substr(0, static_cast<size_t>(pre)) + ell + path.substr(static_cast<size_t>(n - suf));
            if (ImGui::CalcTextSize(trial.c_str()).x <= maxWidthPx) {
                return trial;
            }
        }
    }
    return best;
}

void CloseBlameModal(bool* pOpen) {
    if (!pOpen) {
        return;
    }
    State().worker.Cancel = true;
    if (State().worker.Thread.joinable()) {
        State().worker.Thread.join();
    }
    {
        std::lock_guard<std::mutex> lk(State().worker.Mutex);
        State().worker.Rows.clear();
        State().worker.Progress = 0;
        State().worker.Total = 0;
    }
    {
        std::lock_guard<std::mutex> lk(State().displayMutex);
        for (auto& fut : State().detailFuts) {
            if (fut.valid() && fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                State().detachedDetailFuts.push_back(fut);
            }
        }
        State().displayRows.clear();
        State().detailFuts.clear();
        State().detailPhase.clear();
        State().detailData.clear();
        State().detailScrolled.clear();
    }
    State().clHoverCl.clear();
    if (State().clHoverFut.valid() && State().clHoverFut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        State().detachedClHoverFuts.push_back(State().clHoverFut);
    }
    State().clHoverFut = std::shared_future<P4ChangelistDetails>();
    std::memset(State().callstackBuf, 0, sizeof(State().callstackBuf));
    State().beforeDateIso.clear();
    State().atClBuf[0] = '\0';
    State().lastUiStatus.clear();
    State().pendingSelectEntryIndex = -1;
    State().lastCallstackIssueKey.clear();
    State().blameStreamlinedFromGrid = false;
    State().blamePendingAutoProcess = false;
    State().showRaw = false;
    *pOpen = false;
}

void OpenTrackerUserProfileForP4User(const AppController& app, const std::string& p4User) {
    State().openProfileModal = true;
    State().profileErr.clear();
    State().profileName.clear();
    State().profileEmail.clear();
    State().profileGroups.clear();
    if (p4User.empty() || p4User == "-" || p4User == "...") {
        State().profileName = "Past Employee";
        return;
    }
    std::vector<TrackerUser> users;
    std::string qerr;
    if (!app.SearchUsersByQuery(p4User, users, qerr) || users.empty()) {
        State().profileName = "Past Employee";
        State().profileEmail = p4User;
        if (!qerr.empty()) {
            State().profileErr = qerr;
        }
        return;
    }
    auto it = std::find_if(users.begin(), users.end(), [](const auto& u) {
        return !u.EmailAddress.empty();
    });
    const TrackerUser* best = (it != users.end()) ? &(*it) : &users[0];
    State().profileName = best->DisplayName;
    State().profileEmail = best->EmailAddress;
    std::string gerr;
    app.FetchUserGroupNames(best->AccountId, State().profileGroups, gerr);
    if (State().profileGroups.empty() && !gerr.empty()) {
        State().profileErr = gerr;
    }
}

void PrepareAssignModal(const AppController& app, const BlameRow& row, const std::string& p4UserCell) {
    State().assignRow = row;
    const std::string& pu = p4UserCell.empty() ? row.Blame.User : p4UserCell;
    State().assignAccountId.clear();
    State().assignHasJiraAccount = false;
    if (pu.empty() || pu == "-" || pu == "...") {
        State().assignTitle = "Past Employee";
        return;
    }
    std::vector<TrackerUser> users;
    std::string err;
    if (!app.SearchUsersByQuery(pu, users, err) || users.empty()) {
        State().assignTitle = std::string("Past Employee (") + pu + ")";
        return;
    }
    std::string aid;
    std::string e2;
    if (ResolveP4UserForAssign(app, pu, aid, e2) && !aid.empty()) {
        State().assignAccountId = std::move(aid);
        State().assignHasJiraAccount = true;
        std::string dn = users[0].DisplayName;
        auto it = std::find_if(users.begin(), users.end(), [](const auto& u) {
            return u.AccountId == State().assignAccountId;
        });
        if (it != users.end()) {
            dn = it->DisplayName;
        }
        State().assignTitle = dn + " (" + pu + ")";
    } else {
        State().assignTitle = std::string("Past Employee (") + pu + ")";
    }
}

std::string BuildCallstackRowTsv(const BlameRow& row, size_t displayIndex) {
    std::ostringstream o;
    o << (displayIndex + 1) << '\t' << row.Parsed.Function << '\t' << row.PathForP4 << ':' << row.Parsed.LineNumber
      << '\t' << row.Blame.User << '\t' << row.Blame.Changelist << '\t' << row.Blame.Date;
    return o.str();
}

static std::string SanitizeTsvCell(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return s;
}

static std::string BuildAnnotatedRowTsv(const P4AnnotatedLine& ln) {
    std::ostringstream o;
    o << ln.SourceLine << '\t' << SanitizeTsvCell(ln.Changelist) << '\t' << SanitizeTsvCell(ln.User) << '\t'
      << SanitizeTsvCell(ln.Date) << '\t' << SanitizeTsvCell(ln.Code);
    return o.str();
}

void DrawClTooltipAsync(const std::string& cl, const BlameAnalysisConfig& cfg, const BlameUiThemeColors& theme) {
    if (cl.empty()) {
        return;
    }
    if (State().clHoverCl != cl) {
        State().clHoverCl = cl;
        BlameAnalysisConfig cfgCopy = cfg;
        State().clHoverFut = std::async(std::launch::async, [cfgCopy, cl]() {
                           return State().tooltipClCache.GetOrFetch(cfgCopy, cl);
                       }).share();
    }
    ImGui::BeginTooltip();
    ImGui::TextDisabled("Left-click this changelist cell to open it in p4vc.");
    ImGui::Separator();
    const float wrapX = ImGui::GetCursorPosX() + 600.f;
    ImGui::PushTextWrapPos(wrapX);
    if (!State().clHoverFut.valid()) {
        ImGui::TextUnformatted("Loading CL info...");
    } else if (State().clHoverFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        ImGui::TextUnformatted("Loading CL info...");
    } else {
        try {
            const P4ChangelistDetails d = State().clHoverFut.get();
            if (!d.Error.empty()) {
                ImGui::TextUnformatted(d.Error.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.ClTooltipTitle));
                ImGui::Text("CL %s", cl.c_str());
                ImGui::PopStyleColor();
                if (!d.Author.empty()) {
                    ImGui::TextUnformatted(("by " + d.Author).c_str());
                }
                if (!d.Date.empty()) {
                    ImGui::TextUnformatted(d.Date.c_str());
                }
                if (!d.Description.empty()) {
                    ImGui::TextWrapped("%s", d.Description.c_str());
                }
                if (d.Author.empty() && d.Date.empty() && d.Description.empty()) {
                    ImGui::TextUnformatted("(no describe details)");
                }
            }
        } catch (const std::exception& ex) {
            LOG_WARN("Blame tooltip: changelist detail future exception: %s", ex.what());
            ImGui::TextUnformatted("Loading CL info...");
        } catch (...) {
            LOG_WARN("Blame tooltip: changelist detail future unknown exception");
            ImGui::TextUnformatted("Loading CL info...");
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

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
                // Stable ## id: display string can duplicate or change when Jira renames fields; never key off it alone.
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
    ImGui::InputText("Callstack field id (optional)", State().callstackTrackerFieldBuf, sizeof(State().callstackTrackerFieldBuf));
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
    ImGui::InputText("Last Found CL field id (optional)", State().lastFoundClFieldBuf, sizeof(State().lastFoundClFieldBuf));
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

} // namespace

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
    SyncCallstackTrackerFieldBufFromCfg();
    SyncJiraBlameAuxFieldBufsFromCfg();
    LogBlameP4PathsIfChanged("initial_load");
    cfgLoaded_ = true;
}

void BlameAnalysisUi::DrawBlamePreferencesTab(const AppController& app) {
    ensureSettingsBuffersLoaded();
    MaybeAutoselectCallstackTrackerField(app);
    MaybeAutoselectLastFoundClTrackerField(app);
    MaybeAutoselectLastOccurrencesTrackerField(app);
    ImGui::TextWrapped("Perforce paths, ignore list, and Jira callstack source used by Blame Analysis (stored in "
                       "smatchet_config.json).");
    ImGui::Spacing();
    const BlameUiThemeColors& theme = State().blameCfg.UiColors;
    DrawBlamePersistedOptionsForm(app, theme);
}

void BlameAnalysisUi::DrawWindow(AppController& app, bool* pOpen, const std::string& selectedJiraIssueKey) {
    if (!pOpen) {
        return;
    }
    if (!*pOpen) {
        CloseBlameModal(pOpen);
        return;
    }

    ensureSettingsBuffersLoaded();

    MaybeAutoselectCallstackTrackerField(app);
    MaybeAutoselectLastFoundClTrackerField(app);
    MaybeAutoselectLastOccurrencesTrackerField(app);

    const bool justOpened = !blameOpenPrev_;
    blameOpenPrev_ = true;
    if (justOpened || selectedJiraIssueKey != State().lastCallstackIssueKey) {
        State().lastCallstackIssueKey = selectedJiraIssueKey;
        TryFillCallstackFromJira(app, selectedJiraIssueKey);
        TryFillBeforeChangelistAndDateFromJira(app, selectedJiraIssueKey);
    }

    if (State().blamePendingAutoProcess && !State().worker.Running.load()) {
        RunBlameProcessFromBuffers();
        State().blamePendingAutoProcess = false;
    }

    const BlameUiThemeColors& theme = State().blameCfg.UiColors;

    // Dockable side-bar View — title "Source Blame", stable ID "BlameAnalysisModal".
    // First-time opens auto-dock into the primary side bar (right column, dock id 0x00000004
    // in kDefaultImGuiDockLayoutIni). FirstUseEver lets users move/undock manually.
    constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoCollapse;
    ImGui::SetNextWindowDockID(0x00000004u, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Source Blame###BlameAnalysisModal", pOpen, kPanelFlags)) {
        CloseBlameModal(pOpen);
        ImGui::End();
        return;
    }

    if (!*pOpen) {
        CloseBlameModal(pOpen);
        ImGui::End();
        return;
    }

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        CloseBlameModal(pOpen);
        ImGui::End();
        return;
    }

    const std::string titleIssue =
        selectedJiraIssueKey.empty() ? std::string("(no issue selected)") : selectedJiraIssueKey;
    ImGui::Text("Blame Analysis for: %s", titleIssue.c_str());
    ImGui::SameLine();
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float askW = ImGui::CalcTextSize("Ask AI").x + st.FramePadding.x * 2.f;
        const float exportJsonW = ImGui::CalcTextSize("Export JSON").x + st.FramePadding.x * 2.f;
        const float exportCsvW = ImGui::CalcTextSize("Export CSV").x + st.FramePadding.x * 2.f;
        const float closeW = ImGui::CalcTextSize("Close").x + st.FramePadding.x * 2.f;
        const float rowW = askW + exportJsonW + exportCsvW + closeW + st.ItemSpacing.x * 3.f;
        const float slack = ImGui::GetContentRegionAvail().x - rowW;
        if (slack > 0.f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + slack);
        }
        PushBlameLinkButtonColors(theme);
        if (ImGui::Button("Ask AI")) {
            const std::string payload = BuildAiExport();
            ImGui::SetClipboardText(payload.c_str());
            if (State().aiUrl[0] != '\0') {
                app.OpenUrl(std::string(State().aiUrl));
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "Copy the full blame export to the clipboard, then open the AI chat URL (if set under Preferences → "
                "Blame Analysis).");
        }
        ImGui::SameLine();
        if (ImGui::Button("Export JSON")) {
            ImGui::SetClipboardText(BuildBlameExportJson().c_str());
            State().lastUiStatus = "Blame JSON export copied to clipboard.";
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Copy structured blame export JSON (entries + nearby lines) to clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Export CSV")) {
            ImGui::SetClipboardText(BuildBlameExportCsv().c_str());
            State().lastUiStatus = "Blame CSV export copied to clipboard.";
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Copy one-row-per-entry blame export CSV to clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            PopBlameLinkButtonColors();
            CloseBlameModal(pOpen);
            ImGui::End();
            return;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Close Blame Analysis.");
        }
        PopBlameLinkButtonColors();
    }

    ImGui::Separator();

    {
        const TrackerConnectivityBannerForUi jiraBanner = app.GetTrackerConnectivityBannerForUi(nullptr);
        if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        } else if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Warning) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
    }

    if (!State().lastUiStatus.empty()) {
        ImGui::TextWrapped("%s", State().lastUiStatus.c_str());
        ImGui::Separator();
    }

    std::vector<BlameRow> rowsSnap;
    size_t nrow = 0;
    {
        std::lock_guard<std::mutex> lk(State().displayMutex);
        rowsSnap = State().displayRows;
        nrow = rowsSnap.size();
    }

    const bool busy = State().worker.Running.load();
    const int prog = State().worker.Progress.load();

    if (ImGui::BeginTabBar("blame_main_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Callstack")) {
            const bool streamlinedHide = State().blameStreamlinedFromGrid && !State().showRaw;

            ImGui::Text("Callstack Frames: %zu", nrow);
            {
                const ImGuiStyle& stBtn = ImGui::GetStyle();
                const float padH = stBtn.FramePadding.x * 2.f;
                const float callstackViewBtnW =
                    (std::max)(ImGui::CalcTextSize("Show Raw Text").x + padH,
                               (std::max)(ImGui::CalcTextSize("Show Table").x + padH,
                                          ImGui::CalcTextSize("Show raw callstack…").x + padH));
                if (!streamlinedHide) {
                    ImGui::SameLine();
                    PushBlameLinkButtonColors(theme);
                    if (State().showRaw) {
                        if (ImGui::Button("Show Table", ImVec2(callstackViewBtnW, 0.f))) {
                            State().showRaw = false;
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip(
                                "Return to the callstack table. The multiline field above the table becomes "
                                "editable again (when this tab shows it).");
                        }
                    } else {
                        if (ImGui::Button("Show Raw Text", ImVec2(callstackViewBtnW, 0.f))) {
                            State().showRaw = true;
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip(
                                "Show the callstack text in a wide, horizontally scrollable box (read-only in this "
                                "view). Use Show Table to edit the buffer in the smaller field.");
                        }
                    }
                    PopBlameLinkButtonColors();
                } else {
                    ImGui::SameLine();
                    PushBlameLinkButtonColors(theme);
                    if (ImGui::Button("Show raw callstack…", ImVec2(callstackViewBtnW, 0.f))) {
                        State().showRaw = true;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip(
                            "Reveal the callstack text, raw/table toggle, before changelist, and Process controls "
                            "(compact layout is used when blame is opened from the grid until you click this).");
                    }
                    PopBlameLinkButtonColors();
                }
            }

            if (!State().showRaw && nrow > 0) {
                ImGui::TextDisabled(
                    "Table: left-click the # column to open that frame in an Entry tab; right-click # for a menu "
                    "(Copy as TSV).");
            }

            if (!streamlinedHide) {
                ImGui::Spacing();
                ImGui::TextDisabled(
                    "Max frames, ignore list, P4 tools, and Jira callstack source: Settings → Preferences → Blame "
                    "Analysis.");
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Before changelist");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "Optional: run annotate and blame as of this Perforce changelist (each path is passed as "
                        "`depot/path@CL`).\n\n"
                        "Digits only; leave empty for the current head on each path.\n\n"
                        "The date control on the right resolves a calendar day to the first submitted changelist on "
                        "that day (using `p4 changes -r -m 1 -s submitted` on a server-wide `//...@start,end` range) "
                        "and copies the result into this field.");
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.f);
                ImGui::InputText("##before_cl_optional", State().atClBuf, sizeof(State().atClBuf));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("Perforce changelist number only (decimal digits).");
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("or day");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "Pick a calendar day, then confirm in the calendar popup. The first submitted changelist "
                        "on that day (server date window) is written into the field on the left.");
                }
                ImGui::SameLine();
                if (TrackerDateTimeFieldEditor::RenderGenericDatePicker("##blame_before_day", State().beforeDateIso, false,
                                                                         228.f)) {
                    ParsedJiraDateTime parsed;
                    if (TryParseJiraDateTime(State().beforeDateIso, parsed)) {
                        std::string cl;
                        std::string err;
                        if (P4FirstSubmittedChangelistOnCalendarDay(State().blameCfg, parsed.Year, parsed.Month, parsed.Day,
                                                                    cl, err)) {
                            std::snprintf(State().atClBuf, sizeof(State().atClBuf), "%s", cl.c_str());
                            State().lastUiStatus = "Before changelist set to first submitted CL on that day: " + cl;
                        } else {
                            State().lastUiStatus = err.empty() ? "Could not resolve changelist for that date." : err;
                        }
                    } else {
                        State().lastUiStatus = "Invalid date from picker.";
                    }
                }

                PushBlameLinkButtonColors(theme);
                if (ImGui::Button("Process") && !busy) {
                    RunBlameProcessFromBuffers();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !busy) {
                    ImGui::SetTooltip(
                        "Parse the callstack buffer and fetch Perforce blame for each frame (options under "
                        "Preferences → Blame Analysis). If you opened blame from the grid with a callstack field, "
                        "this usually runs once automatically after the buffer fills.");
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") && busy) {
                    State().worker.Cancel = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && busy) {
                    ImGui::SetTooltip("Stop the in-progress blame worker.");
                }
                PopBlameLinkButtonColors();
            } else if (busy) {
                PushBlameLinkButtonColors(theme);
                if (ImGui::Button("Cancel") && busy) {
                    State().worker.Cancel = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("Stop the in-progress blame worker.");
                }
                PopBlameLinkButtonColors();
            }

            float rawFieldW = -1.f;
            float rawMaxLineW = 0.f;
            if (!streamlinedHide) {
                if (State().showRaw) {
                    for (const char* p = State().callstackBuf; *p != '\0';) {
                        const char* nl = std::strchr(p, '\n');
                        const char* end = nl ? nl : p + std::strlen(p);
                        const ImVec2 sz = ImGui::CalcTextSize(p, end);
                        rawMaxLineW = std::max(rawMaxLineW, sz.x);
                        if (!nl) {
                            break;
                        }
                        p = nl + 1;
                    }
                    const float cap = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.f;
                    rawFieldW = std::min(std::max(rawMaxLineW + ImGui::GetStyle().FramePadding.x * 2.f, 120.f), cap);
                }

                if (State().showRaw) {
                    ImGui::BeginChild("##rawcs_scroll", ImVec2(rawFieldW, 220.f), ImGuiChildFlags_None,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    const ImVec2 inner = ImGui::GetContentRegionAvail();
                    const float inputW = std::max(inner.x, rawMaxLineW + ImGui::GetStyle().FramePadding.x * 2.f + 8.f);
                    ImGui::InputTextMultiline("##callstackpaste", State().callstackBuf, sizeof(State().callstackBuf),
                                              ImVec2(inputW, inner.y), ImGuiInputTextFlags_ReadOnly);
                    ImGui::EndChild();
                } else {
                    ImGui::InputTextMultiline("##callstackpaste", State().callstackBuf, sizeof(State().callstackBuf),
                                              ImVec2(-1.f, 120.f), 0);
                }
            }

            if (!State().showRaw && nrow > 0) {
                // Keep Process / callstack editor above a dedicated scroll region. A tall in-table
                // ScrollY made the whole tab body exceed the viewport so the window scroll bar hid
                // the controls at the top (Process looked "missing").
                const float tblScrollH = std::max(ImGui::GetContentRegionAvail().y - 4.f, 80.f);
                ImGui::BeginChild("##blame_callstack_tbl_scroll", ImVec2(0.f, tblScrollH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_None);
                const float locColW = 250.f;
                if (ImGui::BeginTable("blame_tbl", 6,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
                                      ImVec2(-1.f, 0.f))) {
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.f, 0);
                    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 0.f, 1);
                    ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, locColW, 2);
                    ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed, 120.f, 3);
                    ImGui::TableSetupColumn("CL", ImGuiTableColumnFlags_WidthFixed, 70.f, 4);
                    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 90.f, 5);
                    ImGui::TableHeadersRow();

                    const float rowH = ImGui::GetTextLineHeightWithSpacing();
                    for (size_t i = 0; i < nrow; ++i) {
                        const BlameRow& row = rowsSnap[i];
                        const bool pending = busy && static_cast<int>(i) >= prog;
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        char idxBuf[16];
                        std::snprintf(idxBuf, sizeof(idxBuf), "%u", static_cast<unsigned>(i + 1));
                        PushBlameLinkTextOnly(theme);
                        ImGui::SelectableRaw(idxBuf, false,
                                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                              ImVec2(0.f, rowH));
                        PopBlameLinkTextOnly();
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                            State().pendingSelectEntryIndex = static_cast<int>(i);
                            EnsureDetailLoading(i, State().blameCfg, std::string(State().atClBuf));
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                            ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
                            ImGui::OpenPopup("blame_cs_row_copy");
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("Left-click: open the Entry tab for this frame and load its annotation.\n"
                                              "Right-click: menu with Copy (this row as TSV).");
                        }
                        if (ImGui::BeginPopup("blame_cs_row_copy")) {
                            if (ImGui::MenuItem("Copy")) {
                                ImGui::SetClipboardText(BuildCallstackRowTsv(row, i).c_str());
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemAllowOverlap();
                        {
                            const char* fn = row.Parsed.Function.c_str();
                            const float colAvail = ImGui::GetContentRegionAvail().x;
                            const float lineH = ImGui::GetTextLineHeight();
                            PushBlameLinkTextOnly(theme);
                            if (ImGui::SelectableRaw(fn, false, ImGuiSelectableFlags_AllowOverlap,
                                                   ImVec2(colAvail, lineH))) {
                                /* click handled below */
                            }
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemClicked()) {
                                ImGui::SetClipboardText(fn);
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("Left-click: copy the function name to the clipboard.\n\n%s", fn);
                            }
                        }

                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemAllowOverlap();
                        {
                            const std::string fullLoc = row.PathForP4 + ":" + std::to_string(row.Parsed.LineNumber);
                            const float locCellW = ImGui::GetColumnWidth();
                            const std::string shortPath =
                                ShortenPathForDisplay(row.PathForP4, std::max(32.f, locCellW - 40.f));
                            const std::string shortLoc = shortPath + ":" + std::to_string(row.Parsed.LineNumber);
                            PushBlameLinkTextOnly(theme);
                            if (ImGui::SelectableRaw(shortLoc.c_str(), false, ImGuiSelectableFlags_AllowOverlap)) {
                                LaunchP4VcLike(State().blameCfg, State().timeTpl, State().changeTpl, true, row.PathForP4,
                                               row.Parsed.LineNumber, row.Blame.Changelist);
                            }
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("Left-click: open p4vc timelapse for this file and line.\n\n%s",
                                                  fullLoc.c_str());
                            }
                        }

                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemAllowOverlap();
                        {
                            const std::string userDisp =
                                pending ? std::string("...")
                                        : (row.Blame.User.empty() ? std::string("-") : row.Blame.User);
                            const bool userActionable =
                                !pending && !row.Blame.User.empty() && row.Blame.User != "..." && row.Blame.User != "-";
                            if (pending || row.Blame.User.empty() || row.Blame.User == "-") {
                                ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            } else {
                                PushBlameLinkTextOnly(theme);
                            }
                            ImGui::SelectableRaw(userDisp.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
                            if (pending || row.Blame.User.empty() || row.Blame.User == "-") {
                                ImGui::PopStyleColor();
                            } else {
                                PopBlameLinkTextOnly();
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                if (pending) {
                                    ImGui::SetTooltip("Waiting for blame results for this row.");
                                } else if (userActionable) {
                                    ImGui::SetTooltip("Left-click: look up this Perforce user in Jira.\n"
                                                      "Right-click: open the assign dialog for this row.\n%s",
                                                      row.Blame.Approximate
                                                          ? "\nApproximate blame (line may not match exact CL)."
                                                          : "");
                                } else {
                                    ImGui::SetTooltip("No Perforce user on this row.");
                                }
                            }
                            if (!pending && ImGui::IsItemClicked() && !row.Blame.User.empty() &&
                                row.Blame.User != "...") {
                                OpenTrackerUserProfileForP4User(app, row.Blame.User);
                            }
                            if (ImGui::IsMouseClicked(1) && ImGui::IsItemHovered()) {
                                PrepareAssignModal(app, row, pending ? std::string() : row.Blame.User);
                                ImGui::OpenPopup("blame_assign");
                            }
                        }

                        ImGui::TableSetColumnIndex(4);
                        ImGui::SetNextItemAllowOverlap();
                        {
                            const std::string clDisp =
                                pending ? std::string("...")
                                        : (row.Blame.Changelist.empty() ? std::string("-") : row.Blame.Changelist);
                            if (pending || row.Blame.Changelist.empty()) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            } else {
                                PushBlameLinkTextOnly(theme);
                            }
                            ImGui::SelectableRaw(clDisp.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
                            if (pending || row.Blame.Changelist.empty()) {
                                ImGui::PopStyleColor();
                            } else {
                                PopBlameLinkTextOnly();
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                if (pending) {
                                    ImGui::SetTooltip("Waiting for blame results for this row.");
                                } else if (row.Blame.Changelist.empty()) {
                                    ImGui::SetTooltip("No changelist on this row.");
                                } else {
                                    DrawClTooltipAsync(row.Blame.Changelist, State().blameCfg, theme);
                                }
                            }
                            if (ImGui::IsItemClicked() && !pending && !row.Blame.Changelist.empty()) {
                                LaunchP4VcLike(State().blameCfg, State().timeTpl, State().changeTpl, false, row.PathForP4,
                                               row.Parsed.LineNumber, row.Blame.Changelist);
                            }
                        }

                        ImGui::TableSetColumnIndex(5);
                        ImGui::SetNextItemAllowOverlap();
                        if (pending) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::TextUnformatted("-");
                            ImGui::PopStyleColor();
                        } else if (row.Blame.Date.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::TextUnformatted("-");
                            ImGui::PopStyleColor();
                        } else {
                            const std::string dd = NormalizeDateDisplay(row.Blame.Date);
                            ImGui::TextUnformatted(dd.c_str());
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
            }

            ImGui::EndTabItem();
        }

        for (size_t ti = 0; ti < nrow; ++ti) {
            char tabName[80];
            std::snprintf(tabName, sizeof(tabName), "Entry %zu##BlameEntry%zu", ti + 1, ti);
            ImGuiTabItemFlags tflags = ImGuiTabItemFlags_None;
            if (State().pendingSelectEntryIndex == static_cast<int>(ti)) {
                tflags = ImGuiTabItemFlags_SetSelected;
            }
            if (ImGui::BeginTabItem(tabName, nullptr, tflags)) {
                if (State().pendingSelectEntryIndex == static_cast<int>(ti)) {
                    State().pendingSelectEntryIndex = -1;
                }
                EnsureDetailLoading(ti, State().blameCfg, std::string(State().atClBuf));
                BlameRow row = rowsSnap[ti];
                ImGui::Text("File: %s", row.PathForP4.c_str());
                ImGui::Text("Target Line: %d", row.Parsed.LineNumber);

                int phase = 0;
                if (ti < State().detailPhase.size()) {
                    phase = State().detailPhase[ti];
                }
                if (phase == 1) {
                    ImGui::TextUnformatted("Loading annotated file...");
                } else if (ti < State().detailData.size() && !State().detailData[ti].Error.empty()) {
                    ImGui::TextColored(ThCol(theme.StatusError), "%s", State().detailData[ti].Error.c_str());
                } else if (ti < State().detailData.size() &&
                           ImGui::BeginTable("ann", 6,
                                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                 ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                                             ImVec2(0, ImGui::GetContentRegionAvail().y - 8.f))) {
                    ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 30.f);
                    ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 60.f);
                    ImGui::TableSetupColumn("CL", ImGuiTableColumnFlags_WidthFixed, 70.f);
                    ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed, 120.f);
                    ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 100.f);
                    ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    const int targetLine = row.Parsed.LineNumber;
                    const std::vector<P4AnnotatedLine>& lines = State().detailData[ti].Lines;
                    const ImU32 hlU32 = ImGui::ColorConvertFloat4ToU32(ThCol(theme.FindHighlight));
                    const float annRowHitH = ImGui::GetTextLineHeightWithSpacing();
                    auto annRowOpenCopyMenu = []() {
                        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                            ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
                            ImGui::OpenPopup("blame_ann_row_copy");
                        }
                    };
                    auto annRowHoverTip = []() {
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip(
                                "Right-click any cell in this row for a menu: Copy (full line as TSV). Assign... "
                                "appears only when the Perforce user can be used for assign (not '-' or '...').");
                        }
                    };
                    for (size_t lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
                        const P4AnnotatedLine& ln = lines[lineIdx];
                        ImGui::PushID(static_cast<int>(lineIdx));
                        ImGui::TableNextRow();
                        const bool hl = (ln.SourceLine == targetLine);
                        if (hl) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, hlU32);
                        }

                        ImGui::TableSetColumnIndex(0);
                        {
                            const float markW = ImGui::GetContentRegionAvail().x;
                            if (hl) {
                                ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.StatusWarning));
                            }
                            PushBlameLinkTextOnly(theme);
                            ImGui::SelectableRaw(hl ? ">>>" : " ", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2((std::max)(markW, 1.f), annRowHitH));
                            PopBlameLinkTextOnly();
                            if (hl) {
                                ImGui::PopStyleColor();
                            }
                            annRowOpenCopyMenu();
                            annRowHoverTip();
                        }

                        ImGui::TableSetColumnIndex(1);
                        {
                            char lineBuf[32];
                            std::snprintf(lineBuf, sizeof(lineBuf), "%d", ln.SourceLine);
                            const float lineColW = ImGui::GetContentRegionAvail().x;
                            PushBlameLinkTextOnly(theme);
                            ImGui::SelectableRaw(lineBuf, false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2((std::max)(lineColW, 1.f), annRowHitH));
                            PopBlameLinkTextOnly();
                            annRowOpenCopyMenu();
                            annRowHoverTip();
                        }

                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemAllowOverlap();
                        const float clCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
                        if (ln.Changelist.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::SelectableRaw("-", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(clCellW, annRowHitH));
                            ImGui::PopStyleColor();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("No changelist on this line. Right-click for row menu (Copy).");
                            }
                        } else {
                            PushBlameLinkTextOnly(theme);
                            if (ImGui::SelectableRaw(ln.Changelist.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                                                     ImVec2(clCellW, annRowHitH))) {
                                LaunchP4VcLike(State().blameCfg, State().timeTpl, State().changeTpl, false, row.PathForP4, ln.SourceLine,
                                               ln.Changelist);
                            }
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                DrawClTooltipAsync(ln.Changelist, State().blameCfg, theme);
                            }
                        }
                        annRowOpenCopyMenu();

                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemAllowOverlap();
                        const float userCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
                        if (ln.User.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::SelectableRaw("-", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(userCellW, annRowHitH));
                            ImGui::PopStyleColor();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                std::string tip = "No Perforce user on this line. Right-click for row menu (Copy).";
                                if (hl && row.Blame.Approximate) {
                                    tip += "\n\nApproximate row: unable to find exact CL and user.";
                                }
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                        } else {
                            PushBlameLinkTextOnly(theme);
                            ImGui::SelectableRaw(ln.User.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(userCellW, annRowHitH));
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                std::string tip = "Left-click: look up this Perforce user in Jira.\n"
                                                  "Right-click: row menu - Copy (full line as TSV); Assign... when the "
                                                  "user is eligible (not '-' or '...').";
                                if (hl && row.Blame.Approximate) {
                                    tip += "\n\nApproximate row: unable to find exact CL and user.";
                                }
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                                OpenTrackerUserProfileForP4User(app, ln.User);
                            }
                        }
                        annRowOpenCopyMenu();

                        ImGui::TableSetColumnIndex(4);
                        const float dateCellW = (std::max)(ImGui::GetContentRegionAvail().x, 1.f);
                        if (ln.Date.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::SelectableRaw("-", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(dateCellW, annRowHitH));
                            ImGui::PopStyleColor();
                        } else {
                            const std::string dd = NormalizeDateDisplay(ln.Date);
                            ImGui::SelectableRaw(dd.c_str(), false, ImGuiSelectableFlags_AllowOverlap,
                                                  ImVec2(dateCellW, annRowHitH));
                        }
                        annRowOpenCopyMenu();
                        annRowHoverTip();

                        ImGui::TableSetColumnIndex(5);
                        {
                            const float codeColW = ImGui::GetContentRegionAvail().x;
                            const ImVec2 codeCell0 = ImGui::GetCursorScreenPos();
                            BlameDrawColoredCppLine(ln.Code.c_str(), theme);
                            ImGui::SetCursorScreenPos(codeCell0);
                            ImGui::SelectableRaw("##ann_code_rmb", false, ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2((std::max)(codeColW, 1.f), annRowHitH));
                            annRowOpenCopyMenu();
                            annRowHoverTip();
                        }

                        if (ImGui::BeginPopup("blame_ann_row_copy")) {
                            if (ImGui::MenuItem("Copy")) {
                                ImGui::SetClipboardText(BuildAnnotatedRowTsv(ln).c_str());
                            }
                            const bool canAssign = !ln.User.empty() && ln.User != "-" && ln.User != "...";
                            if (canAssign) {
                                if (ImGui::MenuItem("Assign...")) {
                                    BlameRow br = row;
                                    br.Blame.User = ln.User;
                                    br.Blame.Changelist = ln.Changelist;
                                    br.Parsed.LineNumber = ln.SourceLine;
                                    br.Blame.LineSnippet = ln.Code;
                                    br.Blame.Date = ln.Date;
                                    br.Blame.Approximate = false;
                                    PrepareAssignModal(app, br, ln.User);
                                    ImGui::CloseCurrentPopup();
                                    ImGui::OpenPopup("blame_assign");
                                }
                            }
                            ImGui::EndPopup();
                        }

                        if (hl && ti < State().detailScrolled.size() && !State().detailScrolled[ti]) {
                            ImGui::SetScrollHereY(0.5f);
                            State().detailScrolled[ti] = true;
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    if (ImGui::BeginPopupModal("blame_assign", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const TrackerConnectivityBannerForUi jiraBanner = app.GetTrackerConnectivityBannerForUi(nullptr);
        const TrackerConfig cfg = ConfigManager::Load();
        const bool readOnlyMode = cfg.ReadOnlyMode || (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Error);
        ImGui::TextUnformatted(State().assignTitle.c_str());
        ImGui::Separator();
        if (cfg.ReadOnlyMode) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.22f, 1.0f));
            ImGui::TextWrapped("Read-only mode is enabled in Preferences.");
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Assign and comment actions stay disabled until read-only mode is turned off.");
            ImGui::Separator();
        }
        if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Assign and comment actions stay disabled until Jira is reachable.");
            ImGui::Separator();
        } else if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Warning) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
        if (selectedJiraIssueKey.empty()) {
            ImGui::TextDisabled("Select a Jira issue in the grid.");
        } else {
            const bool hasJiraAccount = State().assignHasJiraAccount && !State().assignAccountId.empty();
            PushBlameLinkTextOnly(theme);
            ImGui::BeginDisabled(readOnlyMode);
            if (ImGui::Selectable("Assign issue to user", false)) {
                std::string err;
                const TrackerField* f = app.FindFieldById("assignee");
                if (!hasJiraAccount) {
                    LOG_ERROR("Blame UI: assign skipped — no Jira account match for this Perforce user.");
                    State().lastUiStatus = "No Jira user match for assign.";
                } else if (!f) {
                    LOG_ERROR("Blame UI: assignee field not in catalog.");
                    State().lastUiStatus = "assignee field not in catalog.";
                } else if (app.SubmitFieldEdit(selectedJiraIssueKey, *f, {State().assignAccountId}, err)) {
                    LOG_INFO("Blame UI: assignee set on %s", selectedJiraIssueKey.c_str());
                    State().lastUiStatus = "Assignee updated.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: assign failed: %s", err.c_str());
                    State().lastUiStatus = err;
                }
            }
            ImGui::EndDisabled();
            PopBlameLinkTextOnly();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "Set the Jira assignee on the selected issue to the Jira user matched from the Perforce user on "
                    "the blame row you used (callstack table or Entry tab row menu).\n"
                    "Requires a matching Jira account; otherwise an error is shown.");
            }
            ImGui::BeginDisabled(readOnlyMode);
            if (ImGui::Selectable("Add blame context comment", false)) {
                std::string err;
                if (app.AddIssueCommentBlameContext(
                        selectedJiraIssueKey, State().assignRow.Blame.User, State().assignRow.Parsed.Function,
                        State().assignRow.PathForP4, State().assignRow.Parsed.LineNumber, State().assignRow.Blame.Changelist,
                        State().assignRow.Blame.Date, State().assignRow.Blame.Approximate, State().assignRow.Blame.LineSnippet, err)) {
                    LOG_INFO("Blame UI: posted blame context comment for %s.", selectedJiraIssueKey.c_str());
                    State().lastUiStatus = "Blame context comment posted.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: comment failed: %s", err.c_str());
                    State().lastUiStatus = err;
                }
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextDisabled("Quick comment templates");
            ImGui::BeginDisabled(readOnlyMode);
            {
                const TrackerConfig jiraCfg = cfg;
                int blameTplIndex = 0;
                for (const auto& t : jiraCfg.BlameCommentTemplates) {
                    if (!t.Id.empty()) {
                        ImGui::PushID(t.Id.c_str());
                    } else {
                        ImGui::PushID(blameTplIndex);
                    }
                    if (ImGui::SelectableRaw(t.Title.c_str(), false)) {
                        std::string err;
                        std::string commentBody = BuildBlameQuickCommentTemplate(selectedJiraIssueKey, t.Id, State().assignRow, jiraCfg.BlameCommentTemplates);
                        if (app.AddIssueCommentPlain(selectedJiraIssueKey, commentBody, err)) {
                            State().lastUiStatus = "Posted '" + t.Title + "' comment.";
                            ImGui::CloseCurrentPopup();
                        } else {
                            State().lastUiStatus = err.empty() ? "Failed to post Jira comment." : err;
                        }
                    }
                    ImGui::PopID();
                    ++blameTplIndex;
                }
            }
            ImGui::EndDisabled();
            if (readOnlyMode && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Disabled while Jira is in read-only mode.");
            }
            if (hasJiraAccount) {
                PushBlameLinkTextOnly(theme);
            }
            ImGui::BeginDisabled(!hasJiraAccount || readOnlyMode);
            if (ImGui::Selectable("Assign and add blame context", false)) {
                std::string err;
                const TrackerField* f = app.FindFieldById("assignee");
                bool assigned = true;
                if (!f) {
                    err = "assignee field not in catalog.";
                    assigned = false;
                } else {
                    assigned = app.SubmitFieldEdit(selectedJiraIssueKey, *f, {State().assignAccountId}, err);
                }
                if (assigned &&
                    app.AddIssueCommentBlameContext(
                        selectedJiraIssueKey, State().assignRow.Blame.User, State().assignRow.Parsed.Function,
                        State().assignRow.PathForP4, State().assignRow.Parsed.LineNumber, State().assignRow.Blame.Changelist,
                        State().assignRow.Blame.Date, State().assignRow.Blame.Approximate, State().assignRow.Blame.LineSnippet, err)) {
                    LOG_INFO("Blame UI: assigned %s and posted blame context comment.", selectedJiraIssueKey.c_str());
                    State().lastUiStatus = "Assigned and commented.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: assign/comment failed: %s", err.c_str());
                    State().lastUiStatus = err;
                }
            }
            ImGui::EndDisabled();
            if (hasJiraAccount) {
                PopBlameLinkTextOnly();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                if (hasJiraAccount) {
                    ImGui::SetTooltip("Assign the issue, then add a Jira comment summarizing blame context "
                                      "(user, function, path, line, CL, date).");
                } else {
                    ImGui::SetTooltip("Enable this action by matching the Perforce user to a Jira account.");
                }
            }
        }
        PushBlameLinkButtonColors(theme);
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Close this dialog without making further changes.");
        }
        PopBlameLinkButtonColors();
        ImGui::EndPopup();
    }

    if (State().openProfileModal) {
        ImGui::OpenPopup("Jira user profile");
        State().openProfileModal = false;
    }
    if (ImGui::BeginPopupModal("Jira user profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!State().profileErr.empty() && State().profileName.empty()) {
            ImGui::TextUnformatted(State().profileErr.c_str());
        } else {
            ImGui::Text("Name: %s", State().profileName.c_str());
            ImGui::Text("Email: %s", State().profileEmail.c_str());
            if (!State().profileErr.empty()) {
                ImGui::TextDisabled("%s", State().profileErr.c_str());
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Groups (best effort):");
            if (State().profileGroups.empty()) {
                ImGui::TextDisabled("(none or not permitted)");
            } else {
                for (const auto& gname : State().profileGroups) {
                    ImGui::BulletText("%s", gname.c_str());
                }
            }
        }
        PushBlameLinkButtonColors(theme);
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Close the Jira user profile dialog.");
        }
        PopBlameLinkButtonColors();
        ImGui::EndPopup();
    }

    ImGui::End();
}

bool BlameRowHasNonEmptyCallstackField(const AppController& app, const CachedTicket& ticket) {
    HydrateBlameCfgDiskOnce();
    std::string fid = State().blameCfg.CallstackTrackerFieldId;
    if (fid.empty()) {
        const auto& fields = app.GetAvailableFields();
        const auto it = std::find_if(fields.begin(), fields.end(), [](const TrackerField& f) {
            return ToLowerAsciiCopy(f.Name) == "callstack";
        });
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

