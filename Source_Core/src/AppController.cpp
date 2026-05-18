// winsock2.h must be the FIRST Win32-related include in this TU. Several headers below
// (ghc::filesystem, ConfigManager.h on Windows, ImGui backends) transitively pull
// <windows.h>, which auto-includes the legacy <winsock.h> unless WIN32_LEAN_AND_MEAN is
// defined. cpr/curl (via TrackerHttpUtils.h) then includes <winsock2.h>, which fires
// `#warning Please include winsock2.h before windows.h` from MinGW's headers. Putting the
// winsock2 block at the very top avoids both the warning and the symbol-conflict it
// foreshadows.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include "AppController.h"
#include "AppControllerDepsAdapter.h"

#if defined(SMATCHET_WITH_AGENTIC)
// Full definition for the AGENTIC build only — keeps SQLiteCpp out of the header.
// In the OFF build the forward declaration in AppController.h is sufficient for
// the unique_ptr<AgentProposalStore> member to be elided + accessor returns nullptr.
#include "AgentProposalStore.h"
#endif

#include <algorithm>
#include <array>

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
#include <sstream>

#include <string>

#include <thread>

#include <unordered_set>

#include <utility>

#include <vector>

#include "ConfigManager.h"

#include "Commands/BuiltinCommands.h"

#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"

#include "FieldCatalogCache.h"

#include <ghc/filesystem.hpp>

#include "DefaultTrackerBackendFactory.h"

#include "ITrackerBackendFactory.h"

#include "LuaAutomationHost.h"

#include "OfflineQueueService.h"

#include "TicketSyncService.h"

#include "TrackerHttpUtils.h"

#include "JqlProjectScope.h"
#include "Logger.h"

#include "StringUtil.h"

#include "Views.h"

#include "SmatchetUI.h"

#include "SmatchetToast.h"

#include "AiTypes.h"
#if defined(SMATCHET_WITH_AI)
#include "AiAssistantController.h"
#endif

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

bool RemoveLocalCacheDbFiles(const std::string& dbPathUtf8, std::string& outError) {
    namespace fs = ghc::filesystem;
    std::error_code ec;
    fs::path p(dbPathUtf8);
    if (!p.is_absolute()) {
        p = fs::absolute(p, ec);
        if (ec) {
            outError = "Could not resolve database path: " + ec.message();
            return false;
        }
    }
    const std::string stem = p.string();
    const fs::path paths[3] = {p, fs::path(stem + "-wal"), fs::path(stem + "-shm")};
    for (const auto& f : paths) {
        ec.clear();
        if (!fs::exists(f, ec)) {
            continue;
        }
        fs::remove(f, ec);
        if (ec) {
            outError = "Could not remove " + f.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

std::string TrimAppUpdateText(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    while (!text.empty() &&
           (text.front() == '\r' || text.front() == '\n' || text.front() == ' ' || text.front() == '\t')) {
        text.erase(text.begin());
    }
    return text;
}

struct SemanticVersion {
    int Major = 0;
    int Minor = 0;
    int Patch = 0;
    bool Valid = false;
};

SemanticVersion ParseSemanticVersion(const std::string& raw) {
    SemanticVersion out;
    std::string s = raw;
    if (!s.empty() && s.front() == 'v') {
        s.erase(s.begin());
    }
    const size_t dash = s.find('-');
    if (dash != std::string::npos) {
        s.resize(dash);
    }

    std::array<int, 3> parts{{0, 0, 0}};
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t dot = s.find('.', start);
        const std::string token = (dot == std::string::npos) ? s.substr(start) : s.substr(start, dot - start);
        if (token.empty() ||
            !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return out;
        }
        parts[static_cast<size_t>(i)] = std::atoi(token.c_str());
        if (dot == std::string::npos) {
            if (i != 2) {
                return out;
            }
            start = s.size();
        } else {
            start = dot + 1;
        }
    }
    if (start < s.size()) {
        return out;
    }

    out.Major = parts[0];
    out.Minor = parts[1];
    out.Patch = parts[2];
    out.Valid = true;
    return out;
}

int CompareSemanticVersion(const SemanticVersion& a, const SemanticVersion& b) {
    if (a.Major != b.Major) {
        return a.Major < b.Major ? -1 : 1;
    }
    if (a.Minor != b.Minor) {
        return a.Minor < b.Minor ? -1 : 1;
    }
    if (a.Patch != b.Patch) {
        return a.Patch < b.Patch ? -1 : 1;
    }
    return 0;
}

std::string FileNameFromUrl(const std::string& url) {
    const size_t slash = url.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= url.size()) {
        return std::string();
    }
    return url.substr(slash + 1);
}

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

void AppController::SetBackendFactory(std::unique_ptr<ITrackerBackendFactory> factory) {
    backendFactory_ = std::move(factory);
}

AppController::~AppController() {

    // Shutdown ordering matters here — every background thread that can post to mainThreadDispatcher
    // or read `this` via __smatchet_app must be joined BEFORE member destruction begins. This
    // matches the contract described in MainThreadDispatcher.h and AppController_LuaBindings.cpp.
#if defined(SMATCHET_WITH_AGENTIC)
    // Join the scheduled-poll worker (T7) before any other teardown. The worker only touches
    // its own members + the SQLite store + the cached triage adapters, so it does not interact
    // with mainThreadDispatcher / AiAssistantController; ordering relative to those is loose.
    // Joining here guarantees no worker iteration is in flight while later members destruct.
    StopAgenticPoll();
    // (Bundle A) Reap any thread that was detached for async-drain but has not yet been
    // joined by `JoinDetachedAgenticPollIfReady`. After `StopAgenticPoll` returns, any
    // detached thread is by definition a previous-incarnation worker that observed the
    // stop atom long ago — its join is near-free.
    {
        std::lock_guard<std::mutex> lifecycleLk(agenticPollLifecycleMutex_);
        if (agenticPollDetachedThread_.joinable()) {
            agenticPollDetachedThread_.join();
        }
    }
#endif
#if defined(SMATCHET_WITH_AI)
    // Drop the AI assistant first — its worker may still be inside SendStreaming. The
    // dtor flips the cancel atom + joins; only after the join can the main-thread
    // dispatcher safely BeginShutdown(), because callbacks already in flight will
    // hand off through `mainThreadDispatcher.PostToMainThread` (still accepting posts
    // at this instant) and the controller's join blocks until those callbacks return.
    aiAssistant_.reset();
#endif

    mainThreadDispatcher.BeginShutdown();

    CancelAndJoinActiveStreamingSync();

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

        for (const auto& t : tickets) {

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
            // A background worker called JoinBackgroundTasks — cannot self-join.
            // Re-queue so ~AppController can join it from the main thread.
            // Detaching was the previous behaviour but let detached threads outlive
            // the controller and race against g_TrackerIssueFetchMutex teardown.
            LOG_WARN("AppController::JoinBackgroundTasks: self-join detected; re-queuing thread for main-thread join.");
            std::lock_guard<std::mutex> requeue_lock(backgroundWorkersMutex_);
            backgroundWorkers_.push_back(std::move(worker));
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

void AppController::SetRequestAppQuitHandler(std::function<void()> handler) {
    RequestAppQuitHandler = std::move(handler);
}

void AppController::RequestAppQuit() const {

    if (RequestAppQuitHandler) {

        RequestAppQuitHandler();
    }
}

void AppController::SetRuntimePluginHost(PluginHost* host) { runtimePluginHost_ = host; }

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

void AppController::NotifyMcpClientHttpActivity() {

    mcpHttpTrafficEpoch_.fetch_add(1, std::memory_order_relaxed);

    const auto now = std::chrono::steady_clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    mcpLastClientHttpActivityNs_.store(static_cast<std::uint64_t>(ns), std::memory_order_release);
}

std::uint64_t AppController::GetMcpHttpTrafficEpoch() const {

    return mcpHttpTrafficEpoch_.load(std::memory_order_acquire);
}

bool AppController::TryGetMcpLastClientHttpActivity(std::chrono::steady_clock::time_point* out) const {

    const std::uint64_t raw = mcpLastClientHttpActivityNs_.load(std::memory_order_acquire);

    if (raw == 0 || out == nullptr) {

        return false;
    }

    *out = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(raw)));

    return true;
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

// AddAutomationLogSink / ClearAutomationLogSinks moved to LuaAutomationHost in Phase 1A of
// the item 14 extraction. Thin delegators below.

void AppController::AddAutomationLogSink(std::function<void(const std::string&)> sink) {
    if (luaHost_) {
        luaHost_->AddAutomationLogSink(std::move(sink));
    } else {
        // OnEarlyInit fires before Initialize constructs luaHost_. Buffer the sink;
        // Initialize drains pendingLogSinks_ into luaHost_ immediately after construction.
        if (sink) {
            pendingLogSinks_.push_back(std::move(sink));
        }
    }
}

void AppController::ClearAutomationLogSinks() {
    if (luaHost_) {
        luaHost_->ClearAutomationLogSinks();
    }
}

void AppController::AddAutomationErrorSink(std::function<void(const std::string&)> sink) {
    errorSinks_.push_back(std::move(sink));
}

bool AppController::ConsumeScriptingWindowRequest() { return scriptingWindowOpenRequested_.exchange(false); }

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

std::string AppController::GetAppVersion() const {
#ifdef SMATCHET_APP_VERSION
    return SMATCHET_APP_VERSION;
#else
    return "0.0.0";
#endif
}

std::string AppController::GetGitHubReleaseRepo() const {
#ifdef SMATCHET_GITHUB_RELEASE_REPO
    return SMATCHET_GITHUB_RELEASE_REPO;
#else
    return "alexandrosk0/Smatchet";
#endif
}

AppUpdateInfo AppController::CheckForAppUpdate(bool includePrerelease) const {
    AppUpdateInfo out;
    out.CurrentVersion = GetAppVersion();

    const TrackerConfig cfg = ConfigManager::Load();
    const std::string repo = cfg.UpdateGithubRepo.empty() ? GetGitHubReleaseRepo() : cfg.UpdateGithubRepo;
    if (repo.empty()) {
        out.Error = "No GitHub release repository configured.";
        return out;
    }

    const std::string url = "https://api.github.com/repos/" + repo + "/releases?per_page=10";
    cpr::Header headers{{"Accept", "application/vnd.github+json"},
                        {"User-Agent", "SmatchetUpdater/" + out.CurrentVersion}};
    cpr::Response response =
        cpr::Get(cpr::Url{url}, headers, cpr::Redirect{true, true}, cpr::ConnectTimeout{5000}, cpr::Timeout{15000});
    if (response.error.code != cpr::ErrorCode::OK) {
        out.Error = "Update check failed: " + response.error.message;
        return out;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        out.Error = "Update check failed: GitHub returned HTTP " + std::to_string(response.status_code);
        return out;
    }

    nlohmann::json releases;
    try {
        releases = nlohmann::json::parse(response.text);
    } catch (const std::exception& ex) {
        out.Error = std::string("Update check failed to parse GitHub response: ") + ex.what();
        return out;
    }
    if (!releases.is_array()) {
        out.Error = "Update check failed: unexpected GitHub response shape.";
        return out;
    }

    const SemanticVersion current = ParseSemanticVersion(out.CurrentVersion);
    for (const auto& release : releases) {
        if (!release.is_object()) {
            continue;
        }
        if (release.value("draft", false)) {
            continue;
        }
        if (!includePrerelease && release.value("prerelease", false)) {
            continue;
        }

        const std::string tag = release.value("tag_name", std::string());
        const SemanticVersion candidate = ParseSemanticVersion(tag);
        if (!candidate.Valid) {
            continue;
        }

        out.Ok = true;
        out.ReleaseTag = tag;
        out.LatestVersion = !tag.empty() && tag.front() == 'v' ? tag.substr(1) : tag;
        out.ReleaseUrl = release.value("html_url", std::string());
        out.ReleaseNotes = TrimAppUpdateText(release.value("body", std::string()));

        if (release.contains("assets") && release["assets"].is_array()) {
            for (const auto& asset : release["assets"]) {
                if (!asset.is_object()) {
                    continue;
                }
                const std::string assetName = asset.value("name", std::string());
                if (assetName.find("-windows-setup.exe") != std::string::npos) {
                    out.InstallerAsset.Name = assetName;
                    out.InstallerAsset.DownloadUrl = asset.value("browser_download_url", std::string());
                    break;
                }
            }
        }

        if (current.Valid && CompareSemanticVersion(candidate, current) <= 0) {
            out.UpdateAvailable = false;
            return out;
        }

        out.UpdateAvailable = !out.InstallerAsset.DownloadUrl.empty();
        if (!out.UpdateAvailable && out.Error.empty()) {
            out.Error = "A newer release exists, but no Windows installer asset was found.";
        }
        return out;
    }

    out.Ok = true;
    out.UpdateAvailable = false;
    return out;
}

bool AppController::DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl, const std::string& assetName,
                                                     std::string& outError,
                                                     std::shared_ptr<std::atomic<bool>> cancelFlag) const {
    outError.clear();
    if (downloadUrl.empty()) {
        outError = "Missing installer download URL.";
        return false;
    }
#if defined(_WIN32)
    char tempPathBuf[MAX_PATH] = {};
    const DWORD tempPathLen = GetTempPathA(static_cast<DWORD>(sizeof(tempPathBuf)), tempPathBuf);
    if (tempPathLen == 0 || tempPathLen >= sizeof(tempPathBuf)) {
        outError = "Failed to resolve temp directory.";
        return false;
    }

    std::string filename = assetName.empty() ? FileNameFromUrl(downloadUrl) : assetName;
    if (filename.empty()) {
        filename = "SmatchetUpdateSetup.exe";
    }
    const std::string localPath = std::string(tempPathBuf) + filename;
    std::ofstream ofs(localPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        outError = "Failed to create installer download file: " + localPath;
        return false;
    }

    cpr::Header headers{{"Accept", "application/octet-stream"}, {"User-Agent", "SmatchetUpdater/" + GetAppVersion()}};
    cpr::Redirect redirect(true, true);
    bool cancelled = false;
    cpr::WriteCallback writeCb{[&](const std::string& data, intptr_t) -> bool {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            cancelled = true;
            return false;
        }
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        return ofs.good();
    }};
    cpr::Response resp =
        cpr::Get(cpr::Url{downloadUrl}, headers, redirect, writeCb, cpr::ConnectTimeout{5000}, cpr::Timeout{120000});
    ofs.close();
    if (cancelled) {
        std::remove(localPath.c_str());
        outError = "Download cancelled.";
        return false;
    }
    if (resp.error.code != cpr::ErrorCode::OK || resp.status_code < 200 || resp.status_code >= 300) {
        std::remove(localPath.c_str());
        outError = "Failed to download installer.";
        if (!resp.error.message.empty()) {
            outError += " " + resp.error.message;
        } else if (resp.status_code > 0) {
            outError += " HTTP " + std::to_string(resp.status_code);
        }
        return false;
    }

    const HINSTANCE openResult = ShellExecuteA(nullptr, "open", localPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(openResult) <= 32) {
        outError = "Failed to launch downloaded installer.";
        return false;
    }

    RequestAppQuit();
    return true;
#else
    (void)assetName;
    (void)downloadUrl;
    (void)cancelFlag;
    outError = "Installer updates are currently supported only on Windows.";
    return false;
#endif
}

bool AppController::IsOnUiThread() const {
    // uiThreadId_ is written once in Initialize before any worker is spawned, then never
    // mutated. Reads from worker threads are race-free under publish-once semantics.
    return std::this_thread::get_id() == uiThreadId_;
}

void AppController::Initialize(const std::string& dbPath, const std::string& backendType) {
    // Record the UI thread identity first. Initialize() is invoked from main() before any
    // background thread is spawned, so this happens-before any worker that could call
    // IsOnUiThread() later. See AppController.h for the full reasoning.
    uiThreadId_ = std::this_thread::get_id();

    LOG_INFO("AppController::Initialize backendType=%s dbPath=%s", backendType.c_str(), dbPath.c_str());

    localCacheDbPath_ = dbPath;

    Cache = std::make_unique<LocalCacheManager>(dbPath);

#if defined(SMATCHET_WITH_AGENTIC)
    // Agentic proposal store — sibling SQLite file next to the local cache. Resolves to
    // <userdata>/agent_proposals.sqlite when the platform user-data dir is available;
    // falls back to the cache's parent directory otherwise. Both DBs use SQLite's own
    // per-connection locking, so colocating them on disk is safe.
    // (Bundle A) AgentProposalStore construction (SQLite open + WAL pragmas +
    // 2-table create + version check + migration ladder) is multi-ms on slow
    // disks and a code-review CRITICAL on the UI thread per Pillar 2. Defer to
    // a worker thread so `Initialize` returns fast; callers null-check the
    // accessor and degrade gracefully for the brief init window.
    std::string agenticDbPath;
    {
        const std::string userDataDir = ConfigManager::GetPlatformSharedUserDataDirectory();
        if (!userDataDir.empty()) {
            agenticDbPath = userDataDir + "agent_proposals.sqlite";
        } else {
            // Derive a sibling path from the cache db path so dev / portable runs still get
            // a deterministic location without a user-data dir.
            const auto slash = dbPath.find_last_of("/\\");
            const std::string dir = (slash == std::string::npos) ? std::string() : dbPath.substr(0, slash + 1);
            agenticDbPath = dir + "agent_proposals.sqlite";
        }
    }
    LaunchBackgroundTask([this, agenticDbPath]() { this->InitAgentProposalStoreOnWorker(agenticDbPath); });
#endif

    // Construct the deps adapter eagerly so OfflineQueueService + TicketSyncService can capture
    // an interface reference at construction time. The adapter is owned by this AppController
    // and outlives both services (per the destructor ordering: ~AppController joins the
    // streaming-sync worker via `CancelAndJoinActiveStreamingSync` before any member is
    // destroyed, so the adapter is live for every `deps_.X` call).
    if (!depsAdapter_) {
        depsAdapter_ = std::make_unique<AppControllerDepsAdapter>(*this);
    }
    // Construct OfflineQueueService eagerly so the legacy-pending startup migration below
    // can write to `offlineQueue_->legacyPendingStartupBanner_` (item 12 extraction).
    if (!offlineQueue_) {
        offlineQueue_ = std::make_unique<OfflineQueueService>(*depsAdapter_);
    }
    // Construct TicketSyncService alongside — its `CancelAndJoinActiveStreamingSync` is called
    // by `RecreateLocalCacheDatabase` (which the legacy-pending cleanup below may trigger),
    // so the service must exist before that path runs (item 11 extraction).
    if (!ticketSync_) {
        ticketSync_ = std::make_unique<TicketSyncService>(*depsAdapter_);
    }
    // Construct LuaAutomationHost so `AddAutomationLogSink` calls from plugins'
    // OnEarlyInit have a target (item 14 extraction, Phase 1A).
    if (!luaHost_) {
        luaHost_ = std::make_unique<LuaAutomationHost>();
    }
    // Drain sinks buffered by AddAutomationLogSink calls during OnEarlyInit (which runs
    // before Initialize). From this point forward AddAutomationLogSink forwards directly.
    for (auto& s : pendingLogSinks_) {
        luaHost_->AddAutomationLogSink(std::move(s));
    }
    pendingLogSinks_.clear();

    try {

        const size_t dropped = Cache->RunOneTimeLegacyDropPendingAtMaxAttempts();

        if (dropped > 0) {

            char buf[384];

            std::snprintf(buf, sizeof(buf),

                          "Startup: dropped %zu legacy offline pending row(s) already at max retries "

                          "(not archived). They were removed from the active queue only.",

                          dropped);

            offlineQueue_->legacyPendingStartupBanner_ = buf;
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

    if (!backendFactory_) {
        backendFactory_ = std::make_unique<DefaultTrackerBackendFactory>();
    }
    Backend = backendFactory_->Create(activeTracker);
    if (!Backend) {
        LOG_ERROR("AppController: tracker backend factory returned null for type '%s'.", activeTracker.c_str());
    } else {
        LOG_INFO("AppController: %s backend initialized.", Backend->GetTrackerType().c_str());
    }

    const std::string activeTrackerType = Backend ? Backend->GetTrackerType() : "Unknown";

    // PR 5 of docs/design/applied/remove-global-project-key.md: one-shot legacy-project sweeps.
    // Drain legacy global project state into per-entity carriers (offline-queue payloads,
    // Plane view query JSON). Each sweep is guarded by its own `cache_meta` flag so it runs
    // exactly once per database file; subsequent launches are no-ops.
    if (offlineQueue_) {
        try {
            // PR 7: legacy carriers removed from TrackerConfig. Pass empty values; the sweep's
            // `legacy_project_swept_v1` cache_meta marker short-circuits on already-migrated installs.
            offlineQueue_->RunLegacyProjectSweep(std::string(), std::string(), activeTrackerType);
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::Initialize legacy-project offline sweep failed: %s", ex.what());
        }
    }
    if (Backend && Cache && activeTrackerType == "Plane") {
        try {
            static const std::string kPlaneSweepFlag = "legacy_plane_view_swept_v1";
            if (!Cache->HasCacheMetaFlag(kPlaneSweepFlag)) {
                // PR 7: legacy `plane_project_id` carrier removed. Walk the views once to log
                // any that still lack project scope, then set the marker so we never look again.
                PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
                const std::string backendKey = ConfigManager::NormalizeViewsBackendKey("Plane");
                auto bucketIt = disk.Backends.find(backendKey);
                if (bucketIt != disk.Backends.end()) {
                    for (const ViewDefinition& view : bucketIt->second.Views) {
                        const std::string extracted = Backend->ExtractProjectFromQuery(view.Jql);
                        if (!extracted.empty()) {
                            continue;
                        }
                        LOG_WARN("Plane view '%s' has no project scope; pick a project in the view editor",
                                 view.Name.c_str());
                    }
                }
                Cache->SetCacheMetaFlag(kPlaneSweepFlag);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::Initialize legacy Plane-view sweep failed: %s", ex.what());
        }
    }

    const std::string& fileBase = ConfigManager::GetRuntimeAssetDirectory();

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

        // PR 6: legacy global project fields removed. Initial catalog load at startup is now
        // unscoped (project key = ""); per-project catalogs are populated lazily on demand
        // through RefreshFieldCatalogForProject() driven by the new-issue draft / picker UI.
        const std::string projectKeyForCache;
        const std::string cacheKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, projectKeyForCache);

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

                    if (field.Id == "comment" || field.Id == "timespent" ||
                        field.Id == "aggregatetimeoriginalestimate" ||

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

    // Scenario runner — constructed before the registry so scenario.* commands
    // can capture a reference to it in their handlers.
    scenarioRunner_.reset(new smatchet::cmd::ScenarioRunner());
    // Register built-in scenario factories. Additional scenarios are added by
    // plugging a new .cpp + RegisterFactory call here.
    scenarioRunner_->RegisterFactory("priority-grid-scroll", []() {
        // Concrete type defined in PriorityGridScrollScenario.cpp.
        // Use a forward-declared factory function declared in the same TU as the scenario.
        // Because we cannot include the concrete .cpp type here, we use a free function
        // defined in that file that returns a unique_ptr<IScenario>.
        extern std::unique_ptr<smatchet::cmd::IScenario> MakePriorityGridScrollScenario();
        return MakePriorityGridScrollScenario();
    });
    scenarioRunner_->RegisterFactory("lua-recorder-fuzz", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeLuaRecorderFuzzScenario();
        return MakeLuaRecorderFuzzScenario();
    });
    scenarioRunner_->RegisterFactory("ui-test", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeUiTestScenario();
        return MakeUiTestScenario();
    });
    // Phase 7 bucket-C screenshot-diff scenarios (test-suite-expansion-
    // completion plan). Each scenario drives the UI to a known steady state,
    // then triggers debug.window.screenshot so the bash driver can diff the
    // captured PPM against tests/golden/<name>.ppm.
    scenarioRunner_->RegisterFactory("dock-gap-sentinel", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeDockGapSentinelScenario();
        return MakeDockGapSentinelScenario();
    });
    scenarioRunner_->RegisterFactory("command-palette-fuzzy", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeCommandPaletteFuzzyScenario();
        return MakeCommandPaletteFuzzyScenario();
    });
    // perf-tooling-bundle scenarios — 5 perf scenarios surfaced by the
    // perf-detective audit on develop@31e1893. Each verifies a previously-
    // shipped pillar-1 / pillar-2 fix doesn't regress, or establishes the
    // baseline floor against which other scenarios are compared.
    scenarioRunner_->RegisterFactory("idle", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeIdleScenario();
        return MakeIdleScenario();
    });
    scenarioRunner_->RegisterFactory("cell-edit-burst", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeCellEditBurstScenario();
        return MakeCellEditBurstScenario();
    });
    scenarioRunner_->RegisterFactory("attachment-preview-open", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeAttachmentPreviewOpenScenario();
        return MakeAttachmentPreviewOpenScenario();
    });
    scenarioRunner_->RegisterFactory("preferences-slider-drag", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakePreferencesSliderDragScenario();
        return MakePreferencesSliderDragScenario();
    });
    scenarioRunner_->RegisterFactory("long-text-open-large-adf", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeLongTextOpenLargeAdfScenario();
        return MakeLongTextOpenLargeAdfScenario();
    });

#if defined(SMATCHET_WITH_WHISPER)
    // Phase G — end-to-end whisper-dictation regression gate. The scenario
    // TU is source-list-conditional (only added to CORE_SOURCES when the
    // CMake option is ON), so the factory call must be ifdef-wrapped too:
    // the OFF build has no symbol for MakeWhisperDictationScenario.
    scenarioRunner_->RegisterFactory("whisper-dictation-roundtrip", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeWhisperDictationScenario();
        return MakeWhisperDictationScenario();
    });
#endif

#if defined(SMATCHET_WITH_AGENTIC)
    // End-to-end agentic-triage regression gate. The scenario wires
    // AgenticTriageController against in-process mock IGitHubReadClient +
    // IInferenceClient seams so no live HTTP / LLM traffic is needed. The
    // scenario TU is source-list-conditional on SMATCHET_WITH_AGENTIC; the
    // factory call is ifdef-wrapped because the OFF build excludes the
    // controller + store symbols the scenario depends on.
    scenarioRunner_->RegisterFactory("agent-triage-roundtrip", []() {
        extern std::unique_ptr<smatchet::cmd::IScenario> MakeAgentTriageScenario();
        return MakeAgentTriageScenario();
    });
#endif

    // Unified Command System — register the catalog last so handlers can capture
    // references to AppController state that's now fully wired (tracker backend,
    // Lua host, offline queue, etc.). See docs/design/applied/command-system-plan.md.
    try {
        commandRegistry_.reset(new smatchet::cmd::CommandRegistry());
        commandRegistry_->LoadRecents();
        smatchet::cmd::RegisterBuiltinCommands(*commandRegistry_, *this);
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::Initialize: CommandRegistry init failed: %s", ex.what());
        // Surface as a degraded registry rather than aborting startup — CLI / MCP
        // / Lua callers will see `not-connected` or empty `commands.list`.
        commandRegistry_.reset();
    } catch (...) {
        LOG_ERROR("AppController::Initialize: CommandRegistry init failed: unknown exception");
        commandRegistry_.reset();
    }

    // One-time audit per design doc §2.8: list saved views whose JQL has no
    // project scope. After PR 6 removes the global project key, those views
    // will broaden to "all projects you can read".
    static bool s_loggedViewsWithoutProjectScope = false;
    if (!s_loggedViewsWithoutProjectScope) {
        s_loggedViewsWithoutProjectScope = true;
        try {
            const TrackerConfig auditCfg = ConfigManager::Load();
            if (auditCfg.TrackerType == "Jira" || auditCfg.TrackerType.empty()) {
                const ViewsStore auditViews = ConfigManager::LoadViewsOrBootstrap(auditCfg);
                std::string namesList;
                size_t count = 0;
                for (const auto& v : auditViews.Views) {
                    if (!JqlProjectScope::HasProjectScope(v.Jql)) {
                        if (!namesList.empty()) {
                            namesList += ", ";
                        }
                        namesList += v.Name;
                        ++count;
                    }
                }
                if (count > 0) {
                    LOG_INFO("Views without project scope: [%s]", namesList.c_str());
                }
            }
        } catch (const std::exception& ex) {
            LOG_WARN("AppController::Initialize: project-scope audit failed: %s", ex.what());
        } catch (...) {
        }
    }

#if defined(SMATCHET_WITH_AI)
    // Construct the Smatchet Assistant controller last so it captures a settled config
    // snapshot (ConfigManager::Load() above already populated all Ai* fields). The
    // controller spawns its worker thread in its own constructor — no further wiring
    // needed. Lifetime contract: destroyed at the top of ~AppController.
    try {
        aiAssistant_ = std::unique_ptr<AiAssistantController>(new AiAssistantController(*this));
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::Initialize: AiAssistantController init failed: %s", ex.what());
        aiAssistant_.reset();
    } catch (...) {
        LOG_ERROR("AppController::Initialize: AiAssistantController init failed: unknown exception");
        aiAssistant_.reset();
    }
#endif

#if defined(SMATCHET_WITH_AGENTIC)
    // T7 — spawn the scheduled-poll worker AFTER all of agentProposalStore_ + AiAssistantController
    // + ConfigManager::Load have settled. The worker is opt-in (cfg.AgenticPollEnabled defaults off);
    // when off, StartAgenticPollIfEnabled is a fast no-op. Lifetime: joined in the destructor.
    StartAgenticPollIfEnabled();
#endif
}

#if defined(SMATCHET_WITH_AI)
AiAssistantController& AppController::GetAiAssistantController() {
    // No lazy construction. Callers MUST guard with HasAiAssistantController()
    // first — the controller is constructed exactly once at the end of
    // AppController::Initialize and destroyed at the top of ~AppController.
    // A lazy ctor here would race with shutdown: if a worker callback or Lua
    // glue called Get* during/after ~AppController, it would spawn a fresh
    // controller with no joiner, leaving a dangling thread at process exit.
    //
    // The previous lazy-fallback was originally added because Initialize could
    // throw on a bad provider, but the try/catch in Initialize already swallows
    // the exception and leaves aiAssistant_ null — at which point
    // HasAiAssistantController() correctly returns false and callers handle it.
    return *aiAssistant_; // pre-condition: HasAiAssistantController() == true
}
#endif

void AppController::AddAiContext(const AiContextBlock& block) {
#if defined(SMATCHET_WITH_AI)
    if (aiAssistant_) {
        aiAssistant_->AddAiContext(block);
    }
#else
    (void)block;
#endif
}

void AppController::ClearAiContext() {
#if defined(SMATCHET_WITH_AI)
    if (aiAssistant_) {
        aiAssistant_->ClearAiContext();
    }
#endif
}

std::vector<AiContextBlock> AppController::GetAiContext() const {
#if defined(SMATCHET_WITH_AI)
    if (aiAssistant_) {
        return aiAssistant_->GetAiContext();
    }
#endif
    return {};
}

void AppController::PromptAi(const std::string& prompt) {
#if defined(SMATCHET_WITH_AI)
    if (aiAssistant_) {
        // Use a process-local counter so the panel-side and Lua-side turn-gens never
        // collide. Reading `g_ui.assistantTurnGen` would be safer but pulls a UI-side
        // global into AppController; for Phase B the controller's caller (the UI panel)
        // owns the gen-counter mutation and Lua glue lands in Phase E.
        static std::atomic<uint64_t> s_promptAiSeq{1ULL << 32};
        const uint64_t turnGen = s_promptAiSeq.fetch_add(1, std::memory_order_relaxed);
        aiAssistant_->Submit(turnGen, prompt, aiAssistant_->GetAiContext());
    }
#else
    (void)prompt;
#endif
}

AgentProposalStore* AppController::GetAgentProposalStore() noexcept {
#if defined(SMATCHET_WITH_AGENTIC)
    // Fast path: once the ready atom flips, the unique_ptr is stable for the
    // lifetime of AppController (cleared only in the destructor after every
    // worker that could touch the store has been joined). Probing the atom
    // first lets the common steady-state case skip the mutex.
    if (!agentProposalStoreReady_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(agentProposalStoreMutex_);
    return agentProposalStore_.get();
#else
    // OFF build: no member exists, so the only correct answer is nullptr. Callers using
    // the documented `if (auto* s = app.GetAgentProposalStore())` pattern degrade silently.
    return nullptr;
#endif
}

#if defined(SMATCHET_WITH_AGENTIC)
void AppController::InitAgentProposalStoreOnWorker(const std::string& dbPath) {
    // Worker-thread entry: opens the SQLite store, runs migrations, then publishes
    // the unique_ptr under the mutex and flips the ready atom. The unique_ptr is
    // constructed outside the mutex so the multi-ms SQLite open never holds the lock
    // (UI-thread accessors would otherwise contend for the duration of init).
    std::unique_ptr<AgentProposalStore> store;
    try {
        store.reset(new AgentProposalStore(dbPath));
    } catch (const std::exception& ex) {
        // Initialisation failure is non-fatal — the agentic flow degrades gracefully via the
        // nullptr-accessor pattern. The user can still operate the app without triage.
        LOG_WARN("AppController: AgentProposalStore init failed: %s", ex.what());
        // Mark ready anyway so callers stop probing — accessor still returns nullptr.
        agentProposalStoreReady_.store(true, std::memory_order_release);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(agentProposalStoreMutex_);
        agentProposalStore_ = std::move(store);
    }
    agentProposalStoreReady_.store(true, std::memory_order_release);
    LOG_INFO("AppController: AgentProposalStore init complete (path=%s)", dbPath.c_str());
}
#endif

#if defined(SMATCHET_WITH_AGENTIC)
namespace {

// Concrete-to-interface bridges. Live in an anonymous namespace inside the
// AGENTIC=ON build of AppController.cpp because they are an implementation
// detail of `AppController::GetAgenticTriageController` — no other TU should
// see them. The bridges are thin: each virtual is a one-line forward.
class GitHubReadAdapter : public smatchet::agentic::IGitHubReadClient {
  public:
    explicit GitHubReadAdapter(GitHubClient& impl) : impl_(impl) {}
    bool FetchIssueBody(const std::string& issueKey, std::string& outBody, std::string& outError) override {
        return impl_.FetchIssueBody(issueKey, outBody, outError);
    }
    bool FetchIssueComments(const std::string& issueKey, std::vector<TrackerIssueComment>& outComments,
                            std::string& outError) override {
        return impl_.FetchIssueComments(issueKey, outComments, outError);
    }
    bool ListOpenIssuesForRepo(const std::string& owner, const std::string& repo, std::vector<std::string>& outKeys,
                               std::string& outError, std::int64_t sinceUnixSec = 0) override {
        return impl_.ListOpenIssuesForRepo(owner, repo, outKeys, outError, sinceUnixSec);
    }

  private:
    GitHubClient& impl_;
};

class InferenceAdapter : public smatchet::agentic::IInferenceClient {
  public:
    explicit InferenceAdapter(AgenticInferenceClient& impl) : impl_(impl) {}
    bool RequestProposals(const std::string& issueBody, const std::vector<TrackerIssueComment>& comments,
                          std::vector<AgenticInferenceClientPure::ProposalDraft>& outDrafts,
                          std::string& outError) override {
        return impl_.RequestProposals(issueBody, comments, outDrafts, outError);
    }

  private:
    AgenticInferenceClient& impl_;
};

} // namespace

smatchet::agentic::AgenticTriageController* AppController::GetAgenticTriageController() noexcept {
    if (agenticTriageController_) {
        return agenticTriageController_.get();
    }
    if (!agentProposalStore_) {
        // T4 store init failed (already logged at construction). Without a store there's
        // nowhere to persist proposals, so triage degrades cleanly to nullptr.
        return nullptr;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    if (cfg.GitHubPat.empty()) {
        // Degraded mode — caller surfaces a "configure GitHub PAT" message.
        return nullptr;
    }
    // GitHub base URL is hard-defaulted by GitHubClient when the argument is empty;
    // there's no separate cfg.GitHubBaseUrl today (GitHub Enterprise lands in a later phase).
    agenticGithubClient_.reset(new GitHubClient(std::string(), cfg.GitHubPat));
    agenticInferenceClient_.reset(new AgenticInferenceClient());
    agenticGithubReadAdapter_.reset(new GitHubReadAdapter(*agenticGithubClient_));
    agenticInferenceAdapter_.reset(new InferenceAdapter(*agenticInferenceClient_));
    agenticTriageController_.reset(new smatchet::agentic::AgenticTriageController(
        agenticGithubReadAdapter_.get(), agenticInferenceAdapter_.get(), agentProposalStore_.get()));
    return agenticTriageController_.get();
}

// ─── T7 scheduled-poll worker ──────────────────────────────────────────────
//
// The worker loop reads its settings from `ConfigManager::Load()` on every
// iteration (not just at start) so a Preferences edit takes effect on the next
// sleep wake without forcing the user to restart the app. `RestartAgenticPoll`
// joins the existing thread + reconstructs the triage controller (the PAT or
// repo query may have changed) before the new worker spawns.
//
// Cursor durability: per-iteration the worker reads the previous
// `last_seen_updated_at` from `agent_poll_cursor` via the proposal store,
// passes it to `IGitHubReadClient::ListOpenIssuesForRepo` as the `since=`
// filter, and after the batch completes it writes the *current* wall-clock
// time back as the new cursor. We use wall-clock (not the max updated_at of
// the returned issues) because GitHub guarantees monotonic `updated_at`
// timestamps and any drift between server + client is bounded by the cursor's
// "since" granularity (seconds).
//
// Threading: the worker thread never touches ImGui state directly — proposals
// land in the SQLite store, which the T6 UI panel reads through its 1-Hz cache.
// Cursor uses GitHub's since= filter to only fetch issues updated after last poll
// — bounded API cost, predictable LLM token spend.

void AppController::AgenticPollWorkerLoop() {
    LOG_INFO("AppController::AgenticPollWorkerLoop: started");
    // (Bundle A) Wait for the deferred AgentProposalStore init to finish before the
    // first iteration. Without this guard the worker would race a still-constructing
    // store and surface "store unavailable" log spam on the first wake-up after a
    // fresh launch. Short-circuit on stop so a UI-thread shutdown during the wait
    // exits cleanly.
    while (!agenticPollShouldStop_.load() && !agentProposalStoreReady_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(agenticPollMu_);
        agenticPollCv_.wait_for(lk, std::chrono::milliseconds(200), [this]() {
            return agenticPollShouldStop_.load() || agentProposalStoreReady_.load(std::memory_order_acquire);
        });
    }
    while (!agenticPollShouldStop_.load()) {
        TrackerConfig cfg = ConfigManager::Load();
        // Re-check the preconditions every iteration so a runtime config edit (Preferences
        // flips PAT off, or clears the query) cleanly stops the loop on the next wake without
        // requiring the user to also flip the master toggle.
        if (!cfg.AgenticPollEnabled || cfg.GitHubPat.empty() || cfg.AgenticPollQuery.empty()) {
            LOG_INFO("AppController::AgenticPollWorkerLoop: precondition lost (enabled=%d pat_empty=%d query_empty=%d) "
                     "— exiting",
                     cfg.AgenticPollEnabled ? 1 : 0, cfg.GitHubPat.empty() ? 1 : 0,
                     cfg.AgenticPollQuery.empty() ? 1 : 0);
            break;
        }

        auto* triage = GetAgenticTriageController();
        if (triage != nullptr && agentProposalStore_ != nullptr) {
            // Read the previous cursor BEFORE triaging so a per-iteration crash leaves the
            // cursor unmoved (the next poll re-fetches the same window). The cursor key
            // (sourceTracker, repoKey) — repo key is the user's query verbatim for source=github.
            std::int64_t prevCursor = 0;
            std::string cursorErr;
            agentProposalStore_->GetPollCursor("github", cfg.AgenticPollQuery, prevCursor, cursorErr);
            // Errors reading the cursor are non-fatal — treat as 0 (full first-page fetch).

            // Inject the cursor by routing the discovery through the adapter directly. The
            // controller's `TriageBatch` calls `ListOpenIssuesForRepo(owner, repo, keys, err)`
            // with the default sinceUnixSec=0 — to apply the cursor we'd need a controller-side
            // overload, but for T7 the pragmatic shape is to invoke the adapter manually for
            // discovery + then funnel each key through TriageIssue. Keeps the cursor filter
            // entirely in the worker, not threaded through the controller API.
            //
            // ParseOwnerRepoQuery duplicates the controller's strict parser; using the same
            // shape locally avoids re-doing the controller's strict checks indirectly.
            const auto slash = cfg.AgenticPollQuery.find('/');
            if (slash == std::string::npos || slash == 0 || slash + 1 >= cfg.AgenticPollQuery.size()) {
                LOG_WARN("AppController::AgenticPollWorkerLoop: malformed query '%s' — skipping iteration",
                         cfg.AgenticPollQuery.c_str());
            } else {
                const std::string owner = cfg.AgenticPollQuery.substr(0, slash);
                const std::string repo = cfg.AgenticPollQuery.substr(slash + 1);
                std::vector<std::string> keys;
                std::string listErr;
                if (agenticGithubReadAdapter_ &&
                    agenticGithubReadAdapter_->ListOpenIssuesForRepo(owner, repo, keys, listErr, prevCursor)) {
                    int totalInserted = 0;
                    int totalFailed = 0;
                    for (const auto& key : keys) {
                        int inserted = 0;
                        std::string triageErr;
                        if (triage->TriageIssue(key, inserted, triageErr)) {
                            totalInserted += inserted;
                        } else {
                            ++totalFailed;
                            LOG_WARN("AppController::AgenticPollWorkerLoop: triage %s failed: %s", key.c_str(),
                                     triageErr.c_str());
                        }
                    }
                    LOG_INFO(
                        "AppController::AgenticPollWorkerLoop: %s/%s scanned=%zu inserted=%d failed=%d (since=%lld)",
                        owner.c_str(), repo.c_str(), keys.size(), totalInserted, totalFailed,
                        static_cast<long long>(prevCursor));
                    // Bundle B CR#232:1653 — only advance the cursor when every issue in the
                    // iteration succeeded. If ANY per-issue triage failed (LLM timeout,
                    // GitHub 5xx, db error, ...) we must leave the cursor at the previous
                    // value so the next poll re-fetches those issues. Otherwise the cursor
                    // silently skips them and they vanish from the queue. Idempotency at
                    // the LLM + InsertMany layer keeps re-triage cheap.
                    if (AgenticInferenceClientPure::ShouldAdvancePollCursor(static_cast<std::size_t>(totalFailed))) {
                        const std::int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                                                        std::chrono::system_clock::now().time_since_epoch())
                                                        .count();
                        std::string setErr;
                        agentProposalStore_->SetPollCursor("github", cfg.AgenticPollQuery, nowSec, setErr);
                    } else {
                        LOG_WARN("AppController::AgenticPollWorkerLoop: %s/%s — %d per-issue failure(s); "
                                 "leaving cursor at %lld for retry on next poll",
                                 owner.c_str(), repo.c_str(), totalFailed, static_cast<long long>(prevCursor));
                    }
                } else {
                    LOG_WARN("AppController::AgenticPollWorkerLoop: ListOpenIssuesForRepo failed: %s", listErr.c_str());
                }
            }
        } else {
            LOG_WARN("AppController::AgenticPollWorkerLoop: triage controller unavailable — iteration skipped");
        }

        // Stamp the end-of-iteration time (for UI readout) before sleeping.
        const std::int64_t nowSec =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        agenticPollLastAtSec_.store(nowSec);

        // Sleep with condition-variable wake so a Stop / Restart from the UI thread doesn't
        // have to wait out the full interval. Re-read cfg each loop so a Preferences edit to
        // the interval applies on the next wake.
        std::unique_lock<std::mutex> lk(agenticPollMu_);
        const int intervalSec = (cfg.AgenticPollIntervalSec < 60)     ? 60
                                : (cfg.AgenticPollIntervalSec > 3600) ? 3600
                                                                      : cfg.AgenticPollIntervalSec;
        agenticPollCv_.wait_for(lk, std::chrono::seconds(intervalSec),
                                [this]() { return agenticPollShouldStop_.load(); });
    }
    agenticPollRunning_.store(false);
    LOG_INFO("AppController::AgenticPollWorkerLoop: exited");
}

void AppController::StartAgenticPollIfEnabled() {
    // (Bundle A) Caller holds `agenticPollLifecycleMutex_` (Restart / RestartAsync /
    // RunAgenticTriageOnce). Internal-only entry; do not call without the lock.
    if (agenticPollRunning_.load() || agenticPollThread_.joinable()) {
        LOG_DEBUG("AppController::StartAgenticPollIfEnabled: worker already running — no-op");
        return;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    if (!cfg.AgenticPollEnabled) {
        LOG_DEBUG("AppController::StartAgenticPollIfEnabled: master toggle off — not starting");
        return;
    }
    if (cfg.GitHubPat.empty()) {
        LOG_INFO("AppController::StartAgenticPollIfEnabled: GitHubPat empty — not starting (configure in Preferences)");
        return;
    }
    if (cfg.AgenticPollQuery.empty()) {
        LOG_INFO("AppController::StartAgenticPollIfEnabled: query empty — not starting (configure OWNER/REPO)");
        return;
    }
    if (cfg.AgenticPollSource != "github") {
        LOG_WARN("AppController::StartAgenticPollIfEnabled: only source=github supported in T7 (got '%s')",
                 cfg.AgenticPollSource.c_str());
        return;
    }
    agenticPollShouldStop_.store(false);
    agenticPollRunning_.store(true);
    agenticPollThread_ = std::thread(&AppController::AgenticPollWorkerLoop, this);
    LOG_INFO("AppController::StartAgenticPollIfEnabled: worker started (interval=%d s, query=%s)",
             cfg.AgenticPollIntervalSec, cfg.AgenticPollQuery.c_str());
}

void AppController::StopAgenticPoll() {
    // (Bundle A) Synchronous join — safe from non-UI threads (destructor, scenarios).
    // UI callers must route through `DetachAgenticPoll()` to avoid a multi-minute
    // freeze when a poll batch is mid-LLM-call.
    std::lock_guard<std::mutex> lifecycleLk(agenticPollLifecycleMutex_);
    if (!agenticPollThread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(agenticPollMu_);
        agenticPollShouldStop_.store(true);
    }
    agenticPollCv_.notify_all();
    agenticPollThread_.join();
    agenticPollRunning_.store(false);
    LOG_INFO("AppController::StopAgenticPoll: worker joined");
}

void AppController::RestartAgenticPoll() {
    // (Bundle A) Synchronous restart — safe from non-UI threads. Holds the lifecycle
    // lock across both phases so a UI toggle mid-restart can't race a duplicate
    // worker into existence.
    std::lock_guard<std::mutex> lifecycleLk(agenticPollLifecycleMutex_);
    if (agenticPollThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(agenticPollMu_);
            agenticPollShouldStop_.store(true);
        }
        agenticPollCv_.notify_all();
        agenticPollThread_.join();
        agenticPollRunning_.store(false);
    }
    // Drop the cached triage controller so the next start picks up a fresh GitHubPat /
    // base URL — Preferences may have changed both behind us. The next GetAgenticTriageController()
    // call from the worker rebuilds the chain (cheap — no HTTP traffic until TriageIssue runs).
    agenticTriageController_.reset();
    agenticGithubReadAdapter_.reset();
    agenticInferenceAdapter_.reset();
    agenticGithubClient_.reset();
    agenticInferenceClient_.reset();
    StartAgenticPollIfEnabled();
}

void AppController::DetachAgenticPoll() {
    // (Bundle A) UI-thread-safe shutdown: flip the stop atom + notify, then move the
    // (still-running) thread into the detached-handle slot so the dispatcher drain
    // can join it once it has actually exited. Joining a finished thread is
    // near-free — the cost the UI saves is the wait-for-LLM-timeout window.
    //
    // If a previous detach is already pending (worker still running), join it first
    // synchronously — at this point it is almost certainly finished (a long-detached
    // worker by definition observed the stop atom before the new detach call). In
    // the worst case this adds a few ms to the toggle response, vs the original
    // multi-minute freeze.
    std::lock_guard<std::mutex> lifecycleLk(agenticPollLifecycleMutex_);
    if (agenticPollDetachedThread_.joinable()) {
        agenticPollDetachedThread_.join();
    }
    if (!agenticPollThread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(agenticPollMu_);
        agenticPollShouldStop_.store(true);
    }
    agenticPollCv_.notify_all();
    // Hand the running thread off to the detached slot — `JoinDetachedAgenticPollIfReady`
    // (called every frame) joins it once `agenticPollRunning_` reads false.
    agenticPollDetachedThread_ = std::move(agenticPollThread_);
    LOG_INFO("AppController::DetachAgenticPoll: stop signalled, join deferred to dispatcher drain");
}

void AppController::RestartAgenticPollAsync() {
    // (Bundle A) UI-thread-safe restart: detaches the current worker (returns
    // immediately) then immediately starts a new one. The detached worker drains
    // off-band; the new one only blocks behind the lifecycle mutex which is held
    // exclusively here for the start phase.
    DetachAgenticPoll();
    std::lock_guard<std::mutex> lifecycleLk(agenticPollLifecycleMutex_);
    // Drop the cached triage controller chain — same rationale as RestartAgenticPoll.
    agenticTriageController_.reset();
    agenticGithubReadAdapter_.reset();
    agenticInferenceAdapter_.reset();
    agenticGithubClient_.reset();
    agenticInferenceClient_.reset();
    StartAgenticPollIfEnabled();
}

void AppController::JoinDetachedAgenticPollIfReady() {
    // (Bundle A) Per-frame helper — fast atomic load + branch. The lifecycle lock is
    // only taken when the detached thread is actually finished (i.e. running atom
    // false + thread joinable), so the steady-state cost is one atomic load.
    if (agenticPollRunning_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lifecycleLk(agenticPollLifecycleMutex_);
    if (agenticPollDetachedThread_.joinable() && !agenticPollRunning_.load(std::memory_order_acquire)) {
        agenticPollDetachedThread_.join();
        LOG_DEBUG("AppController::JoinDetachedAgenticPollIfReady: detached worker joined");
    }
}

bool AppController::RunAgenticTriageOnce(std::string& outError) {
    outError.clear();
    // (Bundle A) Hold the lifecycle lock for the duration of the controller fetch +
    // batch run so a UI checkbox toggle can't tear down the controller chain mid-batch.
    std::lock_guard<std::mutex> lifecycleLk(agenticPollLifecycleMutex_);
    auto* triage = GetAgenticTriageController();
    if (triage == nullptr) {
        outError = "Agentic triage unavailable — configure GitHub PAT in Preferences.";
        return false;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    if (cfg.AgenticPollQuery.empty()) {
        outError = "Agentic poll query empty — set OWNER/REPO in Preferences.";
        return false;
    }
    smatchet::agentic::AgenticTriageController::BatchResult result;
    std::string batchErr;
    if (!triage->TriageBatch("github", cfg.AgenticPollQuery, result, batchErr)) {
        outError = "TriageBatch failed: " + batchErr;
        return false;
    }
    LOG_INFO("AppController::RunAgenticTriageOnce: %s scanned=%d inserted=%d failed=%zu", cfg.AgenticPollQuery.c_str(),
             result.totalIssuesScanned, result.proposalsInserted, result.perIssueErrors.size());
    return true;
}

std::int64_t AppController::GetAgenticLastPollAtSec() const noexcept { return agenticPollLastAtSec_.load(); }

#endif // SMATCHET_WITH_AGENTIC

smatchet::cmd::CommandRegistry& AppController::Commands() {
    if (!commandRegistry_) {
        // Lazy fallback — caller invoked us before Initialize (tests, embedded hosts).
        commandRegistry_.reset(new smatchet::cmd::CommandRegistry());
    }
    return *commandRegistry_;
}

const smatchet::cmd::CommandRegistry& AppController::Commands() const {
    if (!commandRegistry_) {
        const_cast<AppController*>(this)->commandRegistry_.reset(new smatchet::cmd::CommandRegistry());
    }
    return *commandRegistry_;
}

smatchet::cmd::ScenarioRunner& AppController::Scenarios() {
    if (!scenarioRunner_) {
        scenarioRunner_.reset(new smatchet::cmd::ScenarioRunner());
    }
    return *scenarioRunner_;
}

const smatchet::cmd::ScenarioRunner& AppController::Scenarios() const {
    if (!scenarioRunner_) {
        const_cast<AppController*>(this)->scenarioRunner_.reset(new smatchet::cmd::ScenarioRunner());
    }
    return *scenarioRunner_;
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

            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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

    {
        auto cit = fieldIconAssetPathCache_.find(pathOrUrl);
        if (cit != fieldIconAssetPathCache_.end()) {
            return cit->second;
        }
    }

    const std::string t = TrimCopyAsciiWhitespace(pathOrUrl);

    if (t.empty()) {

        return std::string();
    }

    if (t.rfind("https://", 0) == 0 || t.rfind("http://", 0) == 0) {

        if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
            fieldIconAssetPathCache_.clear();
        }
        fieldIconAssetPathCache_.emplace(pathOrUrl, t);
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

        const std::string base = ConfigManager::GetRuntimeAssetDirectory();

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

        std::string out;
        if (!ec && isAllowedPath(absRel)) {
            out = absRel.string();
        }
        if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
            fieldIconAssetPathCache_.clear();
        }
        fieldIconAssetPathCache_.emplace(pathOrUrl, out);
        return out;
    }

    const fs::path abs = fs::weakly_canonical(inp, ec);

    std::string out;
    if (!ec && isAllowedPath(abs)) {
        out = abs.string();
    }
    if (fieldIconAssetPathCache_.size() >= kFieldIconAssetPathCacheCap) {
        fieldIconAssetPathCache_.clear();
    }
    fieldIconAssetPathCache_.emplace(pathOrUrl, out);
    return out;
}

std::string AppController::GetAutomationScriptContent() {

    std::string path = ResolveLuaScriptPath("Automation.lua");

    if (path.empty())
        return "";

    std::ifstream ifs(path);

    if (!ifs.is_open())
        return "";

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

std::string AppController::GetResolvedLocalCacheDbPath() const {
    if (localCacheDbPath_.empty()) {
        return {};
    }
    namespace fs = ghc::filesystem;
    std::error_code ec;
    fs::path p(localCacheDbPath_);
    if (!p.is_absolute()) {
        p = fs::absolute(p, ec);
        if (ec) {
            return localCacheDbPath_;
        }
    }
    return p.string();
}

bool AppController::RecreateLocalCacheDatabase(std::string& outError) {
    outError.clear();
    if (localCacheDbPath_.empty()) {
        outError = "Local cache database path is not set.";
        return false;
    }
    if (shuttingDown_.load()) {
        outError = "Application is shutting down.";
        return false;
    }

    // hasPendingSyncRequest_ was removed by the TicketSyncService Phase 1C extraction
    // (CODE_REVIEW item 11) — pending-sync state now lives on ticketSync_; the
    // cancel-and-join below covers what the flag used to gate.
    CancelAndJoinActiveStreamingSync();
    JoinBackgroundTasks();

    // Streaming-sync teardown lives entirely on TicketSyncService since Phase 1C of the item
    // 11 extraction: the cancel-and-join clears PendingBatches / BackgroundStaleIds /
    // FetchError / Warning / KeepIds; ResetStaleDeletionState clears the stale-delete
    // counters. Both are no-ops if the service was never `Initialize`d.
    if (ticketSync_) {
        ticketSync_->CancelAndJoinActiveStreamingSync();
        ticketSync_->ResetStaleDeletionState();
    }

    Cache.reset();

    std::string removeErr;
    if (!RemoveLocalCacheDbFiles(localCacheDbPath_, removeErr)) {
        try {
            Cache = std::make_unique<LocalCacheManager>(localCacheDbPath_);
        } catch (const std::exception& ex) {
            outError = removeErr + " Failed to reopen database: " + ex.what();
            return false;
        }
        outError = removeErr;
        return false;
    }

    try {
        Cache = std::unique_ptr<LocalCacheManager>(new LocalCacheManager(localCacheDbPath_));
    } catch (const std::exception& ex) {
        outError = std::string("Failed to open new database: ") + ex.what();
        return false;
    }

    try {
        (void)Cache->RunOneTimeLegacyDropPendingAtMaxAttempts();
    } catch (const std::exception& ex) {
        LOG_WARN("AppController::RecreateLocalCacheDatabase legacy cleanup: %s", ex.what());
    } catch (...) {
        LOG_WARN("AppController::RecreateLocalCacheDatabase legacy cleanup: unknown exception");
    }

    ClearLastTrackerTicketSyncWarning();
    if (offlineQueue_) {
        offlineQueue_->legacyPendingStartupBanner_.clear();
    }
    RefreshLocalData();
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

    pack.Tickets =
        Backend->FetchIssues(&pack.FullSyncCompleted, configOverride, viewsOverride, &pack.FetchError, &pack.Warning);

    return pack;
}

// ApplyIssueFetchPack / CancelAndJoinActiveStreamingSync: moved to TicketSyncService in
// Phase 1A of the item 11 extraction. Thin delegators below.

void AppController::ApplyIssueFetchPack(TrackerIssueFetchPack pack) {
    if (ticketSync_) {
        ticketSync_->ApplyIssueFetchPack(std::move(pack));
    }
}

void AppController::CancelAndJoinActiveStreamingSync() {
    if (ticketSync_) {
        ticketSync_->CancelAndJoinActiveStreamingSync();
    }
}

void AppController::TickStreamingApply() {
    if (ticketSync_) {
        ticketSync_->TickStreamingApply();
    }
}

void AppController::SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride) {
    if (ticketSync_) {
        ticketSync_->SyncWithBackend(configOverride, viewsOverride);
    }
}

bool AppController::IsStreamingSyncActive() const { return ticketSync_ && ticketSync_->IsActive(); }
