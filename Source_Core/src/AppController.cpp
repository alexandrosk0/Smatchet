#include "AppController.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ConfigManager.h"
#include "FieldCatalogCache.h"

#include <ghc/filesystem.hpp>
#include "JiraClient.h"
#include "PlaneClient.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "StringUtil.h"
#include "Views.h"

#include "SmatchetUI.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

namespace {
#if defined(__APPLE__) || defined(__linux__)
bool LaunchCommandNoShell(const char* exe, const std::string& arg) {
    if (!exe || arg.empty()) {
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        execlp(exe, exe, arg.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}
#endif
} // namespace

namespace {
void LogProcessCwdForScriptsDiagnostics() {
#if defined(_WIN32)
    char cwdBuf[MAX_PATH];
    const DWORD n = GetCurrentDirectoryA(static_cast<DWORD>(sizeof(cwdBuf)), cwdBuf);
    if (n > 0 && n < sizeof(cwdBuf)) {
        LOG_INFO("AppController: process cwd (Win32)=\"%s\"", cwdBuf);
    } else {
        LOG_WARN("AppController: GetCurrentDirectoryA failed err=%lu", static_cast<unsigned long>(GetLastError()));
    }
#elif defined(__APPLE__) || defined(__linux__)
    char cwdBuf[4096];
    if (getcwd(cwdBuf, sizeof(cwdBuf))) {
        LOG_INFO("AppController: process cwd=\"%s\"", cwdBuf);
    } else {
        LOG_WARN("AppController: getcwd failed errno=%d", errno);
    }
#endif
}

void LogLuaScriptFileProbe(const char* label, const std::string& path) {
    if (path.empty()) {
        LOG_WARN("AppController: Lua script probe %s: path empty (blocked or unresolved)", label);
        return;
    }
    namespace fs = ghc::filesystem;
    std::error_code ec;
    const bool reg = fs::is_regular_file(fs::path(path), ec);
    LOG_INFO("AppController: Lua script probe %s: path=\"%s\" regular_file=%s ec=%s", label, path.c_str(),
             reg ? "yes" : "no", ec ? ec.message().c_str() : "none");
}
} // namespace

namespace {
std::mutex g_TrackerIssueFetchMutex;

bool FieldIconHasCaseInsensitivePrefix(const std::string& value, const std::string& prefix) {
    if (prefix.empty() || value.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(value[i]);
        unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<unsigned char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    if (value.size() == prefix.size()) {
        return true;
    }
    const char next = value[prefix.size()];
    return next == '/' || next == '\\';
}
} // namespace

AppController::~AppController() {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    ClearLuaTicketContextGlue();
    {
        std::lock_guard<std::mutex> lock(automationJobMutex_);
        automationWorkerShuttingDown_.store(true);
    }
    automationJobCv_.notify_all();
    if (automationWorker_.joinable()) {
        automationWorker_.join();
    }
#endif
    // Join UiDrawSession std::async futures that captured this controller (via static `g_ui`) before
    // tearing down members other threads may still touch.
    DrainUiDrawSessionFuturesBeforeAppTeardown(*this);
    shuttingDown_.store(true);
    DrainTrackerConnectivityProbeFuture();
    JoinBackgroundTasks();
}

std::shared_ptr<const std::vector<CachedTicket>> AppController::GetActiveTicketsSnapshot() const {
    std::lock_guard<std::mutex> lock(activeTicketsMutex_);
    if (!activeTicketsPublished_) {
        activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ActiveTickets);
    }
    return activeTicketsPublished_;
}

std::vector<CachedTicket> AppController::GetActiveTickets() const {
    std::lock_guard<std::mutex> lock(activeTicketsMutex_);
    if (!activeTicketsPublished_) {
        activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ActiveTickets);
    }
    return *activeTicketsPublished_;
}

void AppController::PrefetchIssueTicketsForKeys(const std::vector<std::string>& issueKeys, bool includeAlreadyActive) {
    if (!Cache) {
        return;
    }
    std::vector<std::string> toFetch;
    {
        const auto snap = GetActiveTicketsSnapshot();
        std::unordered_set<std::string> have;
        if (snap) {
            for (const auto& t : *snap) {
                have.insert(t.id);
            }
        }
        std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
        for (const auto& k : issueKeys) {
            if (k.empty() || (!includeAlreadyActive && have.count(k) > 0)) {
                continue;
            }
            if (bulkImportPrefetchKeysInFlight_.count(k) > 0) {
                continue;
            }
            bulkImportPrefetchKeysInFlight_.insert(k);
            toFetch.push_back(k);
        }
    }
    if (toFetch.empty()) {
        return;
    }

    LaunchBackgroundTask([this, toFetch]() {
        ITrackerClient* backend = Backend.get();
        if (!backend) {
            return;
        }
        TrackerConfig cfg = ConfigManager::Load();
        ViewsStore views = ConfigManager::LoadViewsOrBootstrap(cfg);
        std::string err;
        std::vector<CachedTicket> tickets;
        const bool ok = backend->FetchIssuesForKeys(cfg, toFetch, views, tickets, err);
        {
            std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
            for (const auto& k : toFetch) {
                bulkImportPrefetchKeysInFlight_.erase(k);
            }
        }
        if (!ok) {
            if (IsTrackerTransportErrorText(err)) {
                LOG_INFO("AppController::PrefetchIssueTicketsForKeys skipped (transport): %s", err.c_str());
            } else {
                LOG_WARN("AppController::PrefetchIssueTicketsForKeys failed: %s", err.c_str());
            }
            return;
        }
        requestDeferredLiveTrackerBackendSuccessNotify_();
        if (!Cache) {
            return;
        }
        for (auto& t : tickets) {
            Cache->SaveTicket(t);
        }
        RefreshLocalData();
    });
}

bool AppController::IsBulkImportPrefetchInFlight(const std::string& issueKey) const {
    if (issueKey.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
    return bulkImportPrefetchKeysInFlight_.find(issueKey) != bulkImportPrefetchKeysInFlight_.end();
}

void AppController::LaunchBackgroundTask(std::function<void()> task) {
    if (!task || shuttingDown_.load()) {
        return;
    }
    std::thread worker([this, task = std::move(task)]() mutable {
        if (shuttingDown_.load()) {
            return;
        }
        task();
    });
    std::lock_guard<std::mutex> lock(backgroundWorkersMutex_);
    backgroundWorkers_.push_back(std::move(worker));
}

void AppController::JoinBackgroundTasks() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(backgroundWorkersMutex_);
        workers = std::move(backgroundWorkers_);
    }
    const std::thread::id selfId = std::this_thread::get_id();
    for (auto& worker : workers) {
        if (!worker.joinable()) {
            continue;
        }
        if (worker.get_id() == selfId) {
            worker.detach();
            continue;
        }
        worker.join();
    }
}

void AppController::SetOpenUrlHandler(std::function<void(const std::string&)> handler) {
    OpenUrlHandler = std::move(handler);
}

void AppController::SetCloseEmbeddedUiHandler(std::function<void()> handler) {
    CloseEmbeddedUiHandler = std::move(handler);
}

void AppController::CloseEmbeddedUi() {
    if (CloseEmbeddedUiHandler) {
        CloseEmbeddedUiHandler();
    }
}

void AppController::SetRuntimePluginHost(PluginHost* host) {
    runtimePluginHost_ = host;
}

#if defined(SMATCHET_WITH_MCP)
namespace {

std::string PrefixMcpActivityLine(const std::string& msg) {
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#if defined(_WIN32)
    if (localtime_s(&tmBuf, &t) != 0) {
        return msg;
    }
#else
    if (localtime_r(&t, &tmBuf) == nullptr) {
        return msg;
    }
#endif
    char timeBuf[32];
    if (std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf) == 0) {
        return msg;
    }
    return std::string(timeBuf) + " " + msg;
}

} // namespace

void AppController::AppendMcpActivity(const std::string& line) {
    std::lock_guard<std::mutex> lock(mcpActivityMutex_);
    mcpActivityLog_.push_back(PrefixMcpActivityLine(line));
    while (mcpActivityLog_.size() > kMcpActivityLogMax) {
        mcpActivityLog_.pop_front();
    }
}

std::vector<std::string> AppController::CopyMcpActivityLog() const {
    std::lock_guard<std::mutex> lock(mcpActivityMutex_);
    return std::vector<std::string>(mcpActivityLog_.begin(), mcpActivityLog_.end());
}
#endif

void AppController::OpenUrl(const std::string& url) const {
    if (url.empty()) {
        return;
    }

    // Scheme allowlist: avoid handing `javascript:`, `file:`, `vbscript:`, etc.
    // Only http(s) and mailto pass through; anything else is rejected.
    {
        std::string schemePrefix;
        const size_t colonPos = url.find(':');
        if (colonPos == std::string::npos) {
            LOG_WARN("AppController::OpenUrl rejected: missing scheme in url=%s", TruncateForLog(url, 200).c_str());
            return;
        }
        schemePrefix.reserve(colonPos);
        for (size_t i = 0; i < colonPos; ++i) {
            schemePrefix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(url[i]))));
        }
        const bool ok = (schemePrefix == "http") || (schemePrefix == "https") || (schemePrefix == "mailto");
        if (!ok) {
            LOG_WARN("AppController::OpenUrl rejected: scheme '%s' not allowlisted for url=%s", schemePrefix.c_str(),
                     TruncateForLog(url, 200).c_str());
            return;
        }
    }

    if (OpenUrlHandler) {
        OpenUrlHandler(url);
        return;
    }

#if defined(_WIN32)
    const HINSTANCE openResult = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(openResult) <= 32) {
        LOG_ERROR("AppController::OpenUrl failed url=%s err=%ld", TruncateForLog(url, 300).c_str(), GetLastError());
    }
#elif defined(__APPLE__)
    if (!LaunchCommandNoShell("open", url)) {
        LOG_ERROR("AppController::OpenUrl failed to launch url=%s", TruncateForLog(url, 300).c_str());
    }
#else
    if (!LaunchCommandNoShell("xdg-open", url)) {
        LOG_ERROR("AppController::OpenUrl failed to launch url=%s", TruncateForLog(url, 300).c_str());
    }
#endif
}

void AppController::AddAutomationLogSink(std::function<void(const std::string&)> sink) {
    if (sink) {
        AutomationLogSinks.push_back(std::move(sink));
    }
}

void AppController::ClearAutomationLogSinks() { AutomationLogSinks.clear(); }

void AppController::SetAttachmentViewerHandler(AttachmentViewerHandler handler) {
    AttachmentViewerHandlerCallback = std::move(handler);
}

void AppController::SetAttachmentPreviewHandler(AttachmentPreviewHandler handler) {
    AttachmentPreviewHandlerCallback = std::move(handler);
}

void AppController::SetAttachmentCollectionHandler(AttachmentCollectionHandler handler) {
    AttachmentCollectionHandlerCallback = std::move(handler);
}

void AppController::SetOpenFilePathsHandler(OpenFilePathsHandler handler) {
    OpenFilePathsHandlerCallback = std::move(handler);
}

void AppController::RequestOpenFilePaths(bool allowMultiple, const std::string& initialDirectoryUtf8,
                                         std::function<void(std::vector<std::string>)> onComplete) const {
    if (!onComplete) {
        return;
    }
    if (OpenFilePathsHandlerCallback) {
        OpenFilePathsHandlerCallback(allowMultiple, initialDirectoryUtf8, std::move(onComplete));
        return;
    }
    onComplete({});
}

void AppController::Initialize(const std::string& dbPath, const std::string& backendType) {
    LOG_INFO("AppController::Initialize backendType=%s dbPath=%s", backendType.c_str(), dbPath.c_str());
    Cache = std::unique_ptr<LocalCacheManager>(new LocalCacheManager(dbPath));
    try {
        const size_t dropped = Cache->RunOneTimeLegacyDropPendingAtMaxAttempts();
        if (dropped > 0) {
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                          "Startup: dropped %zu legacy offline pending row(s) already at max retries "
                          "(not archived). They were removed from the active queue only.",
                          dropped);
            legacyPendingStartupBanner_ = buf;
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::Initialize legacy pending cleanup failed: %s", ex.what());
    } catch (...) {
        LOG_ERROR("AppController::Initialize legacy pending cleanup failed: unknown exception");
    }

    TrackerConfig cfg = ConfigManager::Load();
    std::string activeTracker = cfg.TrackerType;
    if (activeTracker.empty()) {
        activeTracker = "Jira";
    }

    std::string trackerLower = activeTracker;
    std::transform(trackerLower.begin(), trackerLower.end(), trackerLower.begin(), ::tolower);

    if (trackerLower == "plane") {
        Backend = std::unique_ptr<ITrackerClient>(new PlaneClient());
        LOG_INFO("AppController: Plane backend initialized.");
    } else {
        Backend = std::unique_ptr<ITrackerClient>(new JiraClient());
        LOG_INFO("AppController: Jira backend initialized.");
    }
    const std::string activeTrackerType = Backend ? Backend->GetTrackerType() : "Unknown";

    const std::string& fileBase = ConfigManager::GetFilesBaseDirectory();
    if (!fileBase.empty()) {
        luaScriptsDirectory_ = fileBase + "Scripts/";
    } else {
        luaScriptsDirectory_.clear();
    }
    LOG_INFO("AppController: ConfigManager files base %s (len=%zu); luaScriptsDirectory=\"%s\"",
             fileBase.empty() ? "empty" : "set", fileBase.size(), luaScriptsDirectory_.c_str());
    LogProcessCwdForScriptsDiagnostics();
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    LogLuaScriptFileProbe("SmatchetHooks.lua", ResolveLuaScriptPath("SmatchetHooks.lua"));
    LogLuaScriptFileProbe("Automation.lua", ResolveLuaScriptPath("Automation.lua"));
#else
    LOG_INFO("AppController: SMATCHET_WITH_LUA_AUTOMATION off — no Lua init in this build.");
#endif

    // Defer SyncWithBackend to first SmatchetUI::Draw so active view JQL/fields are
    // applied first — avoids fetching issues twice at startup.
    RefreshLocalData();

    {
        std::vector<TrackerField> snapFields;
        std::vector<TrackerComponent> snapComponents;
        std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta;
        std::string snapErr;
        const std::string cacheKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg);
        if (FieldCatalogCache::TryLoadFieldCatalogSnapshot(cacheKey, snapFields, snapComponents, snapIssueTypeMeta,
                                                           snapErr)) {
            AvailableFields = std::move(snapFields);
            AvailableComponents = std::move(snapComponents);
            AvailableIssueTypeMeta = std::move(snapIssueTypeMeta);
            fieldCatalogEverLoaded_ = true;
            LastTrackerFieldCatalogError.clear();
            if (activeTrackerType == "Plane") {
                LastTrackerFieldCatalogWarning =
                    "Working offline: Plane field catalog loaded from local snapshot until a live refresh succeeds.";
            } else {
                LastTrackerFieldCatalogWarning =
                    "Working offline: tracker field catalog loaded from local snapshot until a live refresh succeeds.";
            }
            if (activeTrackerType == "Jira") {
                for (auto& field : AvailableFields) {
                    if (field.Id == "comment" || field.Id == "timespent" || field.Id == "aggregatetimeoriginalestimate" ||
                        field.Id == "aggregatetimeestimate" || field.Id == "aggregatetimespent") {
                        field.ReadOnly = true;
                    }
                }
                EnsureCatalogHistoryField();
            }
            TrackerFieldCatalogRevision.fetch_add(1);
            LOG_INFO("AppController::Initialize: restored field catalog from snapshot (%zu fields)",
                     AvailableFields.size());
        }
    }

    TrackerConfig jiraCfgForEditMetaWarmup{};
    if (activeTrackerType == "Jira") {
        // Load before InitLua(): avoids parsing smatchet_config.json immediately after Lua init on MinGW release.
        jiraCfgForEditMetaWarmup = ConfigManager::Load();
    }

    InitLua();

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    RunLuaSetupScript("SmatchetHooks.lua");
    automationWorker_ = std::thread(&AppController::AutomationWorkerLoop, this);
#endif

    if (activeTrackerType == "Jira") {
        WarmIssueTypeEditMetaAtStartAsync(std::move(jiraCfgForEditMetaWarmup));
    }
}

std::string AppController::ResolveLuaScriptPath(const std::string& filename) const {
    if (filename.empty() || filename.find("..") != std::string::npos || filename.find(':') != std::string::npos ||
        (!filename.empty() && (filename[0] == '/' || filename[0] == '\\'))) {
        LOG_WARN("ResolveLuaScriptPath: blocked suspicious script path=%s", filename.c_str());
        return std::string();
    }
    if (!luaScriptsDirectory_.empty()) {
        return luaScriptsDirectory_ + filename;
    }
    return std::string("Scripts/") + filename;
}

std::vector<std::string> AppController::ListLuaScriptFiles() const {
    namespace fs = ghc::filesystem;
    std::vector<std::string> out;
    try {
        std::error_code ec;
        fs::path root;
        if (!luaScriptsDirectory_.empty()) {
            root = fs::path(luaScriptsDirectory_);
        } else {
            root = fs::path("Scripts");
        }
        if (!fs::is_directory(root, ec)) {
            return out;
        }
        for (const auto& ent : fs::directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            if (!ent.is_regular_file(ec)) {
                continue;
            }
            const std::string fname = ent.path().filename().string();
            if (fname.size() < 5) {
                continue;
            }
            std::string ext = fname.substr(fname.size() - 4);
            for (char& c : ext) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (ext != ".lua") {
                continue;
            }
            out.push_back(fname);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    } catch (const std::exception& ex) {
        LOG_WARN("ListLuaScriptFiles: exception (returning partial/empty): %s", ex.what());
        out.clear();
    } catch (...) {
        LOG_WARN("ListLuaScriptFiles: unknown exception (returning empty).");
        out.clear();
    }
    return out;
}

std::string AppController::ResolveFieldIconAssetPath(const std::string& pathOrUrl) const {
    namespace fs = ghc::filesystem;
    const std::string t = TrimCopyAsciiWhitespace(pathOrUrl);
    if (t.empty()) {
        return std::string();
    }
    if (t.rfind("https://", 0) == 0 || t.rfind("http://", 0) == 0) {
        return t;
    }
    std::error_code ec;
    auto isAllowedPath = [&](const fs::path& absPath) -> bool {
        const std::string absStr = absPath.string();
        if (!luaScriptsDirectory_.empty()) {
            const fs::path scriptsRoot = fs::weakly_canonical(fs::path(luaScriptsDirectory_), ec);
            if (!ec && FieldIconHasCaseInsensitivePrefix(absStr, scriptsRoot.string())) {
                return true;
            }
            ec.clear();
        }
        const std::string base = ConfigManager::GetFilesBaseDirectory();
        if (!base.empty()) {
            const fs::path baseRoot = fs::weakly_canonical(fs::path(base), ec);
            if (!ec && FieldIconHasCaseInsensitivePrefix(absStr, baseRoot.string())) {
                return true;
            }
            ec.clear();
        }
        return false;
    };

    fs::path inp(t);
    if (!inp.is_absolute()) {
        if (luaScriptsDirectory_.empty()) {
            return std::string();
        }
        std::string rel = t;
        if (rel.size() >= 7 && FieldIconHasCaseInsensitivePrefix(rel, "Scripts")) {
            rel = rel.size() == 7 ? std::string() : rel.substr(8);
        }
        const fs::path combined = fs::path(luaScriptsDirectory_) / fs::path(rel);
        const fs::path absRel = fs::weakly_canonical(combined, ec);
        if (ec || !isAllowedPath(absRel)) {
            return std::string();
        }
        return absRel.string();
    }
    const fs::path abs = fs::weakly_canonical(inp, ec);
    if (ec) {
        return std::string();
    }
    if (isAllowedPath(abs)) {
        return abs.string();
    }
    return std::string();
}

std::string AppController::GetAutomationScriptContent() {
    std::string path = ResolveLuaScriptPath("Automation.lua");
    if (path.empty()) return "";
    std::ifstream ifs(path);
    if (!ifs.is_open()) return "";
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

bool AppController::SaveAutomationScriptContent(const std::string& content, std::string& outError) {
    std::string path = ResolveLuaScriptPath("Automation.lua");
    if (path.empty()) {
        outError = "Invalid path";
        return false;
    }
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        outError = "Could not open file for writing: " + path;
        return false;
    }
    ofs << content;
    return true;
}

void AppController::ClearLastTrackerTicketSyncWarning() { LastTrackerTicketSyncWarning.clear(); }

TrackerIssueFetchPack AppController::FetchIssuesForActiveView(const TrackerConfig* configOverride,
                                                           const ViewsStore* viewsOverride) {
    TrackerIssueFetchPack pack;
    if (!Backend || !Cache) {
        return pack;
    }
    std::lock_guard<std::mutex> lock(g_TrackerIssueFetchMutex);
    pack.Tickets = Backend->FetchIssues(&pack.FullSyncCompleted, configOverride, viewsOverride, &pack.FetchError);
    return pack;
}

void AppController::ApplyIssueFetchPack(TrackerIssueFetchPack pack) {
    if (!Backend || !Cache) {
        LOG_WARN("AppController::ApplyIssueFetchPack skipped: backend=%d cache=%d", Backend ? 1 : 0, Cache ? 1 : 0);
        return;
    }
    LastTrackerTicketSyncWarning.clear();
    std::vector<CachedTicket>& freshTickets = pack.Tickets;
    const std::string& fetchError = pack.FetchError;
    const bool fullSyncCompleted = pack.FullSyncCompleted;

    if (!fetchError.empty() && IsTrackerTransportErrorText(fetchError)) {
        LastTrackerTicketSyncWarning = "Showing cached issues — live refresh did not complete: " + fetchError;
        LOG_WARN("AppController::ApplyIssueFetchPack transport-style fetch issue: %s", fetchError.c_str());
        lastTrackerConnectivityState_ = TrackerConnectivityState::TransportDown;
        const auto nowProbe = std::chrono::steady_clock::now();
        nextTrackerConnectivityProbeAt_ = nowProbe;
        PushOfflineReplayTimersDuringTransportOutage(nowProbe);
    } else if (fetchError.empty()) {
        requestDeferredLiveTrackerBackendSuccessNotify_();
    }
    size_t saved = 0;
    for (const auto& t : freshTickets) {
        Cache->SaveTicket(t);
        ++saved;
    }
    size_t deleted = 0;
    if (fullSyncCompleted) {
        std::unordered_set<std::string> keepIds;
        keepIds.reserve(freshTickets.size());
        for (const auto& t : freshTickets) {
            if (!t.id.empty()) {
                keepIds.insert(t.id);
            }
        }
        std::vector<CachedTicket> existing = Cache->GetAllTickets();
        for (const auto& row : existing) {
            if (keepIds.find(row.id) == keepIds.end()) {
                Cache->DeleteTicket(row.id);
                ++deleted;
            }
        }
    }
    LOG_INFO("AppController::ApplyIssueFetchPack finished fetched=%zu saved=%zu deleted=%zu fullSync=%d",
             freshTickets.size(), saved, deleted, fullSyncCompleted ? 1 : 0);
}

void AppController::SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride) {
    LOG_INFO("AppController::SyncWithBackend started.");

    // Resolve effective tracker type — prefer configOverride, else read from disk.
    std::string newTracker;
    if (configOverride) {
        newTracker = configOverride->TrackerType;
    } else {
        newTracker = ConfigManager::Load().TrackerType;
    }
    if (newTracker.empty()) newTracker = "Jira";

    const std::string currentType = Backend ? Backend->GetTrackerType() : "";
    const bool isCurrentlyJira  = (currentType == "Jira");
    const bool isCurrentlyPlane = (currentType == "Plane");

    std::string trackerLower = newTracker;
    std::transform(trackerLower.begin(), trackerLower.end(), trackerLower.begin(), ::tolower);

    if (trackerLower == "plane" && !isCurrentlyPlane) {
        Backend = std::unique_ptr<ITrackerClient>(new PlaneClient());
        LOG_INFO("AppController: Switched backend to Plane.");
    } else if (trackerLower == "jira" && !isCurrentlyJira) {
        Backend = std::unique_ptr<ITrackerClient>(new JiraClient());
        LOG_INFO("AppController: Switched backend to Jira.");
    }

    TrackerIssueFetchPack pack = FetchIssuesForActiveView(configOverride, viewsOverride);
    ApplyIssueFetchPack(std::move(pack));
    RefreshLocalDataAndWarmIssueTypeMeta();
}







