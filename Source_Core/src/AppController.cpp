#include "AppController.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include "JiraClient.h"
#include "JiraHttpUtils.h"
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

AppController::~AppController() {
    // Join UiDrawSession std::async futures that captured this controller (via static `g_ui`) before
    // tearing down members other threads may still touch.
    DrainUiDrawSessionFuturesBeforeAppTeardown(*this);
    shuttingDown_.store(true);
    DrainJiraConnectivityProbeFuture();
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
        JiraClient client;
        JiraConfig cfg = ConfigManager::Load();
        ViewsStore views = ConfigManager::LoadViewsOrBootstrap(cfg);
        std::string err;
        std::vector<CachedTicket> tickets;
        const bool ok = client.FetchIssuesForKeys(cfg, toFetch, views, tickets, err);
        {
            std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
            for (const auto& k : toFetch) {
                bulkImportPrefetchKeysInFlight_.erase(k);
            }
        }
        if (!ok) {
            if (IsJiraTransportErrorText(err)) {
                LOG_INFO("AppController::PrefetchIssueTicketsForKeys skipped (transport): %s", err.c_str());
            } else {
                LOG_WARN("AppController::PrefetchIssueTicketsForKeys failed: %s", err.c_str());
            }
            return;
        }
        requestDeferredLiveJiraBackendSuccessNotify_();
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

    if (backendType == "Jira") {
        Backend = std::unique_ptr<ITrackerClient>(new JiraClient());
        JiraBackend = dynamic_cast<JiraClient*>(Backend.get());
        LOG_INFO("AppController: Jira backend initialized.");
    } else {
        LOG_WARN("AppController: unsupported backendType=%s; backend disabled.", backendType.c_str());
    }

    const std::string& fileBase = ConfigManager::GetFilesBaseDirectory();
    if (!fileBase.empty()) {
        luaScriptsDirectory_ = fileBase + "Scripts/";
    } else {
        luaScriptsDirectory_.clear();
    }

    // Defer SyncWithBackend to first SmatchetUI::Draw so active view JQL/fields are
    // applied first — avoids fetching issues twice at startup.
    RefreshLocalData();

    if (JiraBackend) {
        std::vector<TrackerField> snapFields;
        std::vector<TrackerComponent> snapComponents;
        std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta;
        std::string snapErr;
        if (FieldCatalogCache::TryLoadFieldCatalogSnapshot(snapFields, snapComponents, snapIssueTypeMeta, snapErr)) {
            AvailableFields = std::move(snapFields);
            AvailableComponents = std::move(snapComponents);
            AvailableIssueTypeMeta = std::move(snapIssueTypeMeta);
            fieldCatalogEverLoaded_ = true;
            LastJiraFieldCatalogError.clear();
            LastJiraFieldCatalogWarning =
                "Working offline: Jira field catalog loaded from local snapshot until a live refresh "
                "succeeds.";
            for (auto& field : AvailableFields) {
                if (field.Id == "comment" || field.Id == "timespent" || field.Id == "aggregatetimeoriginalestimate" ||
                    field.Id == "aggregatetimeestimate" || field.Id == "aggregatetimespent") {
                    field.ReadOnly = true;
                }
            }
            EnsureCatalogHistoryField();
            JiraFieldCatalogRevision.fetch_add(1);
            LOG_INFO("AppController::Initialize: restored field catalog from snapshot (%zu fields)",
                     AvailableFields.size());
        }
    }

    WarmIssueTypeEditMetaAtStartAsync();

    InitLua();
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

void AppController::SyncWithBackend(const JiraConfig* configOverride, const ViewsStore* viewsOverride) {
    LOG_INFO("AppController::SyncWithBackend started.");
    LastJiraTicketSyncWarning.clear();
    if (Backend && Cache) {
        bool fullSyncCompleted = false;
        std::string fetchError;
        auto freshTickets = Backend->FetchIssues(&fullSyncCompleted, configOverride, viewsOverride, &fetchError);
        if (!fetchError.empty() && IsJiraTransportErrorText(fetchError)) {
            LastJiraTicketSyncWarning = "Showing cached issues — live refresh did not complete: " + fetchError;
            LOG_WARN("AppController::SyncWithBackend transport-style fetch issue: %s", fetchError.c_str());
            lastJiraConnectivityState_ = JiraConnectivityState::TransportDown;
            const auto nowProbe = std::chrono::steady_clock::now();
            nextJiraConnectivityProbeAt_ = nowProbe;
            PushOfflineReplayTimersDuringTransportOutage(nowProbe);
        } else if (fetchError.empty()) {
            requestDeferredLiveJiraBackendSuccessNotify_();
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
        LOG_INFO("AppController::SyncWithBackend finished fetched=%zu saved=%zu deleted=%zu fullSync=%d",
                 freshTickets.size(), saved, deleted, fullSyncCompleted ? 1 : 0);
    } else {
        LOG_WARN("AppController::SyncWithBackend skipped: backend=%d cache=%d", Backend ? 1 : 0, Cache ? 1 : 0);
    }
    RefreshLocalData();
    WarmIssueTypeEditMetaAtStartAsync();
}
