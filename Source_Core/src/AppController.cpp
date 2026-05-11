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

#include "FieldCatalogCache.h"



#include <ghc/filesystem.hpp>

#include "DefaultTrackerBackendFactory.h"

#include "ITrackerBackendFactory.h"

#include "OfflineQueueService.h"

#include "TrackerHttpUtils.h"

#include "Logger.h"

#include "StringUtil.h"

#include "Views.h"



#include "SmatchetUI.h"

#include "SmatchetToast.h"



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
    while (!text.empty() &&
           (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) {
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
        if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
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

void AppController::SetRequestAppQuitHandler(std::function<void()> handler) { RequestAppQuitHandler = std::move(handler); }

void AppController::RequestAppQuit() const {

    if (RequestAppQuitHandler) {

        RequestAppQuitHandler();

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

void AppController::NotifyMcpClientHttpActivity() {

    const auto now = std::chrono::steady_clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    mcpLastClientHttpActivityNs_.store(static_cast<std::uint64_t>(ns), std::memory_order_release);

}

bool AppController::TryGetMcpLastClientHttpActivity(std::chrono::steady_clock::time_point* out) const {

    const std::uint64_t raw = mcpLastClientHttpActivityNs_.load(std::memory_order_acquire);

    if (raw == 0 || out == nullptr) {

        return false;

    }

    *out = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(raw)));

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
    cpr::Header headers{{"Accept", "application/vnd.github+json"}, {"User-Agent", "SmatchetUpdater/" + out.CurrentVersion}};
    cpr::Response response = cpr::Get(cpr::Url{url}, headers, cpr::Redirect{true, true},
                                      cpr::ConnectTimeout{5000}, cpr::Timeout{15000});
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
                                                     std::string& outError) const {
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
    cpr::WriteCallback writeCb{[&](const std::string& data, intptr_t) -> bool {
        ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        return ofs.good();
    }};
    cpr::Response resp = cpr::Get(cpr::Url{downloadUrl}, headers, redirect, writeCb,
                                  cpr::ConnectTimeout{5000}, cpr::Timeout{120000});
    ofs.close();
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
    outError = "Installer updates are currently supported only on Windows.";
    return false;
#endif
}



void AppController::Initialize(const std::string& dbPath, const std::string& backendType) {

    LOG_INFO("AppController::Initialize backendType=%s dbPath=%s", backendType.c_str(), dbPath.c_str());

    localCacheDbPath_ = dbPath;

    Cache = std::make_unique<LocalCacheManager>(dbPath);

    // Construct OfflineQueueService eagerly so the legacy-pending startup migration below
    // can write to `offlineQueue_->legacyPendingStartupBanner_` (item 12 extraction).
    if (!offlineQueue_) {
        offlineQueue_ = std::make_unique<OfflineQueueService>(*this);
    }

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

            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {

                return static_cast<char>(std::tolower(c));

            });

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

    hasPendingSyncRequest_ = false;
    CancelAndJoinActiveStreamingSync();
    JoinBackgroundTasks();

    {
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        activeStreamingSync_.PendingBatches.clear();
        activeStreamingSync_.BackgroundStaleIds.clear();
    }
    staleIdsToDelete_.clear();
    isDeletingStale_.store(false);
    totalStaleToDelete_ = 0;
    staleDeletedSoFar_ = 0;
    activeStreamingSync_.FullSyncCompleted = false;
    activeStreamingSync_.TotalFetchedCount = 0;
    {
        // FetchError contract: every read/write through QueueMutex (worker has been joined here,
        // so the lock is uncontended — but keeping the lock guards the contract from future drift).
        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
        activeStreamingSync_.FetchError.clear();
        activeStreamingSync_.Warning.clear();
    }
    activeStreamingSync_.KeepIds.clear();
    activeStreamingSync_.Cancelled = false;
    activeStreamingSync_.Superseded = false;

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

    pack.Tickets = Backend->FetchIssues(&pack.FullSyncCompleted, configOverride, viewsOverride, &pack.FetchError,
                                        &pack.Warning);

    return pack;

}



void AppController::ApplyIssueFetchPack(TrackerIssueFetchPack pack) {

    if (!Backend || !Cache) {

        LOG_WARN("AppController::ApplyIssueFetchPack skipped: backend=%d cache=%d", Backend ? 1 : 0, Cache ? 1 : 0);

        return;

    }

    LastTrackerTicketSyncWarning.clear();

    const std::vector<CachedTicket>& freshTickets = pack.Tickets;

    const std::string& fetchError = pack.FetchError;
    const std::string& fetchWarning = pack.Warning;

    const bool fullSyncCompleted = pack.FullSyncCompleted;



    if (!fetchError.empty() && IsTrackerTransportErrorText(fetchError)) {

        LastTrackerTicketSyncWarning = "Showing cached issues — live refresh did not complete: " + fetchError;

        LOG_WARN("AppController::ApplyIssueFetchPack transport-style fetch issue: %s", fetchError.c_str());

        lastTrackerConnectivityState_ = TrackerConnectivityState::TransportDown;

        const auto nowProbe = std::chrono::steady_clock::now();

        nextTrackerConnectivityProbeAt_ = nowProbe;

        PushOfflineReplayTimersDuringTransportOutage(nowProbe);

    } else if (fetchError.empty()) {

        // Soft warnings still count as success — the fetched data is valid, just partial.
        // Surface the caveat as a sync warning banner and still fire the success notify so
        // the connectivity state isn't pinned to TransportDown.
        if (!fetchWarning.empty()) {
            LastTrackerTicketSyncWarning = "Sync completed with a caveat: " + fetchWarning;
            LOG_WARN("AppController::ApplyIssueFetchPack soft warning: %s", fetchWarning.c_str());
        }
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



void AppController::CancelAndJoinActiveStreamingSync() {

    activeStreamingSync_.Cancelled = true;
    activeStreamingSync_.Superseded = true;

    if (activeStreamingSync_.WorkerThread.joinable()) {

        activeStreamingSync_.WorkerThread.join();

    }

    activeStreamingSync_.Active = false;

    activeStreamingSync_.ActiveSessionRunning = false;

    {

        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

        activeStreamingSync_.PendingBatches.clear();
        activeStreamingSync_.BackgroundStaleIds.clear();

    }

}



void AppController::TickStreamingApply() {

    // 1. If we have a pending sync request and the previous session (worker + apply queue + stale cleanup) is fully drained, start it safely now.

    bool isWorkerActive = activeStreamingSync_.Active.load();

    bool isSessionBusy = isWorkerActive || activeStreamingSync_.ActiveSessionRunning || isDeletingStale_.load();

    if (activeStreamingSync_.Superseded.load()) {

        {

            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

            activeStreamingSync_.PendingBatches.clear();

            activeStreamingSync_.BackgroundStaleIds.clear();

        }

        staleIdsToDelete_.clear();

        isDeletingStale_.store(false);

        totalStaleToDelete_ = 0;

        staleDeletedSoFar_ = 0;

        if (isWorkerActive) {

            return;

        }

        if (activeStreamingSync_.WorkerThread.joinable()) {

            activeStreamingSync_.WorkerThread.join();

        }

        activeStreamingSync_.Active = false;

        activeStreamingSync_.ActiveSessionRunning = false;

        activeStreamingSync_.FullSyncCompleted = false;

        activeStreamingSync_.TotalFetchedCount = 0;

        {
            // FetchError contract: every read/write through QueueMutex (worker joined above).
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            activeStreamingSync_.FetchError.clear();
        activeStreamingSync_.Warning.clear();
        }

        activeStreamingSync_.KeepIds.clear();

        activeStreamingSync_.Cancelled = false;

        activeStreamingSync_.Superseded = false;

        LOG_INFO("AppController::TickStreamingApply discarded superseded streaming sync session.");

        if (hasPendingSyncRequest_) {

            hasPendingSyncRequest_ = false;

            StartStreamingSync(pendingConfig_, pendingViews_);

        }

        return;

    }



    if (!isSessionBusy) {

        if (activeStreamingSync_.WorkerThread.joinable()) {

            activeStreamingSync_.WorkerThread.join();

        }

        if (hasPendingSyncRequest_) {

            hasPendingSyncRequest_ = false;

            StartStreamingSync(pendingConfig_, pendingViews_);

            return;

        }

    }



    // 2. Progressive, budgeted stale ticket deletion over multiple frames to avoid UI hitches

    if (isDeletingStale_.load()) {

        auto start = std::chrono::high_resolution_clock::now();

        size_t deletedThisFrame = 0;

        bool inMemoryChanged = false;

        while (!staleIdsToDelete_.empty()) {

            std::string id = std::move(staleIdsToDelete_.back());

            staleIdsToDelete_.pop_back();

            if (Cache) {

                Cache->DeleteTicket(id);

            }

            {

                std::lock_guard<std::mutex> lock(activeTicketsMutex_);

                ActiveTickets.erase(std::remove_if(ActiveTickets.begin(), ActiveTickets.end(),

                                                   [&](const CachedTicket& t) { return t.id == id; }),

                                     ActiveTickets.end());

            }

            inMemoryChanged = true;

            deletedThisFrame++;

            staleDeletedSoFar_++;



            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();

            if (elapsed >= 3 || deletedThisFrame >= 10) {

                break;

            }

        }

        if (inMemoryChanged) {

            std::lock_guard<std::mutex> lock(activeTicketsMutex_);

            activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ActiveTickets);

            ActiveTicketsRevision.fetch_add(1);

        }

        if (staleIdsToDelete_.empty()) {

            isDeletingStale_.store(false);

            LOG_INFO("AppController::TickStreamingApply finished stale deletion. total_deleted=%zu", totalStaleToDelete_);

            // Trigger editmeta warmup after cleanup completes

            WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());

        }

        return;

    }



    if (!activeStreamingSync_.ActiveSessionRunning) {

        return;

    }



    // 3. Process incoming streaming ticket batches in-memory and write to SQLite with budget

    auto start = std::chrono::high_resolution_clock::now();

    size_t ticketsProcessedInFrame = 0;

    bool stateChanged = false;

    std::vector<CachedTicket> batchToProcess;

    std::vector<CachedTicket> processedThisFrame;



    while (true) {

        batchToProcess.clear();

        {

            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

            if (activeStreamingSync_.PendingBatches.empty()) {

                break;

            }

            auto& frontBatch = activeStreamingSync_.PendingBatches.front();

            if (frontBatch.empty()) {

                activeStreamingSync_.PendingBatches.erase(activeStreamingSync_.PendingBatches.begin());

                continue;

            }



            size_t sliceSize = std::min(frontBatch.size(), size_t(20 - ticketsProcessedInFrame));

            if (sliceSize == 0) {

                break; // Frame limit

            }



            batchToProcess.insert(batchToProcess.end(),

                                  std::make_move_iterator(frontBatch.begin()),

                                  std::make_move_iterator(frontBatch.begin() + sliceSize));

            frontBatch.erase(frontBatch.begin(), frontBatch.begin() + sliceSize);



            if (frontBatch.empty()) {

                activeStreamingSync_.PendingBatches.erase(activeStreamingSync_.PendingBatches.begin());

            }

        }



        for (const auto& t : batchToProcess) {

            if (Cache) {

                Cache->SaveTicket(t);

            }

            if (!t.id.empty()) {

                activeStreamingSync_.KeepIds.insert(t.id);

            }

            ticketsProcessedInFrame++;

        }

        processedThisFrame.insert(processedThisFrame.end(), std::make_move_iterator(batchToProcess.begin()),

                                  std::make_move_iterator(batchToProcess.end()));

        stateChanged = true;



        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();

        if (elapsed >= 3 || ticketsProcessedInFrame >= 20) {

            break;

        }

    }



    if (stateChanged) {

        {

            std::lock_guard<std::mutex> lock(activeTicketsMutex_);

            for (const auto& t : processedThisFrame) {

                auto it = std::find_if(ActiveTickets.begin(), ActiveTickets.end(),

                                       [&](const CachedTicket& existing) { return existing.id == t.id; });

                if (it != ActiveTickets.end()) {

                    *it = t;

                } else {

                    ActiveTickets.push_back(t);

                }

            }

            activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ActiveTickets);

        }

        PruneEditMetaCacheToActiveTickets();

        ActiveTicketsRevision.fetch_add(1);

    }



    bool isWorkerFinished = !activeStreamingSync_.Active.load();

    bool hasPending = false;

    {

        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

        hasPending = !activeStreamingSync_.PendingBatches.empty();

    }



    if (isWorkerFinished && !hasPending && activeStreamingSync_.ActiveSessionRunning) {

        activeStreamingSync_.ActiveSessionRunning = false;



        // FetchError + Warning are written by the worker thread under QueueMutex; acquire it
        // for the read. FullSyncCompleted is atomic and can be read without the lock.
        std::string fetchError;
        std::string fetchWarning;
        {
            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);
            fetchError = activeStreamingSync_.FetchError;
            fetchWarning = activeStreamingSync_.Warning;
        }

        bool fullSyncCompleted = activeStreamingSync_.FullSyncCompleted;



        if (!fetchError.empty() && IsTrackerTransportErrorText(fetchError)) {

            LastTrackerTicketSyncWarning = "Showing cached issues — live refresh did not complete: " + fetchError;

            LOG_WARN("AppController::TickStreamingApply transport-style fetch issue: %s", fetchError.c_str());

            lastTrackerConnectivityState_ = TrackerConnectivityState::TransportDown;

            const auto nowProbe = std::chrono::steady_clock::now();

            nextTrackerConnectivityProbeAt_ = nowProbe;

            PushOfflineReplayTimersDuringTransportOutage(nowProbe);

            SmatchetToastManager::Instance().Push("Sync Failed", fetchError, ToastType::Error, 5000);

        } else if (!fetchError.empty()) {

            SmatchetToastManager::Instance().Push("Sync Warning", fetchError, ToastType::Warning, 5000);

        } else {

            // Soft warnings: data is good, just partial — still notify success but surface
            // the caveat as a warning banner + toast.
            if (!fetchWarning.empty()) {
                LastTrackerTicketSyncWarning = "Sync completed with a caveat: " + fetchWarning;
                LOG_WARN("AppController::TickStreamingApply soft warning: %s", fetchWarning.c_str());
                SmatchetToastManager::Instance().Push("Sync Warning", fetchWarning, ToastType::Warning, 5000);
            }
            requestDeferredLiveTrackerBackendSuccessNotify_();

            std::string msg = "Synchronized " + std::to_string(activeStreamingSync_.KeepIds.size()) + " issues successfully.";

            SmatchetToastManager::Instance().Push("Sync Complete", msg, ToastType::Success, 4000);

        }



        totalStaleToDelete_ = 0;

        staleDeletedSoFar_ = 0;

        staleIdsToDelete_.clear();

        if (fullSyncCompleted) {

            std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

            staleIdsToDelete_ = std::move(activeStreamingSync_.BackgroundStaleIds);

            if (!staleIdsToDelete_.empty()) {

                isDeletingStale_.store(true);

                totalStaleToDelete_ = staleIdsToDelete_.size();

                staleDeletedSoFar_ = 0;

            }

        }



        LOG_INFO("AppController::TickStreamingApply finished sync session. saved_or_kept=%zu total_stale=%zu fullSync=%d err=%s",

                 activeStreamingSync_.KeepIds.size(), totalStaleToDelete_, fullSyncCompleted ? 1 : 0, fetchError.c_str());



        if (!isDeletingStale_.load()) {

            WarmIssueTypeEditMetaAtStartAsync(ConfigManager::Load());

        }

    }

}



void AppController::SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride) {

    LOG_INFO("AppController::SyncWithBackend started (asynchronous streaming refresh).");

    TrackerConfig cfgCopy;

    if (configOverride) {

        cfgCopy = *configOverride;

    } else {

        cfgCopy = ConfigManager::Load();

    }

    ViewsStore viewsCopy;

    if (viewsOverride) {

        viewsCopy = *viewsOverride;

    } else {

        viewsCopy = ConfigManager::LoadViewsOrBootstrap(cfgCopy);

    }



    bool isWorkerActive = activeStreamingSync_.Active.load();

    bool isSessionBusy = isWorkerActive || activeStreamingSync_.ActiveSessionRunning || isDeletingStale_.load();



    if (isSessionBusy) {

        activeStreamingSync_.Cancelled = true;

        activeStreamingSync_.Superseded = true;

        hasPendingSyncRequest_ = true;

        pendingConfig_ = cfgCopy;

        pendingViews_ = viewsCopy;

        LOG_INFO("AppController: Active sync/apply/cleanup session busy. New sync request deferred to avoid UI thread block.");

        return;

    }

    StartStreamingSync(cfgCopy, viewsCopy);

}



void AppController::StartStreamingSync(const TrackerConfig& cfgCopy, const ViewsStore& viewsCopy) {

    if (activeStreamingSync_.WorkerThread.joinable()) {

        activeStreamingSync_.WorkerThread.join();

    }

    SmatchetToastManager::Instance().Push("Syncing", "Refreshing issues from Tracker...", ToastType::Info, 2500);

    // Swapping Backend type safely before starting worker

    std::string newTracker = cfgCopy.TrackerType;

    if (newTracker.empty()) newTracker = "Jira";



    const std::string trackerLower = ToLowerAsciiCopy(newTracker);



    const std::string currentType = Backend ? Backend->GetTrackerType() : "";

    const bool isCurrentlyJira  = (currentType == "Jira");

    const bool isCurrentlyPlane = (currentType == "Plane");



    if (trackerLower == "plane" && !isCurrentlyPlane) {

        Backend = backendFactory_->Create("Plane");

        LOG_INFO("AppController: Switched backend to Plane.");

    } else if (trackerLower == "jira" && !isCurrentlyJira) {

        Backend = backendFactory_->Create("Jira");

        LOG_INFO("AppController: Switched backend to Jira.");

    }



    uint64_t reqId = ++currentFetchRequestId_;

    activeStreamingSync_.RequestId = reqId;

    activeStreamingSync_.Cancelled = false;

    activeStreamingSync_.Superseded = false;

    activeStreamingSync_.Active = true;

    activeStreamingSync_.ActiveSessionRunning = true;

    activeStreamingSync_.TotalFetchedCount = 0;

    activeStreamingSync_.FullSyncCompleted = false;

    activeStreamingSync_.KeepIds.clear();

    {

        // FetchError contract: every read/write through QueueMutex. Worker for the new request has
        // not been spawned yet, so the lock is uncontended.

        std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

        activeStreamingSync_.FetchError.clear();
        activeStreamingSync_.Warning.clear();

        activeStreamingSync_.PendingBatches.clear();

        activeStreamingSync_.BackgroundStaleIds.clear();

    }



    LOG_INFO("AppController: Spawning background worker for async streaming fetch request ID=%llu",

             static_cast<unsigned long long>(reqId));



    activeStreamingSync_.WorkerThread = std::thread([this, reqId, cfgCopy, viewsCopy]() {

        try {

            std::unordered_set<std::string> workerKeepIds;

            auto onBatch = [this, reqId, &workerKeepIds](std::vector<CachedTicket>&& batch) {

                for (const auto& ticket : batch) {

                    if (!ticket.id.empty()) {

                        workerKeepIds.insert(ticket.id);

                    }

                }

                std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

                if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {

                    activeStreamingSync_.PendingBatches.push_back(std::move(batch));

                }

            };

            auto shouldCancel = [this, reqId]() -> bool {

                return activeStreamingSync_.Cancelled || activeStreamingSync_.RequestId != reqId;

            };



            TrackerIssueFetchSummary summary = Backend->FetchIssuesStreamed(onBatch, shouldCancel, &cfgCopy, &viewsCopy);



            if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {

                // FullSyncCompleted is atomic; FetchError is not and must be written under
                // QueueMutex so TickStreamingApply on the UI thread reads a consistent value.
                std::vector<std::string> localStaleIds;

                if (summary.FullSyncCompleted && Cache) {

                    std::vector<std::string> existingIds = Cache->GetAllTicketIds();

                    std::copy_if(existingIds.begin(), existingIds.end(), std::back_inserter(localStaleIds),

                                 [&workerKeepIds](const std::string& id) {

                                     return workerKeepIds.find(id) == workerKeepIds.end();

                                 });

                }

                std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

                activeStreamingSync_.FullSyncCompleted = summary.FullSyncCompleted;

                activeStreamingSync_.FetchError = summary.FetchError;
                activeStreamingSync_.Warning = summary.Warning;

                activeStreamingSync_.TotalFetchedCount = summary.FetchedCount;

                if (summary.FullSyncCompleted && Cache) {

                    activeStreamingSync_.BackgroundStaleIds = std::move(localStaleIds);

                }

            }

        } catch (const std::exception& ex) {

            LOG_ERROR("AppController: Worker thread caught exception: %s", ex.what());

            if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {

                std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

                activeStreamingSync_.FetchError = std::string("Sync failed with exception: ") + ex.what();

                activeStreamingSync_.FullSyncCompleted = false;

            }

        } catch (...) {

            LOG_ERROR("AppController: Worker thread caught unknown exception.");

            if (activeStreamingSync_.RequestId == reqId && !activeStreamingSync_.Cancelled) {

                std::lock_guard<std::mutex> qLock(activeStreamingSync_.QueueMutex);

                activeStreamingSync_.FetchError = "Sync failed with unknown exception.";

                activeStreamingSync_.FullSyncCompleted = false;

            }

        }

        activeStreamingSync_.Active = false;

    });

}
