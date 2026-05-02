#include "BlameAnalysisUi.h"

#include "AppController.h"
#include "BlameSyntaxHighlight.h"
#include "CallstackParser.h"
#include "ConfigManager.h"
#include "JiraClient.h"
#include "Logger.h"
#include "P4Blame.h"
#include "StringUtil.h"
#include "imgui.h"
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

namespace {

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

static WorkerState g_worker;
static P4ChangelistDescribeCache g_tooltipClCache(512);

static char g_callstackBuf[65536]{};
static std::vector<char> g_ignoreBuf(4096, '\0');
static char g_atClBuf[64]{};
static int g_maxFramesVal = 64;
static char g_remapFrom[512]{};
static char g_remapTo[512]{};
static char g_p4Exe[260]{};
static char g_p4vcExe[260]{};
static char g_timeTpl[1024]{};
static char g_changeTpl[512]{};
static char g_aiUrl[512]{};
static bool g_showRaw = false;
static BlameAnalysisConfig g_blameCfg;

static std::string g_loggedP4Exe;
static std::string g_loggedP4vcExe;

static void LogBlameP4PathsIfChanged(const char* reason) {
    if (g_blameCfg.P4Executable != g_loggedP4Exe) {
        LOG_INFO("Blame [%s]: p4_exe \"%s\" -> \"%s\"", reason, g_loggedP4Exe.c_str(), g_blameCfg.P4Executable.c_str());
        g_loggedP4Exe = g_blameCfg.P4Executable;
    }
    if (g_blameCfg.P4VcExecutable != g_loggedP4vcExe) {
        LOG_INFO("Blame [%s]: p4vc_exe \"%s\" -> \"%s\"", reason, g_loggedP4vcExe.c_str(),
                 g_blameCfg.P4VcExecutable.c_str());
        g_loggedP4vcExe = g_blameCfg.P4VcExecutable;
    }
}

static std::mutex g_displayMutex;
static std::vector<BlameRow> g_displayRows;
static std::vector<std::shared_future<DetailPack>> g_detailFuts;
static std::vector<std::shared_future<DetailPack>> g_detachedDetailFuts;
static std::vector<int> g_detailPhase;
static std::vector<DetailPack> g_detailData;
static std::vector<bool> g_detailScrolled;

static std::string g_profileName;
static std::string g_profileEmail;
static std::string g_profileErr;
static std::vector<std::string> g_profileGroups;
static bool g_openProfileModal = false;

static std::string g_lastUiStatus;
static int g_pendingSelectEntryIndex = -1;

static std::string g_clHoverCl;
static std::shared_future<P4ChangelistDetails> g_clHoverFut;
static std::vector<std::shared_future<P4ChangelistDetails>> g_detachedClHoverFuts;

static std::string g_assignTitle;
static std::string g_assignAccountId;
static bool g_assignHasJiraAccount = false;
static BlameRow g_assignRow;

static char g_callstackJiraFieldBuf[260]{};
static std::string s_lastCallstackIssueKey;

template <size_t N> void CopyToBuffer(char (&dst)[N], const std::string& src) {
    static_assert(N > 0, "CopyToBuffer requires a non-empty char array");
    std::memset(dst, 0, N);
    const size_t cap = N - 1;
    const size_t n = (std::min)(src.size(), cap);
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
}

void SyncCallstackJiraFieldBufFromCfg() { CopyToBuffer(g_callstackJiraFieldBuf, g_blameCfg.CallstackJiraFieldId); }

void MaybeAutoselectCallstackJiraField(AppController& app) {
    if (!g_blameCfg.CallstackJiraFieldId.empty()) {
        return;
    }
    const auto& fields = app.GetAvailableFields();
    if (fields.empty()) {
        return;
    }
    for (const auto& f : fields) {
        if (ToLowerAsciiCopy(f.Name) == "callstack") {
            g_blameCfg.CallstackJiraFieldId = f.Id;
            SyncCallstackJiraFieldBufFromCfg();
            ConfigManager::SaveBlameAnalysis(g_blameCfg);
            break;
        }
    }
}

void TryFillCallstackFromJira(AppController& app, const std::string& issueKey) {
    if (g_blameCfg.CallstackJiraFieldId.empty() || issueKey.empty()) {
        return;
    }
    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    for (const auto& t : *ticketsSnap) {
        if (t.id != issueKey) {
            continue;
        }
        const std::string v = t.GetFieldValue(g_blameCfg.CallstackJiraFieldId);
        if (v.empty()) {
            return;
        }
        CopyToBuffer(g_callstackBuf, v);
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
    if (g_worker.Thread.joinable() && !g_worker.Running.load()) {
        g_worker.Thread.join();
    }
}

void MirrorWorkerToDisplay() {
    std::lock_guard<std::mutex> lkW(g_worker.Mutex);
    if (g_worker.Rows.empty() && !g_worker.Running.load()) {
        return;
    }
    std::lock_guard<std::mutex> lkD(g_displayMutex);
    g_displayRows = g_worker.Rows;
}

void ResetDetailAfterRunComplete() {
    if (!g_worker.PendingPublish.exchange(false)) {
        return;
    }
    std::lock_guard<std::mutex> lkD(g_displayMutex);
    const size_t n = g_displayRows.size();
    g_detailFuts.assign(n, std::shared_future<DetailPack>());
    g_detailPhase.assign(n, 0);
    g_detailData.assign(n, DetailPack{});
    g_detailScrolled.assign(n, false);
}

void WorkerThreadMain(size_t n) {
    BlameAnalysisConfig cfg = g_worker.Cfg;
    const std::string atCl = g_worker.AtChangelist;
    P4ChangelistDescribeCache* cache = g_worker.Cache.get();
    LOG_INFO("Blame worker: started rows=%zu atCl=\"%s\" p4_exe=\"%s\"", n, atCl.c_str(), cfg.P4Executable.c_str());
    int failures = 0;
    try {
        for (size_t i = 0; i < n; ++i) {
            if (g_worker.Cancel.load()) {
                LOG_INFO("Blame worker: cancelled at row %zu/%zu (failures=%d)", i, n, failures);
                break;
            }
            BlameRow row;
            {
                std::lock_guard<std::mutex> lk(g_worker.Mutex);
                if (i >= g_worker.Rows.size()) {
                    break;
                }
                row = g_worker.Rows[i];
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
                std::lock_guard<std::mutex> lk(g_worker.Mutex);
                if (i < g_worker.Rows.size()) {
                    g_worker.Rows[i].Blame = std::move(b);
                }
            }
            g_worker.Progress = static_cast<int>(i + 1);
        }
        if (!g_worker.Cancel.load()) {
            LOG_INFO("Blame worker: finished rows=%zu failures=%d", n, failures);
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("Blame worker: exception: %s", ex.what());
    } catch (...) {
        LOG_ERROR("Blame worker: unknown exception");
    }
    g_worker.PendingPublish = true;
    g_worker.Running = false;
}

void StartWorker(std::vector<BlameRow> rows, BlameAnalysisConfig cfg, std::string atCl) {
    JoinWorkerIfNeeded();
    LOG_INFO("Blame: StartWorker rows=%zu atCl=\"%s\"", rows.size(), atCl.c_str());
    g_worker.Cancel = false;
    g_worker.Progress = 0;
    g_worker.PendingPublish = false;
    g_worker.Cfg = std::move(cfg);
    g_worker.AtChangelist = std::move(atCl);
    const int cap = g_worker.Cfg.ChangelistCacheMaxEntries > 0 ? g_worker.Cfg.ChangelistCacheMaxEntries : 512;
    g_worker.Cache.reset(new P4ChangelistDescribeCache(cap));
    const size_t n = rows.size();
    g_worker.Total = n;
    {
        std::lock_guard<std::mutex> lk(g_worker.Mutex);
        g_worker.Rows = std::move(rows);
    }
    {
        std::lock_guard<std::mutex> lkW(g_worker.Mutex);
        std::lock_guard<std::mutex> lkD(g_displayMutex);
        g_displayRows = g_worker.Rows;
        g_detailFuts.assign(n, std::shared_future<DetailPack>());
        g_detailPhase.assign(n, 0);
        g_detailData.assign(n, DetailPack{});
        g_detailScrolled.assign(n, false);
    }
    g_worker.Running = true;
    g_worker.Thread = std::thread(WorkerThreadMain, n);
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
        const JiraConfig jiraCfg = ConfigManager::Load();
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
    std::lock_guard<std::mutex> lk(g_displayMutex);
    if (idx >= g_displayRows.size()) {
        return;
    }
    if (idx >= g_detailPhase.size() || g_detailPhase[idx] != 0) {
        return;
    }
    g_detailPhase[idx] = 1;
    const std::string path = g_displayRows[idx].PathForP4;
    LOG_DEBUG("Blame detail: async load start idx=%zu path=%s", idx, path.c_str());
    std::future<DetailPack> fut = std::async(std::launch::async, [cfg, path, atCl]() {
        DetailPack p;
        p.Lines = P4AnnotateFile(cfg, path, atCl, p.Error);
        for (auto& ln : p.Lines) {
            if (!ln.Changelist.empty()) {
                P4ChangelistDetails d = g_tooltipClCache.GetOrFetch(cfg, ln.Changelist);
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
    g_detailFuts[idx] = fut.share();
}

void PollDetails() {
    for (size_t i = 0; i < g_detachedDetailFuts.size();) {
        if (!g_detachedDetailFuts[i].valid() ||
            g_detachedDetailFuts[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            g_detachedDetailFuts.erase(g_detachedDetailFuts.begin() +
                                       static_cast<std::vector<std::shared_future<DetailPack>>::difference_type>(i));
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < g_detachedClHoverFuts.size();) {
        if (!g_detachedClHoverFuts[i].valid() ||
            g_detachedClHoverFuts[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            g_detachedClHoverFuts.erase(
                g_detachedClHoverFuts.begin() +
                static_cast<std::vector<std::shared_future<P4ChangelistDetails>>::difference_type>(i));
        } else {
            ++i;
        }
    }

    std::lock_guard<std::mutex> lk(g_displayMutex);
    for (size_t i = 0; i < g_detailFuts.size(); ++i) {
        if (i >= g_detailPhase.size() || g_detailPhase[i] != 1) {
            continue;
        }
        if (!g_detailFuts[i].valid()) {
            continue;
        }
        if (g_detailFuts[i].wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            continue;
        }
        std::string pathForLog;
        if (i < g_displayRows.size()) {
            pathForLog = g_displayRows[i].PathForP4;
        }
        try {
            g_detailData[i] = g_detailFuts[i].get();
        } catch (const std::exception& ex) {
            LOG_WARN("Blame detail: idx=%zu path=%s async exception=%s", i, pathForLog.c_str(), ex.what());
            g_detailData[i].Error = std::string("detail load failed: ") + ex.what();
        } catch (...) {
            LOG_WARN("Blame detail: idx=%zu path=%s async unknown exception", i, pathForLog.c_str());
            g_detailData[i].Error = "detail load failed";
        }
        if (!g_detailData[i].Error.empty()) {
            LOG_WARN("Blame detail: idx=%zu path=%s err=%s", i, pathForLog.c_str(), g_detailData[i].Error.c_str());
        }
        g_detailPhase[i] = 2;
    }
}

bool ResolveP4UserForAssign(AppController& app, const std::string& p4User, std::string& accountId, std::string& err) {
    accountId.clear();
    err.clear();
    if (p4User.empty() || p4User == "-") {
        err = "No Perforce user.";
        return false;
    }
    std::vector<JiraUser> users;
    if (!app.JiraSearchUsersByQuery(p4User, users, err)) {
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
    std::lock_guard<std::mutex> lk(g_displayMutex);
    std::ostringstream oss;
    for (size_t i = 0; i < g_displayRows.size(); ++i) {
        const BlameRow& r = g_displayRows[i];
        oss << "#" << (i + 1) << " " << r.Parsed.Function << "\n  " << r.PathForP4 << ":" << r.Parsed.LineNumber
            << "\n  User=" << r.Blame.User << " CL=" << r.Blame.Changelist << " Date=" << r.Blame.Date;
        if (r.Blame.Approximate) {
            oss << " [approximate]";
        }
        oss << "\n";
        if (!r.Blame.LineSnippet.empty()) {
            oss << "  " << r.Blame.LineSnippet << "\n";
        }
        if (i < g_detailData.size() && !g_detailData[i].Lines.empty()) {
            const int target = r.Parsed.LineNumber;
            for (const auto& ln : g_detailData[i].Lines) {
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
    bool needsQuotes = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuotes = true;
            break;
        }
    }
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
    std::lock_guard<std::mutex> lk(g_displayMutex);
    std::ostringstream oss;
    oss << "entry,function,path,line,user,changelist,date,approximate,line_snippet\n";
    for (size_t i = 0; i < g_displayRows.size(); ++i) {
        const BlameRow& r = g_displayRows[i];
        oss << (i + 1) << "," << CsvEscape(r.Parsed.Function) << "," << CsvEscape(r.PathForP4) << ","
            << r.Parsed.LineNumber << "," << CsvEscape(r.Blame.User) << "," << CsvEscape(r.Blame.Changelist) << ","
            << CsvEscape(r.Blame.Date) << "," << (r.Blame.Approximate ? "true" : "false") << ","
            << CsvEscape(r.Blame.LineSnippet) << "\n";
    }
    return oss.str();
}

std::string BuildBlameExportJson() {
    std::lock_guard<std::mutex> lk(g_displayMutex);
    nlohmann::json root = nlohmann::json::object();
    root["entries"] = nlohmann::json::array();
    for (size_t i = 0; i < g_displayRows.size(); ++i) {
        const BlameRow& r = g_displayRows[i];
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
        if (i < g_detailData.size() && !g_detailData[i].Lines.empty()) {
            const int target = r.Parsed.LineNumber;
            for (const auto& ln : g_detailData[i].Lines) {
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

std::string BuildBlameQuickCommentTemplate(const std::string& issueKey, const std::string& templateId,
                                           const BlameRow& row) {
    if (templateId == "need_repro") {
        return "Need repro details for " + issueKey + " (blame context: " + row.PathForP4 + ":" +
               std::to_string(row.Parsed.LineNumber) + ", CL " + row.Blame.Changelist + ").";
    }
    if (templateId == "need_logs") {
        return "Please attach logs/diagnostics for " + issueKey +
               " to continue triage.\nReference: " + row.Parsed.Function + " @ " + row.PathForP4 + ":" +
               std::to_string(row.Parsed.LineNumber) + ".";
    }
    return "Triage handoff for " + issueKey + ":\n- Suggested owner: " + row.Blame.User +
           "\n- Suspect location: " + row.Parsed.Function + " (" + row.PathForP4 + ":" +
           std::to_string(row.Parsed.LineNumber) + ")\n- CL: " + row.Blame.Changelist;
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
            if (pre < 1 || suf < 1 || pre + suf > n) {
                continue;
            }
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
    g_worker.Cancel = true;
    if (g_worker.Thread.joinable()) {
        g_worker.Thread.join();
    }
    {
        std::lock_guard<std::mutex> lk(g_worker.Mutex);
        g_worker.Rows.clear();
        g_worker.Progress = 0;
        g_worker.Total = 0;
    }
    {
        std::lock_guard<std::mutex> lk(g_displayMutex);
        for (auto& fut : g_detailFuts) {
            if (fut.valid() && fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                g_detachedDetailFuts.push_back(fut);
            }
        }
        g_displayRows.clear();
        g_detailFuts.clear();
        g_detailPhase.clear();
        g_detailData.clear();
        g_detailScrolled.clear();
    }
    g_clHoverCl.clear();
    if (g_clHoverFut.valid() && g_clHoverFut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        g_detachedClHoverFuts.push_back(g_clHoverFut);
    }
    g_clHoverFut = std::shared_future<P4ChangelistDetails>();
    std::memset(g_callstackBuf, 0, sizeof(g_callstackBuf));
    g_lastUiStatus.clear();
    g_pendingSelectEntryIndex = -1;
    s_lastCallstackIssueKey.clear();
    *pOpen = false;
}

void OpenJiraUserProfileForP4User(AppController& app, const std::string& p4User) {
    g_openProfileModal = true;
    g_profileErr.clear();
    g_profileName.clear();
    g_profileEmail.clear();
    g_profileGroups.clear();
    if (p4User.empty() || p4User == "-" || p4User == "...") {
        g_profileName = "Past Employee";
        return;
    }
    std::vector<JiraUser> users;
    std::string qerr;
    if (!app.JiraSearchUsersByQuery(p4User, users, qerr) || users.empty()) {
        g_profileName = "Past Employee";
        g_profileEmail = p4User;
        if (!qerr.empty()) {
            g_profileErr = qerr;
        }
        return;
    }
    const JiraUser* best = &users[0];
    for (const auto& u : users) {
        if (!u.EmailAddress.empty()) {
            best = &u;
            break;
        }
    }
    g_profileName = best->DisplayName;
    g_profileEmail = best->EmailAddress;
    std::string gerr;
    app.JiraFetchUserGroupNames(best->AccountId, g_profileGroups, gerr);
    if (g_profileGroups.empty() && !gerr.empty()) {
        g_profileErr = gerr;
    }
}

void PrepareAssignModal(AppController& app, const BlameRow& row, const std::string& p4UserCell) {
    g_assignRow = row;
    const std::string& pu = p4UserCell.empty() ? row.Blame.User : p4UserCell;
    g_assignAccountId.clear();
    g_assignHasJiraAccount = false;
    if (pu.empty() || pu == "-" || pu == "...") {
        g_assignTitle = "Past Employee";
        return;
    }
    std::vector<JiraUser> users;
    std::string err;
    if (!app.JiraSearchUsersByQuery(pu, users, err) || users.empty()) {
        g_assignTitle = std::string("Past Employee (") + pu + ")";
        return;
    }
    std::string aid;
    std::string e2;
    if (ResolveP4UserForAssign(app, pu, aid, e2) && !aid.empty()) {
        g_assignAccountId = std::move(aid);
        g_assignHasJiraAccount = true;
        std::string dn = users[0].DisplayName;
        for (const auto& u : users) {
            if (u.AccountId == g_assignAccountId) {
                dn = u.DisplayName;
                break;
            }
        }
        g_assignTitle = dn + " (" + pu + ")";
    } else {
        g_assignTitle = std::string("Past Employee (") + pu + ")";
    }
}

std::string BuildCallstackRowTsv(const BlameRow& row, size_t displayIndex) {
    std::ostringstream o;
    o << (displayIndex + 1) << '\t' << row.Parsed.Function << '\t' << row.PathForP4 << ':' << row.Parsed.LineNumber
      << '\t' << row.Blame.User << '\t' << row.Blame.Changelist << '\t' << row.Blame.Date;
    return o.str();
}

void DrawClTooltipAsync(const std::string& cl, const BlameAnalysisConfig& cfg, const BlameUiThemeColors& theme) {
    if (cl.empty()) {
        return;
    }
    if (g_clHoverCl != cl) {
        g_clHoverCl = cl;
        BlameAnalysisConfig cfgCopy = cfg;
        g_clHoverFut = std::async(std::launch::async, [cfgCopy, cl]() {
                           return g_tooltipClCache.GetOrFetch(cfgCopy, cl);
                       }).share();
    }
    ImGui::BeginTooltip();
    ImGui::TextDisabled("Click to open this changelist in p4vc.");
    ImGui::Separator();
    const float wrapX = ImGui::GetCursorPosX() + 600.f;
    ImGui::PushTextWrapPos(wrapX);
    if (!g_clHoverFut.valid()) {
        ImGui::TextUnformatted("Loading CL info…");
    } else if (g_clHoverFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        ImGui::TextUnformatted("Loading CL info…");
    } else {
        try {
            const P4ChangelistDetails d = g_clHoverFut.get();
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
            ImGui::TextUnformatted("Loading CL info…");
        } catch (...) {
            LOG_WARN("Blame tooltip: changelist detail future unknown exception");
            ImGui::TextUnformatted("Loading CL info…");
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DrawBlamePersistedOptionsForm(AppController& app, const BlameUiThemeColors& theme) {
    ImGui::InputInt("Max frames", &g_maxFramesVal);
    if (g_maxFramesVal < 1) {
        g_maxFramesVal = 1;
    }
    if (g_maxFramesVal > 500) {
        g_maxFramesVal = 500;
    }
    ImGui::InputTextMultiline("Ignore keywords (comma or newline)", g_ignoreBuf.data(), g_ignoreBuf.size(),
                              ImVec2(-1, 60));
    ImGui::InputText("P4 executable", g_p4Exe, sizeof(g_p4Exe));
    ImGui::InputText("p4vc executable", g_p4vcExe, sizeof(g_p4vcExe));
    ImGui::InputText("Timelapse cmd (optional)", g_timeTpl, sizeof(g_timeTpl));
    ImGui::TextDisabled("Placeholders: {file} {line} {cl}");
    ImGui::InputText("Changelist cmd (optional)", g_changeTpl, sizeof(g_changeTpl));
    ImGui::InputText("AI chat URL (optional)", g_aiUrl, sizeof(g_aiUrl));
    ImGui::Separator();
    ImGui::TextUnformatted("Callstack from Jira");
    ImGui::TextDisabled("When set, the callstack text is filled from this field on the selected issue when you open "
                        "Blame Analysis or change the selection.");
    {
        std::string comboPreview = "(none)";
        if (!g_blameCfg.CallstackJiraFieldId.empty()) {
            const TrackerField* mf = app.FindFieldById(g_blameCfg.CallstackJiraFieldId);
            if (mf && !mf->Name.empty()) {
                comboPreview = mf->Name + " (" + mf->Id + ")";
            } else {
                comboPreview = g_blameCfg.CallstackJiraFieldId;
            }
        }
        PushBlameLinkButtonColors(theme);
        const bool jiraFieldComboOpen = ImGui::BeginCombo("Jira field##callstacksrc", comboPreview.c_str());
        PopBlameLinkButtonColors();
        if (!jiraFieldComboOpen) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Choose which Jira field fills the callstack when you open Blame Analysis.");
            }
        } else {
            PushBlameLinkTextOnly(theme);
            if (ImGui::Selectable("(none)", g_blameCfg.CallstackJiraFieldId.empty())) {
                g_blameCfg.CallstackJiraFieldId.clear();
                SyncCallstackJiraFieldBufFromCfg();
            }
            PopBlameLinkTextOnly();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Do not pull callstack text from Jira.");
            }
            for (const auto& f : app.GetAvailableFields()) {
                const bool sel = (f.Id == g_blameCfg.CallstackJiraFieldId);
                const std::string lbl = f.Name.empty() ? f.Id : (f.Name + std::string(" (") + f.Id + ")");
                PushBlameLinkTextOnly(theme);
                if (ImGui::Selectable(lbl.c_str(), sel)) {
                    g_blameCfg.CallstackJiraFieldId = f.Id;
                    SyncCallstackJiraFieldBufFromCfg();
                }
                PopBlameLinkTextOnly();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("Use this Jira field as the callstack source for the selected issue.");
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputText("Callstack field id (optional)", g_callstackJiraFieldBuf, sizeof(g_callstackJiraFieldBuf));
    ImGui::TextDisabled("Override or set manually (e.g. customfield_10000). Saved with Save settings.");
    ImGui::InputText("Path remap from", g_remapFrom, sizeof(g_remapFrom));
    ImGui::InputText("Path remap to", g_remapTo, sizeof(g_remapTo));
    PushBlameLinkButtonColors(theme);
    if (ImGui::Button("Save settings")) {
        g_blameCfg.P4Executable = g_p4Exe;
        g_blameCfg.P4VcExecutable = g_p4vcExe;
        g_blameCfg.TimelapseCommandTemplate = g_timeTpl;
        g_blameCfg.ChangeCommandTemplate = g_changeTpl;
        g_blameCfg.AiChatUrl = g_aiUrl;
        g_blameCfg.DefaultMaxFrames = g_maxFramesVal;
        g_blameCfg.CallstackJiraFieldId.assign(g_callstackJiraFieldBuf);
        g_blameCfg.PathRemaps.clear();
        if (g_remapFrom[0] != '\0') {
            g_blameCfg.PathRemaps.push_back({g_remapFrom, g_remapTo});
        }
        g_blameCfg.DefaultIgnoreKeywords = SplitIgnoreKeywords(std::string(g_ignoreBuf.data()));
        ConfigManager::SaveBlameAnalysis(g_blameCfg);
        SyncCallstackJiraFieldBufFromCfg();
        LogBlameP4PathsIfChanged("save_settings");
        g_lastUiStatus = "Blame settings saved.";
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Write blame options (P4 paths, Jira field, remaps, etc.) to smatchet_config.json.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload settings")) {
        g_blameCfg = ConfigManager::LoadBlameAnalysis();
        CopyToBuffer(g_p4Exe, g_blameCfg.P4Executable);
        CopyToBuffer(g_p4vcExe, g_blameCfg.P4VcExecutable);
        CopyToBuffer(g_timeTpl, g_blameCfg.TimelapseCommandTemplate);
        CopyToBuffer(g_changeTpl, g_blameCfg.ChangeCommandTemplate);
        CopyToBuffer(g_aiUrl, g_blameCfg.AiChatUrl);
        g_maxFramesVal = g_blameCfg.DefaultMaxFrames;
        size_t off = 0;
        for (const auto& kw : g_blameCfg.DefaultIgnoreKeywords) {
            if (off + kw.size() + 2 >= g_ignoreBuf.size()) {
                break;
            }
            if (off > 0) {
                g_ignoreBuf[off++] = '\n';
            }
            for (char c : kw) {
                if (off + 1 >= g_ignoreBuf.size()) {
                    break;
                }
                g_ignoreBuf[off++] = static_cast<char>(c);
            }
        }
        if (off < g_ignoreBuf.size()) {
            g_ignoreBuf[off] = '\0';
        }
        if (g_blameCfg.PathRemaps.empty()) {
            g_remapFrom[0] = '\0';
            g_remapTo[0] = '\0';
        } else {
            CopyToBuffer(g_remapFrom, g_blameCfg.PathRemaps[0].FromPrefix);
            CopyToBuffer(g_remapTo, g_blameCfg.PathRemaps[0].ToPrefix);
        }
        SyncCallstackJiraFieldBufFromCfg();
        LogBlameP4PathsIfChanged("reload_settings");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Discard unsaved edits and reload from smatchet_config.json.");
    }
    PopBlameLinkButtonColors();
}

} // namespace

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
    g_blameCfg = ConfigManager::LoadBlameAnalysis();
    g_maxFramesVal = g_blameCfg.DefaultMaxFrames;
    CopyToBuffer(g_p4Exe, g_blameCfg.P4Executable);
    CopyToBuffer(g_p4vcExe, g_blameCfg.P4VcExecutable);
    CopyToBuffer(g_timeTpl, g_blameCfg.TimelapseCommandTemplate);
    CopyToBuffer(g_changeTpl, g_blameCfg.ChangeCommandTemplate);
    CopyToBuffer(g_aiUrl, g_blameCfg.AiChatUrl);
    size_t off = 0;
    for (const auto& kw : g_blameCfg.DefaultIgnoreKeywords) {
        if (off + kw.size() + 2 >= g_ignoreBuf.size()) {
            break;
        }
        if (off > 0) {
            g_ignoreBuf[off++] = '\n';
        }
        for (char c : kw) {
            if (off + 1 >= g_ignoreBuf.size()) {
                break;
            }
            g_ignoreBuf[off++] = static_cast<char>(c);
        }
    }
    if (off < g_ignoreBuf.size()) {
        g_ignoreBuf[off] = '\0';
    }
    if (g_blameCfg.PathRemaps.empty()) {
        g_remapFrom[0] = '\0';
        g_remapTo[0] = '\0';
    } else {
        CopyToBuffer(g_remapFrom, g_blameCfg.PathRemaps[0].FromPrefix);
        CopyToBuffer(g_remapTo, g_blameCfg.PathRemaps[0].ToPrefix);
    }
    SyncCallstackJiraFieldBufFromCfg();
    LogBlameP4PathsIfChanged("initial_load");
    cfgLoaded_ = true;
}

void BlameAnalysisUi::DrawBlamePreferencesTab(AppController& app) {
    ensureSettingsBuffersLoaded();
    MaybeAutoselectCallstackJiraField(app);
    ImGui::TextWrapped("Perforce paths, ignore list, and Jira callstack source used by Blame Analysis (stored in "
                       "smatchet_config.json).");
    ImGui::Spacing();
    const BlameUiThemeColors& theme = g_blameCfg.UiColors;
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

    MaybeAutoselectCallstackJiraField(app);

    const bool justOpened = !blameOpenPrev_;
    blameOpenPrev_ = true;
    if (justOpened || selectedJiraIssueKey != s_lastCallstackIssueKey) {
        s_lastCallstackIssueKey = selectedJiraIssueKey;
        TryFillCallstackFromJira(app, selectedJiraIssueKey);
    }

    const BlameUiThemeColors& theme = g_blameCfg.UiColors;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    constexpr ImGuiWindowFlags kModalFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("###BlameAnalysisModal", pOpen, kModalFlags)) {
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
            if (g_aiUrl[0] != '\0') {
                app.OpenUrl(std::string(g_aiUrl));
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
            g_lastUiStatus = "Blame JSON export copied to clipboard.";
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Copy structured blame export JSON (entries + nearby lines) to clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Export CSV")) {
            ImGui::SetClipboardText(BuildBlameExportCsv().c_str());
            g_lastUiStatus = "Blame CSV export copied to clipboard.";
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
        const JiraConnectivityBannerForUi jiraBanner = app.GetJiraConnectivityBannerForUi(nullptr);
        if (jiraBanner.Kind == JiraConnectivityBannerForUi::Level::Error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        } else if (jiraBanner.Kind == JiraConnectivityBannerForUi::Level::Warning) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
    }

    if (!g_lastUiStatus.empty()) {
        ImGui::TextWrapped("%s", g_lastUiStatus.c_str());
        ImGui::Separator();
    }

    std::vector<BlameRow> rowsSnap;
    size_t nrow = 0;
    {
        std::lock_guard<std::mutex> lk(g_displayMutex);
        rowsSnap = g_displayRows;
        nrow = rowsSnap.size();
    }

    const bool busy = g_worker.Running.load();
    const int prog = g_worker.Progress.load();

    if (ImGui::BeginTabBar("blame_main_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Callstack")) {
            ImGui::Text("Callstack Frames: %zu", nrow);
            ImGui::SameLine();
            PushBlameLinkButtonColors(theme);
            if (g_showRaw) {
                if (ImGui::Button("Show Table")) {
                    g_showRaw = false;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("Switch back to the callstack table view.");
                }
            } else {
                if (ImGui::Button("Show Raw Text")) {
                    g_showRaw = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("Show the raw pasted callstack text in a scrollable field.");
                }
            }
            PopBlameLinkButtonColors();
            if (!g_showRaw && nrow > 0) {
                ImGui::TextDisabled("Click row to copy, double-click in the # to jump to entry");
            }

            ImGui::Spacing();
            ImGui::TextDisabled(
                "Max frames, ignore list, P4 tools, and Jira callstack source: Settings → Preferences → Blame "
                "Analysis.");
            ImGui::InputText("At changelist (optional)", g_atClBuf, sizeof(g_atClBuf));

            PushBlameLinkButtonColors(theme);
            if (ImGui::Button("Process") && !busy) {
                g_lastUiStatus.clear();
                g_blameCfg.P4Executable = g_p4Exe;
                g_blameCfg.P4VcExecutable = g_p4vcExe;
                g_blameCfg.TimelapseCommandTemplate = g_timeTpl;
                g_blameCfg.ChangeCommandTemplate = g_changeTpl;
                g_blameCfg.AiChatUrl = g_aiUrl;
                g_blameCfg.CallstackJiraFieldId.assign(g_callstackJiraFieldBuf);
                g_blameCfg.PathRemaps.clear();
                if (g_remapFrom[0] != '\0') {
                    g_blameCfg.PathRemaps.push_back({g_remapFrom, g_remapTo});
                }
                LogBlameP4PathsIfChanged("process");
                std::vector<std::string> keywords = SplitIgnoreKeywords(std::string(g_ignoreBuf.data()));
                if (keywords.empty()) {
                    keywords = g_blameCfg.DefaultIgnoreKeywords;
                }
                std::vector<ParsedCallstackFrame> parsed = ParseCallstackText(std::string(g_callstackBuf));
                std::vector<BlameRow> rows;
                for (auto& p : parsed) {
                    if (FrameMatchesIgnoreKeywords(p, keywords)) {
                        continue;
                    }
                    BlameRow br;
                    br.Parsed = std::move(p);
                    br.PathForP4 = ApplyPathRemaps(br.Parsed.FilePath, g_blameCfg.PathRemaps);
                    rows.push_back(std::move(br));
                    if (static_cast<int>(rows.size()) >= g_maxFramesVal) {
                        break;
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(g_displayMutex);
                    g_displayRows.clear();
                    g_detailFuts.clear();
                    g_detailPhase.clear();
                    g_detailData.clear();
                    g_detailScrolled.clear();
                }
                if (rows.empty()) {
                    g_lastUiStatus = "No stack frames parsed (check format and ignore list).";
                } else {
                    StartWorker(std::move(rows), g_blameCfg, std::string(g_atClBuf));
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !busy) {
                ImGui::SetTooltip(
                    "Parse the callstack and fetch Perforce blame for each frame (uses fields from Preferences → "
                    "Blame Analysis and the callstack text above).");
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") && busy) {
                g_worker.Cancel = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && busy) {
                ImGui::SetTooltip("Stop the in-progress blame worker.");
            }
            PopBlameLinkButtonColors();

            float rawFieldW = -1.f;
            float rawMaxLineW = 0.f;
            if (g_showRaw) {
                for (const char* p = g_callstackBuf; *p != '\0';) {
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

            if (g_showRaw) {
                ImGui::BeginChild("##rawcs_scroll", ImVec2(rawFieldW, 220.f), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                const ImVec2 inner = ImGui::GetContentRegionAvail();
                const float inputW = std::max(inner.x, rawMaxLineW + ImGui::GetStyle().FramePadding.x * 2.f + 8.f);
                ImGui::InputTextMultiline("##callstackpaste", g_callstackBuf, sizeof(g_callstackBuf),
                                          ImVec2(inputW, inner.y), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndChild();
            } else {
                ImGui::InputTextMultiline("##callstackpaste", g_callstackBuf, sizeof(g_callstackBuf),
                                          ImVec2(-1.f, 120.f), 0);
            }

            if (!g_showRaw && nrow > 0) {
                const float locColW = 250.f;
                if (ImGui::BeginTable("blame_tbl", 6,
                                      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                          ImGuiTableFlags_ScrollY,
                                      ImVec2(0, ImGui::GetContentRegionAvail().y - 8.f))) {
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
                        ImGui::Selectable(idxBuf, false,
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                              ImGuiSelectableFlags_AllowDoubleClick,
                                          ImVec2(0.f, rowH));
                        PopBlameLinkTextOnly();
                        if (ImGui::IsItemClicked()) {
                            ImGui::SetClipboardText(BuildCallstackRowTsv(row, i).c_str());
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("Click: copy this row as tab-separated values.\n"
                                              "Double-click: open the Entry tab for this frame.");
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                            g_pendingSelectEntryIndex = static_cast<int>(i);
                            EnsureDetailLoading(i, g_blameCfg, std::string(g_atClBuf));
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemAllowOverlap();
                        {
                            const char* fn = row.Parsed.Function.c_str();
                            const float colAvail = ImGui::GetContentRegionAvail().x;
                            const float lineH = ImGui::GetTextLineHeight();
                            PushBlameLinkTextOnly(theme);
                            if (ImGui::Selectable(fn, false, ImGuiSelectableFlags_AllowOverlap,
                                                  ImVec2(colAvail, lineH))) {
                                /* click handled below */
                            }
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemClicked()) {
                                ImGui::SetClipboardText(fn);
                            }
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("Click to copy the function name to the clipboard.\n\n%s", fn);
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
                            if (ImGui::Selectable(shortLoc.c_str(), false, ImGuiSelectableFlags_AllowOverlap)) {
                                LaunchP4VcLike(g_blameCfg, g_timeTpl, g_changeTpl, true, row.PathForP4,
                                               row.Parsed.LineNumber, row.Blame.Changelist);
                            }
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("Click to open p4vc timelapse for this file and line.\n\n%s",
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
                            ImGui::Selectable(userDisp.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
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
                                                      "Right-click: assign flow / context menu.\n%s",
                                                      row.Blame.Approximate
                                                          ? "\nApproximate blame (line may not match exact CL)."
                                                          : "");
                                } else {
                                    ImGui::SetTooltip("No Perforce user on this row.");
                                }
                            }
                            if (!pending && ImGui::IsItemClicked() && !row.Blame.User.empty() &&
                                row.Blame.User != "...") {
                                OpenJiraUserProfileForP4User(app, row.Blame.User);
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
                            ImGui::Selectable(clDisp.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
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
                                    DrawClTooltipAsync(row.Blame.Changelist, g_blameCfg, theme);
                                }
                            }
                            if (ImGui::IsItemClicked() && !pending && !row.Blame.Changelist.empty()) {
                                LaunchP4VcLike(g_blameCfg, g_timeTpl, g_changeTpl, false, row.PathForP4,
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
            }

            ImGui::EndTabItem();
        }

        for (size_t ti = 0; ti < nrow; ++ti) {
            char tabName[32];
            std::snprintf(tabName, sizeof(tabName), "Entry %zu", ti + 1);
            ImGuiTabItemFlags tflags = ImGuiTabItemFlags_None;
            if (g_pendingSelectEntryIndex == static_cast<int>(ti)) {
                tflags = ImGuiTabItemFlags_SetSelected;
            }
            if (ImGui::BeginTabItem(tabName, nullptr, tflags)) {
                if (g_pendingSelectEntryIndex == static_cast<int>(ti)) {
                    g_pendingSelectEntryIndex = -1;
                }
                EnsureDetailLoading(ti, g_blameCfg, std::string(g_atClBuf));
                BlameRow row = rowsSnap[ti];
                ImGui::Text("File: %s", row.PathForP4.c_str());
                ImGui::Text("Target Line: %d", row.Parsed.LineNumber);

                int phase = 0;
                if (ti < g_detailPhase.size()) {
                    phase = g_detailPhase[ti];
                }
                if (phase == 1) {
                    ImGui::TextUnformatted("Loading annotated file…");
                } else if (ti < g_detailData.size() && !g_detailData[ti].Error.empty()) {
                    ImGui::TextColored(ThCol(theme.StatusError), "%s", g_detailData[ti].Error.c_str());
                } else if (ti < g_detailData.size() &&
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
                    const std::vector<P4AnnotatedLine>& lines = g_detailData[ti].Lines;
                    const ImU32 hlU32 = ImGui::ColorConvertFloat4ToU32(ThCol(theme.FindHighlight));
                    for (const auto& ln : lines) {
                        ImGui::PushID(ln.SourceLine);
                        ImGui::TableNextRow();
                        const bool hl = (ln.SourceLine == targetLine);
                        if (hl) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, hlU32);
                        }

                        ImGui::TableSetColumnIndex(0);
                        if (hl) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.StatusWarning));
                            ImGui::TextUnformatted(">>>");
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextUnformatted(" ");
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d", ln.SourceLine);

                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemAllowOverlap();
                        if (ln.Changelist.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::Selectable("-", false, ImGuiSelectableFlags_AllowOverlap);
                            ImGui::PopStyleColor();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                ImGui::SetTooltip("No changelist on this line.");
                            }
                        } else {
                            PushBlameLinkTextOnly(theme);
                            if (ImGui::Selectable(ln.Changelist.c_str(), false, ImGuiSelectableFlags_AllowOverlap)) {
                                LaunchP4VcLike(g_blameCfg, g_timeTpl, g_changeTpl, false, row.PathForP4, ln.SourceLine,
                                               ln.Changelist);
                            }
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                DrawClTooltipAsync(ln.Changelist, g_blameCfg, theme);
                            }
                        }

                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemAllowOverlap();
                        if (ln.User.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::Selectable("-", false, ImGuiSelectableFlags_AllowOverlap);
                            ImGui::PopStyleColor();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                std::string tip = "No Perforce user on this line.";
                                if (hl && row.Blame.Approximate) {
                                    tip += "\n\nApproximate row: unable to find exact CL and user.";
                                }
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                        } else {
                            PushBlameLinkTextOnly(theme);
                            ImGui::Selectable(ln.User.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
                            PopBlameLinkTextOnly();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                                std::string tip = "Left-click: look up this Perforce user in Jira.\n"
                                                  "Right-click: assign flow / context menu.";
                                if (hl && row.Blame.Approximate) {
                                    tip += "\n\nApproximate row: unable to find exact CL and user.";
                                }
                                ImGui::SetTooltip("%s", tip.c_str());
                            }
                            if (ImGui::IsItemClicked()) {
                                OpenJiraUserProfileForP4User(app, ln.User);
                            }
                        }
                        if (ImGui::IsMouseClicked(1) && ImGui::IsItemHovered()) {
                            BlameRow br = row;
                            br.Blame.User = ln.User;
                            br.Blame.Changelist = ln.Changelist;
                            br.Parsed.LineNumber = ln.SourceLine;
                            br.Blame.LineSnippet = ln.Code;
                            br.Blame.Date = ln.Date;
                            br.Blame.Approximate = false;
                            PrepareAssignModal(app, br, ln.User);
                            ImGui::OpenPopup("blame_assign");
                        }

                        ImGui::TableSetColumnIndex(4);
                        if (ln.Date.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.TextDisabled));
                            ImGui::TextUnformatted("-");
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextUnformatted(NormalizeDateDisplay(ln.Date).c_str());
                        }

                        ImGui::TableSetColumnIndex(5);
                        BlameDrawColoredCppLine(ln.Code.c_str(), theme);

                        if (hl && ti < g_detailScrolled.size() && !g_detailScrolled[ti]) {
                            ImGui::SetScrollHereY(0.5f);
                            g_detailScrolled[ti] = true;
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
        const JiraConnectivityBannerForUi jiraBanner = app.GetJiraConnectivityBannerForUi(nullptr);
        const bool readOnlyMode = (jiraBanner.Kind == JiraConnectivityBannerForUi::Level::Error);
        ImGui::TextUnformatted(g_assignTitle.c_str());
        ImGui::Separator();
        if (jiraBanner.Kind == JiraConnectivityBannerForUi::Level::Error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Assign and comment actions stay disabled until Jira is reachable.");
            ImGui::Separator();
        } else if (jiraBanner.Kind == JiraConnectivityBannerForUi::Level::Warning) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
        if (selectedJiraIssueKey.empty()) {
            ImGui::TextDisabled("Select a Jira issue in the grid.");
        } else {
            const bool hasJiraAccount = g_assignHasJiraAccount && !g_assignAccountId.empty();
            PushBlameLinkTextOnly(theme);
            ImGui::BeginDisabled(readOnlyMode);
            if (ImGui::Selectable("Assign issue to user", false)) {
                std::string err;
                const TrackerField* f = app.FindFieldById("assignee");
                if (!hasJiraAccount) {
                    LOG_ERROR("Blame UI: assign skipped — no Jira account match for this Perforce user.");
                    g_lastUiStatus = "No Jira user match for assign.";
                } else if (!f) {
                    LOG_ERROR("Blame UI: assignee field not in catalog.");
                    g_lastUiStatus = "assignee field not in catalog.";
                } else if (app.SubmitFieldEdit(selectedJiraIssueKey, *f, {g_assignAccountId}, err)) {
                    LOG_INFO("Blame UI: assignee set on %s", selectedJiraIssueKey.c_str());
                    g_lastUiStatus = "Assignee updated.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: assign failed: %s", err.c_str());
                    g_lastUiStatus = err;
                }
            }
            ImGui::EndDisabled();
            PopBlameLinkTextOnly();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("Set the issue assignee to the Jira user matched from this blame line.\n"
                                  "Requires a matching Jira account; otherwise an error is shown.");
            }
            ImGui::BeginDisabled(readOnlyMode);
            if (ImGui::Selectable("Add blame context comment", false)) {
                std::string err;
                if (app.JiraAddIssueCommentBlameContext(
                        selectedJiraIssueKey, g_assignRow.Blame.User, g_assignRow.Parsed.Function,
                        g_assignRow.PathForP4, g_assignRow.Parsed.LineNumber, g_assignRow.Blame.Changelist,
                        g_assignRow.Blame.Date, g_assignRow.Blame.Approximate, g_assignRow.Blame.LineSnippet, err)) {
                    LOG_INFO("Blame UI: posted blame context comment for %s.", selectedJiraIssueKey.c_str());
                    g_lastUiStatus = "Blame context comment posted.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: comment failed: %s", err.c_str());
                    g_lastUiStatus = err;
                }
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextDisabled("Quick comment templates");
            ImGui::BeginDisabled(readOnlyMode);
            if (ImGui::Selectable("Need repro details", false)) {
                std::string err;
                if (app.JiraAddIssueCommentPlain(
                        selectedJiraIssueKey,
                        BuildBlameQuickCommentTemplate(selectedJiraIssueKey, "need_repro", g_assignRow), err)) {
                    g_lastUiStatus = "Posted 'Need repro details' comment.";
                    ImGui::CloseCurrentPopup();
                } else {
                    g_lastUiStatus = err.empty() ? "Failed to post Jira comment." : err;
                }
            }
            if (ImGui::Selectable("Need logs / diagnostics", false)) {
                std::string err;
                if (app.JiraAddIssueCommentPlain(
                        selectedJiraIssueKey,
                        BuildBlameQuickCommentTemplate(selectedJiraIssueKey, "need_logs", g_assignRow), err)) {
                    g_lastUiStatus = "Posted 'Need logs / diagnostics' comment.";
                    ImGui::CloseCurrentPopup();
                } else {
                    g_lastUiStatus = err.empty() ? "Failed to post Jira comment." : err;
                }
            }
            if (ImGui::Selectable("Triage handoff summary", false)) {
                std::string err;
                if (app.JiraAddIssueCommentPlain(
                        selectedJiraIssueKey,
                        BuildBlameQuickCommentTemplate(selectedJiraIssueKey, "handoff", g_assignRow), err)) {
                    g_lastUiStatus = "Posted triage handoff comment.";
                    ImGui::CloseCurrentPopup();
                } else {
                    g_lastUiStatus = err.empty() ? "Failed to post Jira comment." : err;
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
                    assigned = app.SubmitFieldEdit(selectedJiraIssueKey, *f, {g_assignAccountId}, err);
                }
                if (assigned &&
                    app.JiraAddIssueCommentBlameContext(
                        selectedJiraIssueKey, g_assignRow.Blame.User, g_assignRow.Parsed.Function,
                        g_assignRow.PathForP4, g_assignRow.Parsed.LineNumber, g_assignRow.Blame.Changelist,
                        g_assignRow.Blame.Date, g_assignRow.Blame.Approximate, g_assignRow.Blame.LineSnippet, err)) {
                    LOG_INFO("Blame UI: assigned %s and posted blame context comment.", selectedJiraIssueKey.c_str());
                    g_lastUiStatus = "Assigned and commented.";
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_ERROR("Blame UI: assign/comment failed: %s", err.c_str());
                    g_lastUiStatus = err;
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

    if (g_openProfileModal) {
        ImGui::OpenPopup("Jira user profile");
        g_openProfileModal = false;
    }
    if (ImGui::BeginPopupModal("Jira user profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!g_profileErr.empty() && g_profileName.empty()) {
            ImGui::TextUnformatted(g_profileErr.c_str());
        } else {
            ImGui::Text("Name: %s", g_profileName.c_str());
            ImGui::Text("Email: %s", g_profileEmail.c_str());
            if (!g_profileErr.empty()) {
                ImGui::TextDisabled("%s", g_profileErr.c_str());
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Groups (best effort):");
            if (g_profileGroups.empty()) {
                ImGui::TextDisabled("(none or not permitted)");
            } else {
                for (const auto& gname : g_profileGroups) {
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
