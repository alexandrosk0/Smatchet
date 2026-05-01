#include "AppController.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <unordered_set>
#include <exception>
#include <mutex>
#include <thread>
#include <algorithm>

#include <nlohmann/json.hpp>

#include "AttachmentMimeUtils.h"
#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "FieldCatalogCache.h"
#include "JiraClient.h"
#include "JiraHttpUtils.h"
#include "JiraTrackerFieldAdapter.h"
#include "Logger.h"
#include "StringUtil.h"
#include "CompactDateFormat.h"
#include "JiraFieldPayload.h"
#include "SmatchetUI.h"

#include <cpr/cpr.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

namespace {
std::string MakeUniqueTempFilePath(const std::string& filename, const std::string& extension);

std::string NormalizeDomain(const std::string& rawDomain) {
    std::string value = TrimCopyAsciiWhitespace(rawDomain);
    const size_t schemeSep = value.find("://");
    if (schemeSep != std::string::npos) {
        value = value.substr(schemeSep + 3);
    }
    const size_t slashPos = value.find('/');
    if (slashPos != std::string::npos) {
        value = value.substr(0, slashPos);
    }
    const size_t atPos = value.rfind('@');
    if (atPos != std::string::npos) {
        value = value.substr(atPos + 1);
    }
    const size_t colonPos = value.find(':');
    if (colonPos != std::string::npos) {
        value = value.substr(0, colonPos);
    }
    return ToLowerAsciiCopy(value);
}

std::string ExtractHostFromUrl(const std::string& url) {
    const size_t schemeSep = url.find("://");
    if (schemeSep == std::string::npos) {
        return std::string();
    }
    const size_t hostStart = schemeSep + 3;
    const size_t hostEnd = url.find_first_of("/?#", hostStart);
    const std::string hostPort = url.substr(hostStart, hostEnd - hostStart);
    if (hostPort.empty()) {
        return std::string();
    }
    if (hostPort.front() == '[') {
        const size_t closePos = hostPort.find(']');
        if (closePos == std::string::npos) {
            return std::string();
        }
        return ToLowerAsciiCopy(hostPort.substr(1, closePos - 1));
    }
    const size_t colonPos = hostPort.find(':');
    return ToLowerAsciiCopy(colonPos == std::string::npos ? hostPort : hostPort.substr(0, colonPos));
}

bool IsAllowedJiraAttachmentHost(const std::string& host, const std::string& jiraDomain) {
    if (host.empty() || jiraDomain.empty()) {
        return false;
    }
    if (host == jiraDomain) {
        return true;
    }
    const std::string suffix = "." + jiraDomain;
    if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return true;
    }
    return host == "api.media.atlassian.com";
}

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

bool DownloadAttachmentToLocalFile(const std::string& url, const std::string& filename, const std::string& mimeType,
                                   std::string& outFilePath, std::string& outMime, std::string& outError) {
    outFilePath.clear();
    outMime.clear();
    outError.clear();
    if (url.empty()) {
        outError = "Attachment URL is empty.";
        return false;
    }

    JiraConfig cfg = ConfigManager::Load();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira credentials/domain.";
        return false;
    }
    if (url.rfind("https://", 0) != 0) {
        outError = "Attachment URL must use HTTPS.";
        return false;
    }
    const std::string jiraDomain = NormalizeDomain(cfg.Domain);
    const std::string targetHost = ExtractHostFromUrl(url);
    if (!IsAllowedJiraAttachmentHost(targetHost, jiraDomain)) {
        outError = "Attachment host is not allowlisted.";
        return false;
    }

    cpr::Header headers{{"Accept", "*/*"},
                        {"Authorization", BuildJiraBasicAuthHeader(cfg)},
                        {"User-Agent", "Smatchet/1.0 Attachment-Downloader"}};
    cpr::Redirect redirect(true, false);

    constexpr size_t kMaxAttachmentDownloadBytes = 50u * 1024u * 1024u;
    bool sizeExceeded = false;
    std::string bodyAccum;
    bodyAccum.reserve(64 * 1024);
    cpr::WriteCallback writeCb{[&](std::string data, intptr_t) -> bool {
        if (bodyAccum.size() + data.size() > kMaxAttachmentDownloadBytes) {
            sizeExceeded = true;
            return false;
        }
        bodyAccum.append(data);
        return true;
    }};
    const auto resp =
        cpr::Get(cpr::Url{url}, headers, redirect, writeCb, cpr::ConnectTimeout{5000}, cpr::Timeout{120000});
    if (sizeExceeded) {
        outError = "Attachment exceeds max allowed size.";
        return false;
    }
    if (resp.error.code != cpr::ErrorCode::OK || resp.status_code < 200 || resp.status_code >= 300) {
        outError = "Download failed: HTTP " + std::to_string(static_cast<int>(resp.status_code));
        return false;
    }

    outMime = mimeType;
    try {
        auto it = resp.header.find("Content-Type");
        if (it != resp.header.end() && !it->second.empty()) {
            outMime = it->second;
        }
    } catch (...) {
        LOG_DEBUG("DownloadAttachmentToLocalFile: response header parse failed; using provided mime type.");
    }
    if (outMime.empty()) {
        outMime = "application/octet-stream";
    }

    const std::string extension = ExtensionFromMime(outMime);
    outFilePath = MakeUniqueTempFilePath(filename, extension);
    std::ofstream ofs(outFilePath, std::ios::binary);
    if (!ofs.is_open()) {
        outError = "Failed to open local temp file.";
        outFilePath.clear();
        return false;
    }

    ofs.write(bodyAccum.data(), static_cast<std::streamsize>(bodyAccum.size()));
    if (!ofs.good()) {
        outError = "Failed to write downloaded attachment bytes.";
        ofs.close();
        outFilePath.clear();
        return false;
    }
    ofs.close();
    LOG_INFO("DownloadAttachmentToLocalFile: downloaded %zu bytes mime=%s path=%s", bodyAccum.size(), outMime.c_str(),
             outFilePath.c_str());
    return true;
}

std::string GetTempDir() {
#if defined(_WIN32) && defined(_MSC_VER)
    // MSVC: getenv is deprecated (C4996); _dupenv_s is not linked the same way on MinGW.
    auto readEnv = [](const char* name) -> std::string {
        char* buf = nullptr;
        size_t sz = 0;
        if (_dupenv_s(&buf, &sz, name) != 0) {
            if (buf) {
                std::free(buf);
            }
            return {};
        }
        if (!buf || buf[0] == '\0') {
            if (buf) {
                std::free(buf);
            }
            return {};
        }
        std::string out(buf);
        std::free(buf);
        return out;
    };
    std::string t = readEnv("TEMP");
    if (!t.empty()) {
        return t;
    }
    t = readEnv("TMP");
    if (!t.empty()) {
        return t;
    }
    return ".";
#else
    const char* env = std::getenv("TEMP");
    if (env && *env)
        return std::string(env);
    env = std::getenv("TMP");
    if (env && *env)
        return std::string(env);
    return ".";
#endif
}

std::string SanitizeFilename(const std::string& name) {
    std::string out = name;
    for (auto& ch : out) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '\"' || ch == '<' || ch == '>' ||
            ch == '|') {
            ch = '_';
        }
    }
    if (out.empty())
        return std::string("attachment");
    return out;
}

std::string MakeUniqueTempFilePath(const std::string& filename, const std::string& extension) {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const std::string safe = SanitizeFilename(filename);
    std::string base = safe;

    // If caller didn't include an extension, append our derived one.
    if (base.find_last_of('.') == std::string::npos) {
        base += extension;
    }

    const std::string tempDir = GetTempDir();
    if (tempDir.empty())
        return base;

    // Ensure tempDir ends with a path separator.
    if (tempDir.back() != '/' && tempDir.back() != '\\') {
        return tempDir + "/" + std::to_string(now) + "_" + base;
    }
    return tempDir + std::to_string(now) + "_" + base;
}

std::string SanitizeOfflineQueueDetail(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
        if (c == '=') {
            c = ':';
        }
    }
    return s;
}

std::string FormatOfflineQueueTerminalLine(const char* action, const char* stage, const char* reason,
                                           std::string detail) {
    detail = SanitizeOfflineQueueDetail(std::move(detail));
    std::string out;
    out.reserve(64u + detail.size());
    out += "action=";
    out += action;
    out += " stage=";
    out += stage;
    out += " reason=";
    out += reason;
    out += " detail=";
    out += detail;
    return out;
}

#if !defined(_WIN32)
std::string EscapeShellArg(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '"' || c == '\\' || c == '`' || c == '$') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
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

void AppController::ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments) {
    if (attachments.empty()) {
        return;
    }
    if (AttachmentCollectionHandlerCallback) {
        AttachmentCollectionHandlerCallback(attachments);
        return;
    }

    const AttachmentDescriptor& first = attachments.front();
    if (!first.Url.empty()) {
        OpenAttachment(first.Url, first.Filename, first.MimeType);
    }
}

void AppController::OpenAttachment(const std::string& url, const std::string& filename, const std::string& mimeType) {
    if (url.empty()) {
        return;
    }

    // If no file-based path exists, fall back to regular URL opening.
    if (!AttachmentViewerHandlerCallback && !AttachmentPreviewHandlerCallback) {
        LOG_INFO("OpenAttachment: no attachment handler, opening URL directly.");
        OpenUrl(url);
        return;
    }

    std::string outFilePath;
    std::string outMime;
    std::string outError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, outError)) {
        LOG_WARN("OpenAttachment: %s; falling back to URL open.", outError.c_str());
        OpenUrl(url);
        return;
    }
    if (AttachmentViewerHandlerCallback) {
        LOG_INFO("OpenAttachment: dispatching to host attachment viewer.");
        AttachmentViewerHandlerCallback(outFilePath, outMime, filename);
        return;
    }

    if (AttachmentPreviewHandlerCallback && IsSupportedImageMime(outMime)) {
        if (AttachmentPreviewHandlerCallback(outFilePath, outMime, filename, url)) {
            LOG_INFO("OpenAttachment: queued in-app image preview.");
            return;
        }
        LOG_WARN("OpenAttachment: in-app preview rejected; falling back to URL open.");
    }

    LOG_INFO("OpenAttachment: opening attachment via URL fallback.");
    OpenUrl(url);
}

void AppController::OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename,
                                                 const std::string& mimeType) {
    if (url.empty()) {
        return;
    }
    std::string outFilePath;
    std::string outMime;
    std::string outError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, outError)) {
        LOG_WARN("OpenAttachmentInSystemViewer: %s; falling back to URL open.", outError.c_str());
        OpenUrl(url);
        return;
    }
    bool launchOk = false;
#if defined(_WIN32)
    const HINSTANCE shellResult = ShellExecuteA(nullptr, "open", outFilePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    launchOk = reinterpret_cast<intptr_t>(shellResult) > 32;
    if (!launchOk) {
        LOG_ERROR("OpenAttachmentInSystemViewer: ShellExecute failed path=%s err=%lu",
                  TruncateForLog(outFilePath, 300).c_str(), static_cast<unsigned long>(GetLastError()));
    }
#elif defined(__APPLE__)
    launchOk = LaunchCommandNoShell("open", outFilePath);
    if (!launchOk) {
        LOG_ERROR("OpenAttachmentInSystemViewer: open failed path=%s", TruncateForLog(outFilePath, 300).c_str());
    }
#else
    launchOk = LaunchCommandNoShell("xdg-open", outFilePath);
    if (!launchOk) {
        LOG_ERROR("OpenAttachmentInSystemViewer: xdg-open failed path=%s", TruncateForLog(outFilePath, 300).c_str());
    }
#endif
}

bool AppController::DownloadAttachmentForPreview(const std::string& url, const std::string& filename,
                                                 const std::string& mimeType, std::string* outError) {
    auto fail = [outError](const std::string& errorMessage) {
        if (outError != nullptr) {
            *outError = errorMessage;
        }
        return false;
    };
    if (!AttachmentPreviewHandlerCallback || !IsSupportedImageMime(mimeType)) {
        return fail(!AttachmentPreviewHandlerCallback ? std::string("Preview handler is unavailable.")
                                                      : std::string("Attachment is not a supported image type."));
    }
    std::string outFilePath;
    std::string outMime;
    std::string downloadError;
    if (!DownloadAttachmentToLocalFile(url, filename, mimeType, outFilePath, outMime, downloadError)) {
        LOG_WARN("DownloadAttachmentForPreview: %s", downloadError.c_str());
        return fail(downloadError);
    }
    if (!AttachmentPreviewHandlerCallback(outFilePath, outMime, filename, url)) {
        LOG_WARN("DownloadAttachmentForPreview: preview handler rejected file=%s mime=%s", filename.c_str(),
                 outMime.c_str());
        return fail("Preview handler rejected the attachment.");
    }
    if (outError != nullptr) {
        outError->clear();
    }
    return true;
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

namespace {
constexpr auto kJiraConnectivityProbeAggressiveInterval = std::chrono::seconds{20};
constexpr auto kJiraConnectivityProbeRelaxedInterval = std::chrono::seconds{90};
constexpr auto kOfflineReplayDelayWhileTransportDown = std::chrono::seconds{25};
} // namespace

void AppController::DrainJiraConnectivityProbeFuture() {
    if (!jiraConnectivityProbeInFlight_) {
        return;
    }
    try {
        if (jiraConnectivityProbeFuture_.valid()) {
            jiraConnectivityProbeFuture_.wait();
            (void)jiraConnectivityProbeFuture_.get();
        }
    } catch (...) {
        // Shutdown path: swallow probe failures.
    }
    jiraConnectivityProbeInFlight_ = false;
}

AppController::JiraConnectivityState AppController::MapReachabilityProbeKind(JiraReachabilityProbeKind k) {
    switch (k) {
    case JiraReachabilityProbeKind::AuthenticatedReachable:
        return JiraConnectivityState::AuthenticatedReachable;
    case JiraReachabilityProbeKind::ReachableAuthOrConfigError:
        return JiraConnectivityState::ReachableAuthOrConfigError;
    case JiraReachabilityProbeKind::TransportDown:
        return JiraConnectivityState::TransportDown;
    case JiraReachabilityProbeKind::ServiceUnavailable:
        return JiraConnectivityState::ServiceUnavailable;
    }
    return JiraConnectivityState::Unknown;
}

bool AppController::IsConnectivityDegradedForProbeInterval(JiraConnectivityState nextProbeState) const {
    if (nextProbeState == JiraConnectivityState::TransportDown ||
        nextProbeState == JiraConnectivityState::ServiceUnavailable) {
        return true;
    }
    if (!LastJiraTicketSyncWarning.empty() && IsJiraTransportErrorText(LastJiraTicketSyncWarning)) {
        return true;
    }
    const std::string& catalogErr = LastJiraFieldCatalogError;
    if (!catalogErr.empty() && IsJiraTransportErrorText(catalogErr)) {
        return true;
    }
    return false;
}

void AppController::PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now) {
    const auto pushTo = now + kOfflineReplayDelayWhileTransportDown;
    std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
    nextOfflineReplayAt_ = (std::max)(nextOfflineReplayAt_, pushTo);
    nextOfflineFieldEditReplayAt_ = (std::max)(nextOfflineFieldEditReplayAt_, pushTo);
}

void AppController::ApplyJiraConnectivityProbeResult(const std::chrono::steady_clock::time_point now,
                                                     const JiraReachabilityProbeResult& r) {
    const JiraConnectivityState prev = lastJiraConnectivityState_;
    const JiraConnectivityState next = MapReachabilityProbeKind(r.Kind);
    lastJiraConnectivityState_ = next;
    lastJiraConnectivityDiagnostic_ = r.Diagnostic;

    if (prev != next) {
        LOG_INFO("AppController: Jira connectivity probe state %d -> %d diag=%s", static_cast<int>(prev),
                 static_cast<int>(next), r.Diagnostic.c_str());
    }

    const bool nowAuthenticatedReachable = (next == JiraConnectivityState::AuthenticatedReachable);
    const bool wasConnectivityDegraded =
        (prev == JiraConnectivityState::TransportDown || prev == JiraConnectivityState::ServiceUnavailable ||
         prev == JiraConnectivityState::ReachableAuthOrConfigError);
    // First successful probe after startup can be Unknown -> AuthenticatedReachable while we still show
    // an offline/snapshot catalog banner from cache or a failed fetch; nudge a live catalog refresh.
    const bool coldStartCatalogBanner =
        (prev == JiraConnectivityState::Unknown && nowAuthenticatedReachable && !LastJiraFieldCatalogWarning.empty());
    if (nowAuthenticatedReachable && (wasConnectivityDegraded || coldStartCatalogBanner)) {
        if (!jiraConnectivityRecoveryPending_) {
            jiraConnectivityRecoveryPending_ = true;
            LOG_INFO("AppController: Jira authenticated reachability restored; UI recovery pending.");
        }
    }

    if (prev == JiraConnectivityState::AuthenticatedReachable &&
        (next == JiraConnectivityState::TransportDown || next == JiraConnectivityState::ServiceUnavailable)) {
        std::string diag = r.Diagnostic;
        constexpr std::size_t kMaxDiagChars = 200;
        if (diag.size() > kMaxDiagChars) {
            diag.resize(kMaxDiagChars);
        }
        LastJiraTicketSyncWarning = "Showing cached issues — lost connection to Jira: " + diag;
        LOG_WARN("AppController: Jira probe reports connectivity loss: %s", diag.c_str());
    }

    if (next == JiraConnectivityState::TransportDown || next == JiraConnectivityState::ServiceUnavailable) {
        PushOfflineReplayTimersDuringTransportOutage(now);
    }

    const auto interval = IsConnectivityDegradedForProbeInterval(next) ? kJiraConnectivityProbeAggressiveInterval
                                                                       : kJiraConnectivityProbeRelaxedInterval;
    nextJiraConnectivityProbeAt_ = now + interval;
}

void AppController::TickJiraConnectivityMonitor(const JiraConfig& cfg) {
    if (!JiraBackend || shuttingDown_.load()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();

    if (jiraConnectivityProbeInFlight_) {
        if (jiraConnectivityProbeFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            JiraReachabilityProbeResult r;
            try {
                r = jiraConnectivityProbeFuture_.get();
            } catch (...) {
                r.Kind = JiraReachabilityProbeKind::TransportDown;
                r.Diagnostic = "probe future exception";
            }
            jiraConnectivityProbeInFlight_ = false;
            ApplyJiraConnectivityProbeResult(now, r);
        }
        return;
    }

    if (now < nextJiraConnectivityProbeAt_) {
        return;
    }

    std::string authGate;
    if (!EnsureJiraAuthConfig(cfg, authGate)) {
        nextJiraConnectivityProbeAt_ = now + kJiraConnectivityProbeRelaxedInterval;
        return;
    }

    try {
        jiraConnectivityProbeFuture_ =
            std::async(std::launch::async, [cfg]() { return JiraClient::ProbeReachability(cfg); });
        jiraConnectivityProbeInFlight_ = true;
    } catch (...) {
        nextJiraConnectivityProbeAt_ = now + kJiraConnectivityProbeAggressiveInterval;
    }
}

bool AppController::ConsumeFieldCatalogRefetchAfterLiveTicketSync() {
    return fieldCatalogRefetchAfterLiveTicketSyncPending_.exchange(false, std::memory_order_acq_rel);
}

void AppController::requestDeferredLiveJiraBackendSuccessNotify_() const {
    if (!JiraBackend) {
        return;
    }
    deferredLiveJiraBackendSuccessNotify_.store(true, std::memory_order_release);
}

void AppController::applyLiveJiraReachabilityAfterSuccessfulBackendRequest_() {
    if (!JiraBackend) {
        return;
    }
    lastJiraConnectivityState_ = JiraConnectivityState::AuthenticatedReachable;
    LastJiraTicketSyncWarning.clear();
    if (!LastJiraFieldCatalogWarning.empty()) {
        LastJiraFieldCatalogWarning.clear();
        JiraFieldCatalogRevision.fetch_add(1);
        fieldCatalogRefetchAfterLiveTicketSyncPending_.store(true, std::memory_order_release);
    }
}

bool AppController::ConsumeDeferredLiveJiraBackendSuccessNotifyIfAny() {
    if (!deferredLiveJiraBackendSuccessNotify_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    applyLiveJiraReachabilityAfterSuccessfulBackendRequest_();
    return true;
}

namespace {

constexpr char kWorkingOfflineSnapshotCatalog[] =
    "Working offline: Jira field catalog loaded from local snapshot until a live refresh succeeds.";

std::string TruncateJiraBannerDetail(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) {
        return s;
    }
    if (maxLen <= 3) {
        return "...";
    }
    return s.substr(0, maxLen - 3) + "...";
}

std::string CatalogOfflineTechnicalSuffix(const std::string& cw) {
    if (cw.empty()) {
        return std::string();
    }
    static const char* prefixes[] = {
        "Offline: using cached Jira field catalog. Last fetch failed: ",
        "Offline: restored Jira field catalog from local snapshot. Last fetch failed: ",
        "Offline: no field catalog snapshot could be loaded. Last fetch failed: ",
    };
    for (const char* p : prefixes) {
        const size_t pl = std::strlen(p);
        if (cw.size() >= pl && cw.compare(0, pl, p) == 0) {
            return cw.substr(pl);
        }
    }
    if (cw == kWorkingOfflineSnapshotCatalog) {
        return std::string();
    }
    return TruncateJiraBannerDetail(cw, 100);
}

std::string TicketOfflineTechnicalSuffix(const std::string& tw) {
    if (tw.empty()) {
        return std::string();
    }
    static const char* prefixes[] = {
        "Showing cached issues — live refresh did not complete: ",
        "Showing cached issues — lost connection to Jira: ",
        // ASCII hyphen (some logs / older strings / copy-paste normalization).
        "Showing cached issues - live refresh did not complete: ",
        "Showing cached issues - lost connection to Jira: ",
    };
    for (const char* p : prefixes) {
        const size_t pl = std::strlen(p);
        if (tw.size() >= pl && tw.compare(0, pl, p) == 0) {
            return tw.substr(pl);
        }
    }
    return TruncateJiraBannerDetail(tw, 100);
}

void AppendSessionCatalogNoteToBanner(std::string& out, const std::string* sessionNote) {
    if (!sessionNote || sessionNote->empty()) {
        return;
    }
    if (!out.empty()) {
        out += " ";
    }
    out += "(" + TruncateJiraBannerDetail(*sessionNote, 90) + ")";
}

} // namespace

JiraConnectivityBannerForUi AppController::GetJiraConnectivityBannerForUi(const std::string* sessionCatalogNote) const {
    JiraConnectivityBannerForUi out;
    const std::string& ce = LastJiraFieldCatalogError;
    const std::string& cw = LastJiraFieldCatalogWarning;
    const std::string& tw = LastJiraTicketSyncWarning;
    const bool haveSession = sessionCatalogNote && !sessionCatalogNote->empty();
    const bool haveCw = !cw.empty();
    const bool haveTw = !tw.empty();

    if (!ce.empty()) {
        out.Kind = JiraConnectivityBannerForUi::Level::Error;
        out.Message = TruncateJiraBannerDetail(ce, 240);
        if (haveTw) {
            const std::string ts = TicketOfflineTechnicalSuffix(tw);
            if (!ts.empty()) {
                out.Message += " · Issues: ";
                out.Message += TruncateJiraBannerDetail(ts, 100);
            }
        }
        AppendSessionCatalogNoteToBanner(out.Message, sessionCatalogNote);
        return out;
    }

    if (!haveCw && !haveTw && haveSession) {
        out.Kind = JiraConnectivityBannerForUi::Level::Warning;
        out.Message = "Offline: ";
        out.Message += TruncateJiraBannerDetail(*sessionCatalogNote, 220);
        return out;
    }

    if (!haveCw && !haveTw) {
        return out;
    }

    out.Kind = JiraConnectivityBannerForUi::Level::Warning;
    const std::string catSuffix = CatalogOfflineTechnicalSuffix(cw);
    const std::string ticketSuffix = TicketOfflineTechnicalSuffix(tw);
    const bool snapshotCatalogOnly = haveCw && cw == kWorkingOfflineSnapshotCatalog;

    std::string headline;
    if (haveCw && haveTw) {
        headline = "Offline: issue list and field catalog are not live with Jira.";
    } else if (haveCw) {
        headline = snapshotCatalogOnly ? "Offline: field catalog is a local snapshot until a live refresh succeeds."
                                       : "Offline: field catalog is from cache (not refreshed from Jira).";
    } else {
        headline = "Offline: issue list is from cache (live refresh did not complete).";
    }

    std::string detail;
    if (!catSuffix.empty() && !ticketSuffix.empty()) {
        if (catSuffix == ticketSuffix) {
            detail = catSuffix;
        } else {
            detail = TruncateJiraBannerDetail(catSuffix, 70) + " · " + TruncateJiraBannerDetail(ticketSuffix, 70);
        }
    } else if (!catSuffix.empty()) {
        detail = catSuffix;
    } else if (!ticketSuffix.empty()) {
        detail = ticketSuffix;
    }

    out.Message = headline;
    if (!detail.empty()) {
        out.Message += " — ";
        out.Message += TruncateJiraBannerDetail(detail, 130);
    }
    AppendSessionCatalogNoteToBanner(out.Message, sessionCatalogNote);
    return out;
}

bool AppController::ConsumeJiraConnectivityRecovery() {
    if (!jiraConnectivityRecoveryPending_) {
        return false;
    }
    jiraConnectivityRecoveryPending_ = false;
    LastJiraTicketSyncWarning.clear();
    LastJiraFieldCatalogWarning.clear();
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        nextOfflineReplayAt_ = now;
        nextOfflineFieldEditReplayAt_ = now;
    }
    LOG_INFO("AppController: consumed Jira connectivity recovery latch.");
    return true;
}

namespace {
bool IsSprintField(const TrackerField& field) {
    return field.Family == TrackerFieldFamily::Sprint || field.SchemaCustom.find("gh-sprint") != std::string::npos;
}

bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId) {
    return fieldId == "timeoriginalestimate" || fieldId == "timeestimate";
}

bool IsNonEditableTimetrackingFieldId(const std::string& fieldId) {
    return fieldId == "timespent" || fieldId == "aggregatetimeoriginalestimate" || fieldId == "aggregatetimeestimate" ||
           fieldId == "aggregatetimespent";
}

bool ErrorTextContainsHttpStatus(const std::string& errorText, int statusCode) {
    if (statusCode < 100 || statusCode > 599) {
        return false;
    }
    const std::string needle = "HTTP " + std::to_string(statusCode);
    return errorText.find(needle) != std::string::npos;
}

} // namespace

void AppController::RefreshLocalData() {
    if (Cache) {
        auto latestTickets = Cache->GetAllTickets();
        {
            std::lock_guard<std::mutex> lock(activeTicketsMutex_);
            ActiveTickets = std::move(latestTickets);
            activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ActiveTickets);
        }
        PruneEditMetaCacheToActiveTickets();
        ActiveTicketsRevision.fetch_add(1);
    }
}

void AppController::UpdateTicket(const CachedTicket& ticket) {
    if (Cache) {
        Cache->SaveTicket(ticket);
        RefreshLocalData(); // Push changes back to ActiveTickets vector
    }
}

bool AppController::RefreshFieldCatalog(const JiraConfig& cfg) {
    if (!Backend) {
        SetFieldCatalog({}, {}, "Tracker backend is not initialized.");
        return false;
    }

    TrackerFieldCatalogResult catalog;
    std::string error;
    const bool ok = Backend->FetchFieldCatalog(cfg, catalog, error);
    if (!ok) {
        SetFieldCatalog({}, {}, error);
        LOG_ERROR("AppController::RefreshFieldCatalog failed: %s", error.c_str());
        return false;
    }

    SetFieldCatalog(std::move(catalog.Fields), std::move(catalog.Components), std::move(catalog.IssueTypeMeta), {});
    return true;
}

bool AppController::FetchFieldCatalog(const JiraConfig& cfg, TrackerFieldCatalogResult& outCatalog,
                                      std::string& outError) const {
    outCatalog = TrackerFieldCatalogResult{};
    outError.clear();
    if (!Backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    return Backend->FetchFieldCatalog(cfg, outCatalog, outError);
}

std::string AppController::BuildIssueBrowseUrl(const JiraConfig& cfg, const std::string& issueKey) const {
    return Backend ? Backend->BuildBrowseUrl(cfg, issueKey) : std::string();
}

std::string AppController::BuildJqlSearchUrl(const JiraConfig& cfg, const std::string& jql) const {
    if (cfg.Domain.empty() || jql.empty()) {
        return std::string();
    }
    return NormalizeBaseUrl(cfg.Domain) + "/issues/?jql=" + UrlEncode(jql);
}

void AppController::SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                                    const std::string& error) {
    SetFieldCatalog(std::move(fields), std::move(components), {}, error);
}

void AppController::SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                                    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error) {
    if (!error.empty()) {
        if (IsJiraTransportErrorText(error)) {
            if (!AvailableFields.empty()) {
                LastJiraFieldCatalogError.clear();
                const std::string nextWarning = "Offline: using cached Jira field catalog. Last fetch failed: " + error;
                if (nextWarning != LastJiraFieldCatalogWarning) {
                    LastJiraFieldCatalogWarning = nextWarning;
                    JiraFieldCatalogRevision.fetch_add(1);
                } else {
                    LastJiraFieldCatalogWarning = nextWarning;
                }
                LOG_WARN("AppController::SetFieldCatalog transport failure (catalog preserved): %s", error.c_str());
                return;
            }

            std::vector<TrackerField> snapFields;
            std::vector<TrackerComponent> snapComponents;
            std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta;
            std::string snapErr;
            if (FieldCatalogCache::TryLoadFieldCatalogSnapshot(snapFields, snapComponents, snapIssueTypeMeta,
                                                               snapErr)) {
                AvailableFields = std::move(snapFields);
                AvailableComponents = std::move(snapComponents);
                AvailableIssueTypeMeta = std::move(snapIssueTypeMeta);
                fieldCatalogEverLoaded_ = true;
                LastJiraFieldCatalogError.clear();
                LastJiraFieldCatalogWarning =
                    "Offline: restored Jira field catalog from local snapshot. Last fetch failed: " + error;
                for (auto& field : AvailableFields) {
                    if (field.Id == "comment" || IsNonEditableTimetrackingFieldId(field.Id)) {
                        field.ReadOnly = true;
                    }
                }
                EnsureCatalogHistoryField();
                JiraFieldCatalogRevision.fetch_add(1);
                LOG_WARN("AppController::SetFieldCatalog transport failure; loaded snapshot err=%s", snapErr.c_str());
                return;
            }

            if (fieldCatalogEverLoaded_) {
                LastJiraFieldCatalogError.clear();
                LastJiraFieldCatalogWarning =
                    "Offline: no field catalog snapshot could be loaded. Last fetch failed: " + error;
                JiraFieldCatalogRevision.fetch_add(1);
                LOG_WARN("AppController::SetFieldCatalog transport failure; no snapshot (session had catalog): %s",
                         error.c_str());
                return;
            }

            AvailableFields.clear();
            AvailableComponents.clear();
            AvailableIssueTypeMeta.clear();
            fieldCatalogEverLoaded_ = false;
            LastJiraFieldCatalogWarning.clear();
            LastJiraFieldCatalogError = "No cached Jira field catalog available. " +
                                        (error.empty() ? std::string("Last fetch failed.") : error);
            JiraFieldCatalogRevision.fetch_add(1);
            LOG_ERROR("AppController::SetFieldCatalog error (no cache): %s", error.c_str());
            return;
        }

        AvailableFields.clear();
        AvailableComponents.clear();
        AvailableIssueTypeMeta.clear();
        fieldCatalogEverLoaded_ = false;
        LastJiraFieldCatalogWarning.clear();
        LastJiraFieldCatalogError = error;
        JiraFieldCatalogRevision.fetch_add(1);
        LOG_ERROR("AppController::SetFieldCatalog error: %s", error.c_str());
        return;
    }

    AvailableFields = std::move(fields);
    AvailableComponents = std::move(components);
    AvailableIssueTypeMeta = std::move(issueTypeMeta);
    LastJiraFieldCatalogError.clear();
    LastJiraFieldCatalogWarning.clear();
    fieldCatalogEverLoaded_ = true;
    requestDeferredLiveJiraBackendSuccessNotify_();
    {
        std::string snapErr;
        if (!FieldCatalogCache::SaveFieldCatalogSnapshot(AvailableFields, AvailableComponents, AvailableIssueTypeMeta,
                                                         snapErr)) {
            LOG_WARN("AppController::SetFieldCatalog: snapshot save failed: %s", snapErr.c_str());
        }
    }
    for (auto& field : AvailableFields) {
        if (field.Id == "comment" || IsNonEditableTimetrackingFieldId(field.Id)) {
            field.ReadOnly = true;
        }
    }
    EnsureCatalogHistoryField();

    JiraFieldCatalogRevision.fetch_add(1);
}

const TrackerField* AppController::FindFieldById(const std::string& fieldId) const {
    const auto it = std::find_if(AvailableFields.begin(), AvailableFields.end(),
                                 [&](const TrackerField& field) { return field.Id == fieldId; });
    return it == AvailableFields.end() ? nullptr : &(*it);
}

void AppController::EnsureCatalogHistoryField() {
    if (FindFieldById("history") != nullptr) {
        return;
    }
    TrackerField historyField;
    historyField.Id = "history";
    historyField.Name = "History";
    historyField.ReadOnly = true;
    AvailableFields.push_back(std::move(historyField));
}

bool AppController::FieldEditSupportsOfflineQueue(const TrackerField& field) {
    if (IsSprintField(field)) {
        return false;
    }
    if (IsNonEditableTimetrackingFieldId(field.Id) || IsEditableTimetrackingEstimateFieldId(field.Id)) {
        return false;
    }
    switch (field.Family) {
    case TrackerFieldFamily::Text:
    case TrackerFieldFamily::Number:
    case TrackerFieldFamily::Date:
    case TrackerFieldFamily::DateTime:
    case TrackerFieldFamily::Labels:
    case TrackerFieldFamily::SelectSingle:
    case TrackerFieldFamily::SelectMulti:
    case TrackerFieldFamily::UserSingle:
    case TrackerFieldFamily::UserMulti:
    case TrackerFieldFamily::CascadingSelect:
        return true;
    default:
        return false;
    }
}

bool AppController::TryBuildFieldEditPayloadForNetwork(
    const std::string& issueId, const TrackerField& field, const std::vector<std::string>& rawValues,
    const std::string& originalEstimateSnapshot, const std::string& remainingEstimateSnapshot,
    const std::string& issueTypeKeySnapshot, nlohmann::json& outFieldsPayload,
    std::unordered_map<std::string, std::string>& outDisplayValues, std::string& outError) {
    (void)originalEstimateSnapshot;
    (void)remainingEstimateSnapshot;
    outError.clear();
    outDisplayValues.clear();
    outFieldsPayload = nlohmann::json::object();
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }
    if (IsSprintField(field) || IsNonEditableTimetrackingFieldId(field.Id) ||
        IsEditableTimetrackingEstimateFieldId(field.Id)) {
        outError = "Field type not supported for this edit path.";
        return false;
    }

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    for (const auto& value : rawValues) {
        if (!value.empty()) {
            values.push_back(value);
        }
    }

    const std::string* issueTypeKeyOpt = issueTypeKeySnapshot.empty() ? nullptr : &issueTypeKeySnapshot;
    if (JiraBackend && !IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id)) {
        EnsureIssueEditMetaLoaded(issueId, nullptr, issueTypeKeyOpt);
    }
    if (JiraBackend && !IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id) &&
        !CanEditFieldForIssue(issueId, field.Id, &field, issueTypeKeyOpt)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        return false;
    }

    nlohmann::json valuePayload;
    std::string buildErr;
    const JiraField jiraField = JiraTrackerFieldAdapter::ToJiraField(field);
    if (!JiraFieldPayload::BuildValue(jiraField, rawValues, valuePayload, buildErr)) {
        outError = buildErr.empty() ? std::string("Invalid field value.") : buildErr;
        return false;
    }

    outFieldsPayload = nlohmann::json::object();
    outFieldsPayload[field.Id] = std::move(valuePayload);

    std::string displayValue;
    if (!values.empty()) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                displayValue += ", ";
            }
            displayValue += JiraFieldPayload::ResolveDisplayValueForSubmittedSelection(jiraField, values[i]);
        }
    }
    outDisplayValues[field.Id] = std::move(displayValue);
    return true;
}

// --- Create-issue helpers -------------------------------------------------

IssueDraft AppController::BuildDraftFromLastTicket(const JiraConfig& cfg) const {
    const auto snap = GetActiveTicketsSnapshot();
    const auto& tickets = *snap;
    CachedTicket lastTicket;
    if (!tickets.empty()) {
        lastTicket = tickets.back();
    }
    return IssueDraftHelpers::FromCachedTicket(lastTicket, AvailableFields, cfg.ProjectKey, cfg.DefaultIssueTypeId,
                                               cfg.DefaultIssueTypeName, cfg.NewIssueInheritFieldIds);
}

RequiredFieldSet AppController::GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                                    const std::string& issueTypeName) const {
    RequiredFieldSet result;
    for (const auto& entry : AvailableIssueTypeMeta) {
        const bool projectMatch = entry.ProjectKey.empty() || projectKey.empty() || entry.ProjectKey == projectKey;
        if (!projectMatch) {
            continue;
        }
        const bool idMatch = !issueTypeId.empty() && entry.IssueTypeId == issueTypeId;
        const bool nameMatch = !issueTypeName.empty() && entry.IssueTypeName == issueTypeName;
        if (idMatch || nameMatch) {
            result.FieldIds = entry.RequiredFieldIds;
            result.IsSubtask = entry.IsSubtask;
            return result;
        }
    }
    return result; // empty -> hard minimum only
}

std::future<IssueCreateResult> AppController::CreateIssueAsync(const IssueDraft& draft) {
    // Snapshot state the worker needs up front so we don't race with UI edits.
    auto catalogCopy = std::make_shared<std::vector<TrackerField>>(AvailableFields);
    const RequiredFieldSet required = GetRequiredFieldSet(draft.ProjectKey, draft.IssueTypeId, draft.IssueTypeName);
    std::shared_ptr<std::promise<IssueCreateResult>> promise = std::make_shared<std::promise<IssueCreateResult>>();
    std::future<IssueCreateResult> future = promise->get_future();

    if (!Backend) {
        IssueCreateResult err;
        err.Error = "Tracker backend is not initialized.";
        promise->set_value(std::move(err));
        return future;
    }

    ITrackerClient* backend = Backend.get();
    LocalCacheManager* cache = Cache.get();
    IssueDraft draftCopy = draft;

    // Update path: prune fields whose draft value already matches cache so we don't
    // round-trip ADF descriptions (formatting loss), re-run no-op transitions for
    // status, or re-add issues to their current sprint.
    if (!draftCopy.ExistingIssueKey.empty()) {
        const auto snap = GetActiveTicketsSnapshot();
        if (snap) {
            const std::string key = draftCopy.ExistingIssueKey;
            for (const auto& t : *snap) {
                if (t.id == key) {
                    IssueDraftHelpers::PruneUnchangedFields(draftCopy, t);
                    break;
                }
            }
        }
    }

    LaunchBackgroundTask([this, promise, backend, cache, draftCopy, catalogCopy, required]() {
        IssueCreateResult result = IssueCreatePipeline::Run(*backend, cache, draftCopy, required, *catalogCopy);
        if (result.Ok) {
            RefreshLocalData();
            requestDeferredLiveJiraBackendSuccessNotify_();
        }
        promise->set_value(std::move(result));
    });
    return future;
}

std::int64_t AppController::QueueCreateOffline(const IssueDraft& draft) {
    if (!Cache) {
        LOG_WARN("AppController::QueueCreateOffline skipped: cache not initialized.");
        return 0;
    }
    const std::string payload = IssueDraftHelpers::ToJson(draft);
    try {
        const std::int64_t id = Cache->EnqueuePendingCreate(payload);
        LOG_INFO("AppController: queued offline create id=%lld", static_cast<long long>(id));
        BackendAuditTrail::AppendResult(
            "offline_queue_create", "ui", std::string(), std::to_string(id), true, std::string(),
            nlohmann::json{{"pending_create_id", id}, {"draft", IssueDraftHelpers::ToJson(draft)}});
        return id;
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::QueueCreateOffline failed: %s", ex.what());
        BackendAuditTrail::AppendResult("offline_queue_create", "ui", std::string(),
                                        BackendAuditTrail::MakeOperationId("offline-queue"), false, ex.what(),
                                        nlohmann::json{{"draft", IssueDraftHelpers::ToJson(draft)}});
        return 0;
    }
}

size_t AppController::GetPendingCreateCount() const {
    if (!Cache) {
        return 0;
    }
    try {
        return Cache->LoadPendingCreates().size();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::GetPendingCreateCount failed: %s", ex.what());
        return 0;
    } catch (...) {
        LOG_ERROR("AppController::GetPendingCreateCount failed: unknown exception");
        return 0;
    }
}

size_t AppController::GetDeadPendingCreateCount() const {
    if (!Cache) {
        return 0;
    }
    try {
        return Cache->GetDeadPendingCreateCount();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::GetDeadPendingCreateCount failed: %s", ex.what());
        return 0;
    } catch (...) {
        LOG_ERROR("AppController::GetDeadPendingCreateCount failed: unknown exception");
        return 0;
    }
}

std::vector<DeadPendingCreate> AppController::GetDeadPendingCreates() const {
    if (!Cache) {
        return {};
    }
    try {
        return Cache->LoadDeadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::GetDeadPendingCreates failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("AppController::GetDeadPendingCreates failed: unknown exception");
        return {};
    }
}

std::vector<PendingCreate> AppController::GetPendingCreates() const {
    if (!Cache) {
        return {};
    }
    try {
        return Cache->LoadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::GetPendingCreates failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("AppController::GetPendingCreates failed: unknown exception");
        return {};
    }
}

AppController::DeadLetterRestoreSummary
AppController::RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds) {
    DeadLetterRestoreSummary summary;
    if (!Cache || originalIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : originalIds) {
        try {
            if (Cache->RestoreDeadPendingCreate(id)) {
                ++summary.Restored;
                BackendAuditTrail::AppendResult("offline_dead_letter_restore", "ui", std::string(), std::to_string(id),
                                                true, std::string(),
                                                nlohmann::json{{"original_pending_create_id", id}});
            } else {
                ++summary.Failed;
                BackendAuditTrail::AppendResult("offline_dead_letter_restore", "ui", std::string(), std::to_string(id),
                                                false, "Row not found.",
                                                nlohmann::json{{"original_pending_create_id", id}});
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::RestoreDeadPendingCreates id=%lld err=%s", static_cast<long long>(id), ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_restore", "ui", std::string(), std::to_string(id),
                                            false, ex.what(), nlohmann::json{{"original_pending_create_id", id}});
        } catch (...) {
            LOG_ERROR("AppController::RestoreDeadPendingCreates id=%lld unknown exception", static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_restore", "ui", std::string(), std::to_string(id),
                                            false, "Unknown exception.",
                                            nlohmann::json{{"original_pending_create_id", id}});
        }
    }
    return summary;
}

std::string AppController::TakeLegacyPendingStartupBanner() { return std::move(legacyPendingStartupBanner_); }

AppController::DeadLetterDeleteSummary
AppController::DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds) {
    DeadLetterDeleteSummary summary;
    if (!Cache || deadIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : deadIds) {
        try {
            Cache->DeleteDeadPendingCreate(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_dead_letter_delete", "ui", std::string(), std::to_string(id), true,
                                            std::string(), nlohmann::json{{"dead_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::DeleteDeadPendingCreates dead_id=%lld err=%s", static_cast<long long>(id),
                      ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_delete", "ui", std::string(), std::to_string(id),
                                            false, ex.what(), nlohmann::json{{"dead_id", id}});
        } catch (...) {
            LOG_ERROR("AppController::DeleteDeadPendingCreates dead_id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_delete", "ui", std::string(), std::to_string(id),
                                            false, "Unknown exception.", nlohmann::json{{"dead_id", id}});
        }
    }
    return summary;
}

AppController::PendingQueueDeleteSummary
AppController::DeletePendingCreates(const std::vector<std::int64_t>& pendingIds) {
    PendingQueueDeleteSummary summary;
    if (!Cache || pendingIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : pendingIds) {
        try {
            Cache->DeletePendingCreate(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_queue_delete", "ui", std::string(), std::to_string(id), true,
                                            std::string(), nlohmann::json{{"pending_create_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::DeletePendingCreates id=%lld err=%s", static_cast<long long>(id), ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_delete", "ui", std::string(), std::to_string(id), false,
                                            ex.what(), nlohmann::json{{"pending_create_id", id}});
        } catch (...) {
            LOG_ERROR("AppController::DeletePendingCreates id=%lld unknown exception", static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_delete", "ui", std::string(), std::to_string(id), false,
                                            "Unknown exception.", nlohmann::json{{"pending_create_id", id}});
        }
    }
    return summary;
}

std::int64_t AppController::QueueFieldEditOffline(const std::string& issueKey, const std::string& fieldId,
                                                  const std::string& fieldsPayloadJson, std::string& outError) {
    outError.clear();
    if (!Cache) {
        outError = "Cache is not initialized.";
        return 0;
    }
    if (issueKey.empty() || fieldId.empty() || fieldsPayloadJson.empty()) {
        outError = "Invalid offline field edit enqueue parameters.";
        return 0;
    }
    try {
        const std::int64_t id = Cache->EnqueuePendingFieldEdit(issueKey, fieldId, fieldsPayloadJson);
        LOG_INFO("AppController: queued offline field edit id=%lld issue=%s field=%s", static_cast<long long>(id),
                 issueKey.c_str(), fieldId.c_str());
        BackendAuditTrail::AppendResult("offline_queue_field_edit", "ui", issueKey, std::to_string(id), true,
                                        std::string(),
                                        nlohmann::json{{"pending_field_edit_id", id}, {"field_id", fieldId}});
        return id;
    } catch (const std::exception& ex) {
        outError = ex.what();
        LOG_ERROR("AppController::QueueFieldEditOffline failed: %s", ex.what());
        BackendAuditTrail::AppendResult("offline_queue_field_edit", "ui", issueKey,
                                        BackendAuditTrail::MakeOperationId("offline-field-queue"), false, ex.what(),
                                        nlohmann::json{{"field_id", fieldId}});
        return 0;
    }
}

std::vector<PendingFieldEditRecord> AppController::GetPendingFieldEdits() const {
    if (!Cache) {
        return {};
    }
    try {
        return Cache->LoadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::GetPendingFieldEdits failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("AppController::GetPendingFieldEdits failed: unknown exception");
        return {};
    }
}

std::vector<DeadPendingFieldEdit> AppController::GetDeadPendingFieldEdits() const {
    if (!Cache) {
        return {};
    }
    try {
        return Cache->LoadDeadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::GetDeadPendingFieldEdits failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("AppController::GetDeadPendingFieldEdits failed: unknown exception");
        return {};
    }
}

AppController::PendingFieldEditDeleteSummary
AppController::DeletePendingFieldEdits(const std::vector<std::int64_t>& ids) {
    PendingFieldEditDeleteSummary summary;
    if (!Cache || ids.empty()) {
        return summary;
    }
    for (const std::int64_t id : ids) {
        try {
            Cache->DeletePendingFieldEdit(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            true, std::string(), nlohmann::json{{"pending_field_edit_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::DeletePendingFieldEdits id=%lld err=%s", static_cast<long long>(id), ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            false, ex.what(), nlohmann::json{{"pending_field_edit_id", id}});
        } catch (...) {
            LOG_ERROR("AppController::DeletePendingFieldEdits id=%lld unknown exception", static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            false, "Unknown exception.", nlohmann::json{{"pending_field_edit_id", id}});
        }
    }
    return summary;
}

AppController::DeadFieldEditDeleteSummary
AppController::DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds) {
    DeadFieldEditDeleteSummary summary;
    if (!Cache || deadIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : deadIds) {
        try {
            Cache->DeleteDeadPendingFieldEdit(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            true, std::string(), nlohmann::json{{"dead_field_edit_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::DeleteDeadPendingFieldEdits dead_id=%lld err=%s", static_cast<long long>(id),
                      ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            false, ex.what(), nlohmann::json{{"dead_field_edit_id", id}});
        } catch (...) {
            LOG_ERROR("AppController::DeleteDeadPendingFieldEdits dead_id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            false, "Unknown exception.", nlohmann::json{{"dead_field_edit_id", id}});
        }
    }
    return summary;
}

void AppController::TickOfflineFieldEdits() {
    if (!Cache || !JiraBackend) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        if (offlineFieldEditReplayInFlight_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextOfflineFieldEditReplayAt_) {
            return;
        }
        offlineFieldEditReplayInFlight_ = true;
    }

    std::vector<PendingFieldEditRecord> pending;
    try {
        pending = Cache->LoadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::TickOfflineFieldEdits load failed: %s", ex.what());
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    } catch (...) {
        LOG_ERROR("AppController::TickOfflineFieldEdits load failed: unknown exception");
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    }
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    }

    LocalCacheManager* cache = Cache.get();
    LaunchBackgroundTask([this, pending, cache]() {
        JiraClient client;
        const JiraConfig cfg = ConfigManager::Load();
        int successes = 0;
        int failures = 0;
        int archived = 0;
        int cacheOpFailures = 0;
        bool ranUpdate = false;

        const auto tryCacheMutation = [&](const char* action, std::int64_t id,
                                          const std::function<void()>& fn) -> bool {
            try {
                fn();
                return true;
            } catch (const std::exception& ex) {
                ++cacheOpFailures;
                LOG_ERROR("AppController::TickOfflineFieldEdits %s failed id=%lld err=%s", action,
                          static_cast<long long>(id), ex.what());
                return false;
            } catch (...) {
                ++cacheOpFailures;
                LOG_ERROR("AppController::TickOfflineFieldEdits %s failed id=%lld err=unknown exception", action,
                          static_cast<long long>(id));
                return false;
            }
        };

        const int kMaxReplayAttempts = OfflineFieldEditQueue::kMaxReplayAttempts;
        for (const auto& row : pending) {
            if (row.Attempts >= kMaxReplayAttempts) {
                char detailBuf[384];
                std::snprintf(detailBuf, sizeof(detailBuf),
                              "Queued offline field edit id %lld already at attempts=%d (max=%d).",
                              static_cast<long long>(row.Id), row.Attempts, kMaxReplayAttempts);
                const std::string terminal = FormatOfflineQueueTerminalLine("offline_field_replay", "max_attempt_gate",
                                                                            "max_attempts", detailBuf);
                if (tryCacheMutation("archive_pending_field_edit", row.Id,
                                     [&]() { cache->ArchivePendingFieldEdit(row.Id, "max_attempts", terminal); })) {
                    ++archived;
                } else {
                    ++failures;
                }
                continue;
            }

            nlohmann::json fieldsPayload;
            try {
                fieldsPayload = nlohmann::json::parse(row.FieldsPayloadJson);
            } catch (const std::exception& ex) {
                const std::string terminal = FormatOfflineQueueTerminalLine("offline_field_replay", "parse_payload",
                                                                            "malformed_json", ex.what());
                if (tryCacheMutation("archive_pending_field_edit_malformed", row.Id,
                                     [&]() { cache->ArchivePendingFieldEdit(row.Id, "malformed_json", terminal); })) {
                    ++archived;
                } else {
                    ++failures;
                }
                continue;
            }

            ranUpdate = true;
            std::string err;
            if (!client.UpdateIssueFields(row.IssueKey, fieldsPayload, err)) {
                if (IsJiraTransportErrorText(err)) {
                    const int nextAttempts = row.Attempts + 1;
                    if (nextAttempts >= kMaxReplayAttempts) {
                        const std::string terminal =
                            FormatOfflineQueueTerminalLine("offline_field_replay", "transport_cap", "max_attempts",
                                                           "Transport failures exhausted replay attempts: " + err);
                        if (tryCacheMutation("archive_pending_field_edit_transport_cap", row.Id, [&]() {
                                cache->UpdatePendingFieldEdit(row.Id, nextAttempts, err);
                                cache->ArchivePendingFieldEdit(row.Id, "transport_cap", terminal);
                            })) {
                            ++archived;
                        } else {
                            ++failures;
                        }
                    } else {
                        (void)tryCacheMutation("update_pending_field_edit", row.Id,
                                               [&]() { cache->UpdatePendingFieldEdit(row.Id, nextAttempts, err); });
                        ++failures;
                    }
                } else {
                    const std::string terminal =
                        FormatOfflineQueueTerminalLine("offline_field_replay", "replay_rejected", "jira_error", err);
                    if (tryCacheMutation("archive_pending_field_edit_rejected", row.Id, [&]() {
                            cache->ArchivePendingFieldEdit(row.Id, "replay_rejected", terminal);
                        })) {
                        ++archived;
                    } else {
                        ++failures;
                    }
                }
                continue;
            }

            if (tryCacheMutation("delete_pending_field_edit", row.Id,
                                 [&]() { cache->DeletePendingFieldEdit(row.Id); })) {
                ++successes;
                requestDeferredLiveJiraBackendSuccessNotify_();
                BackendAuditTrail::AppendResult(
                    "offline_replay_field_edit", "offline_field_replay", row.IssueKey, std::to_string(row.Id), true,
                    std::string(), nlohmann::json{{"pending_field_edit_id", row.Id}, {"field_id", row.FieldId}});
            } else {
                ++failures;
            }
        }

        if (successes > 0) {
            RefreshLocalData();
        }
        if (successes > 0 || failures > 0 || archived > 0 || cacheOpFailures > 0) {
            LOG_INFO("AppController: offline field edit replay finished successes=%d failures=%d archived=%d "
                     "cache_op_failures=%d",
                     successes, failures, archived, cacheOpFailures);
        }

        std::chrono::seconds delay{5};
        if (!ranUpdate && !pending.empty()) {
            delay = std::chrono::seconds(300);
        } else if (failures > 0 && successes == 0) {
            delay = std::chrono::seconds(30);
        }
        const auto nextAt = std::chrono::steady_clock::now() + delay;
        {
            std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
            nextOfflineFieldEditReplayAt_ = nextAt;
            offlineFieldEditReplayInFlight_ = false;
        }
    });
}

void AppController::TickOfflineCreates() {
    if (!Cache || !Backend) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        if (offlineReplayInFlight_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextOfflineReplayAt_) {
            return;
        }
        offlineReplayInFlight_ = true;
    }

    std::vector<PendingCreate> pending;
    try {
        pending = Cache->LoadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::TickOfflineCreates load failed: %s", ex.what());
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    } catch (...) {
        LOG_ERROR("AppController::TickOfflineCreates load failed: unknown exception");
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    }
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    }

    // Defer heavy work to a worker. Grab snapshots the worker needs.
    auto catalogCopy = std::make_shared<std::vector<TrackerField>>(AvailableFields);
    ITrackerClient* backend = Backend.get();
    LocalCacheManager* cache = Cache.get();
    // Hard cap attempts per row so a permanently-bad payload doesn't loop forever.

    LaunchBackgroundTask([this, pending, backend, cache, catalogCopy]() {
        const int kMaxReplayAttempts = OfflineCreateQueue::kMaxReplayAttempts;
        int successes = 0;
        int failures = 0;
        int archived = 0;
        int cacheOpFailures = 0;
        bool ranCreate = false;
        const auto tryCacheMutation = [&](const char* action, std::int64_t id,
                                          const std::function<void()>& fn) -> bool {
            try {
                fn();
                return true;
            } catch (const std::exception& ex) {
                ++cacheOpFailures;
                LOG_ERROR("AppController::TickOfflineCreates %s failed id=%lld err=%s", action,
                          static_cast<long long>(id), ex.what());
                return false;
            } catch (...) {
                ++cacheOpFailures;
                LOG_ERROR("AppController::TickOfflineCreates %s failed id=%lld err=unknown exception", action,
                          static_cast<long long>(id));
                return false;
            }
        };
        const auto archivePending = [&](const PendingCreate& pc, const std::string& reason,
                                        const std::string& terminalError) -> bool {
            return tryCacheMutation("archive_pending_create", pc.Id,
                                    [&]() { cache->ArchivePendingCreate(pc.Id, reason, terminalError); });
        };
        for (const auto& pc : pending) {
            if (pc.Attempts >= kMaxReplayAttempts) {
                char detailBuf[384];
                std::snprintf(detailBuf, sizeof(detailBuf),
                              "Queued offline create id %lld already at attempts=%d (max=%d). "
                              "Offline replay did not call Jira create; entry moved to Failed offline creates.",
                              static_cast<long long>(pc.Id), pc.Attempts, kMaxReplayAttempts);
                const std::string terminal =
                    FormatOfflineQueueTerminalLine("offline_replay", "max_attempt_gate", "max_attempts", detailBuf);
                if (archivePending(pc, "max_attempts", terminal)) {
                    ++archived;
                    BackendAuditTrail::AppendResult(
                        "offline_dead_letter", "offline_replay", std::string(), std::to_string(pc.Id), true,
                        std::string(), nlohmann::json{{"pending_create_id", pc.Id}, {"reason", "max_attempts"}});
                } else {
                    ++failures;
                }
                continue;
            }
            IssueDraft draft;
            std::string parseErr;
            if (!IssueDraftHelpers::FromJson(pc.Payload, draft, parseErr)) {
                LOG_WARN("AppController: archiving malformed pending_create id=%lld err=%s",
                         static_cast<long long>(pc.Id), parseErr.c_str());
                const std::string terminal =
                    FormatOfflineQueueTerminalLine("offline_replay", "parse_payload", "malformed_payload",
                                                   std::string("IssueDraft JSON parse failed: ") + parseErr);
                if (archivePending(pc, "malformed_payload", terminal)) {
                    ++archived;
                    BackendAuditTrail::AppendResult("offline_dead_letter", "offline_replay", std::string(),
                                                    std::to_string(pc.Id), true, std::string(),
                                                    nlohmann::json{{"pending_create_id", pc.Id},
                                                                   {"reason", "malformed_payload"},
                                                                   {"error", parseErr}});
                } else {
                    ++failures;
                }
                continue;
            }
            const RequiredFieldSet required =
                GetRequiredFieldSet(draft.ProjectKey, draft.IssueTypeId, draft.IssueTypeName);
            ranCreate = true;
            IssueCreateResult result = IssueCreatePipeline::Run(*backend, cache, draft, required, *catalogCopy);
            if (result.Ok) {
                if (tryCacheMutation("delete_pending_create", pc.Id, [&]() { cache->DeletePendingCreate(pc.Id); })) {
                    ++successes;
                    requestDeferredLiveJiraBackendSuccessNotify_();
                    BackendAuditTrail::AppendResult(
                        "offline_replay_create", "offline_replay", result.IssueKey, std::to_string(pc.Id), true,
                        std::string(), nlohmann::json{{"pending_create_id", pc.Id}, {"attempts_before", pc.Attempts}});
                } else {
                    ++failures;
                }
            } else {
                const int nextAttempts = pc.Attempts + 1;
                if (nextAttempts >= kMaxReplayAttempts) {
                    std::string trackerPart =
                        result.Error.empty()
                            ? std::string("Create pipeline returned failure with empty error on final attempt.")
                            : std::string("Create pipeline error: ") + result.Error;
                    char headBuf[224];
                    std::snprintf(headBuf, sizeof(headBuf), "Offline replay attempts went from %d to %d (cap=%d). ",
                                  pc.Attempts, nextAttempts, kMaxReplayAttempts);
                    const std::string terminalError = FormatOfflineQueueTerminalLine(
                        "offline_replay", "issue_create", "max_attempts", std::string(headBuf) + trackerPart);
                    const bool archivedOk = tryCacheMutation("archive_pending_create", pc.Id, [&]() {
                        cache->UpdatePendingCreate(pc.Id, nextAttempts, terminalError);
                        cache->ArchivePendingCreate(pc.Id, "max_attempts", terminalError);
                    });
                    if (archivedOk) {
                        ++archived;
                        BackendAuditTrail::AppendResult("offline_dead_letter", "offline_replay", std::string(),
                                                        std::to_string(pc.Id), true, std::string(),
                                                        nlohmann::json{{"pending_create_id", pc.Id},
                                                                       {"reason", "max_attempts"},
                                                                       {"error", result.Error}});
                    } else {
                        ++failures;
                    }
                } else {
                    const bool updateOk = tryCacheMutation("update_pending_create", pc.Id, [&]() {
                        cache->UpdatePendingCreate(pc.Id, nextAttempts, result.Error);
                    });
                    (void)updateOk;
                    BackendAuditTrail::AppendResult("offline_replay_create", "offline_replay", std::string(),
                                                    std::to_string(pc.Id), false, result.Error,
                                                    nlohmann::json{{"pending_create_id", pc.Id},
                                                                   {"attempts_before", pc.Attempts},
                                                                   {"attempts_after", nextAttempts}});
                    ++failures;
                }
            }
        }
        if (successes > 0) {
            RefreshLocalData();
        }
        if (successes > 0 || failures > 0 || archived > 0 || cacheOpFailures > 0) {
            LOG_INFO("AppController: offline replay finished successes=%d failures=%d archived=%d cache_op_failures=%d",
                     successes, failures, archived, cacheOpFailures);
        }
        // Back off if all failed; otherwise try again soon. Pending rows only skipped (max
        // attempts) still tick every few seconds — use a long delay when no create ran.
        std::chrono::seconds delay{5};
        if (!ranCreate && !pending.empty()) {
            delay = std::chrono::seconds(300);
        } else if (failures > 0 && successes == 0) {
            delay = std::chrono::seconds(30);
        }
        const auto nextAt = std::chrono::steady_clock::now() + delay;
        {
            std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
            nextOfflineReplayAt_ = nextAt;
            offlineReplayInFlight_ = false;
        }
    });
}

std::string AppController::ResolveIssueTypeKeyForIssue(const std::string& issueId) const {
    if (issueId.empty()) {
        return std::string();
    }
    const auto ticketsSnap = GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    const auto it =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (it == tickets.end()) {
        return std::string();
    }
    return ToLowerAsciiCopy(TrimCopy(it->GetFieldValue("issuetype")));
}

void AppController::WarmIssueTypeEditMetaAtStartAsync() {
    if (!JiraBackend) {
        return;
    }
    const auto ticketsSnap = GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    std::vector<std::pair<std::string, std::string>> representatives;
    std::unordered_set<std::string> seenTypes;
    seenTypes.reserve(tickets.size());
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        for (const auto& ticket : tickets) {
            if (ticket.id.empty()) {
                continue;
            }
            const std::string typeKey = ToLowerAsciiCopy(TrimCopy(ticket.GetFieldValue("issuetype")));
            if (typeKey.empty()) {
                continue;
            }
            if (seenTypes.find(typeKey) != seenTypes.end()) {
                continue;
            }
            const auto typeIt = issueTypeEditMeta_.find(typeKey);
            if (typeIt != issueTypeEditMeta_.end() && typeIt->second.loaded) {
                seenTypes.insert(typeKey);
                continue;
            }
            seenTypes.insert(typeKey);
            representatives.push_back({typeKey, ticket.id});
        }
    }
    if (representatives.empty()) {
        return;
    }
    LaunchBackgroundTask([this, representatives]() {
        for (const auto& pair : representatives) {
            if (shuttingDown_.load()) {
                break;
            }
            std::string ignored;
            EnsureIssueEditMetaLoaded(pair.second, &ignored);
        }
    });
}

bool AppController::CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                                         const TrackerField* fieldMeta, const std::string* issueTypeKeyOverride) const {
    if (!JiraBackend || issueId.empty() || fieldId.empty()) {
        return true;
    }
    if (IsEditableTimetrackingEstimateFieldId(fieldId)) {
        return true;
    }
    const TrackerField* meta = fieldMeta ? fieldMeta : FindFieldById(fieldId);
    if (meta && IsSprintField(*meta)) {
        return true;
    }
    const std::string fieldKey = ToLowerAsciiCopy(fieldId);
    // Edit metadata usually does not include a plain "set" for status; Jira applies changes via
    // POST /issue/{key}/transitions (see JiraClient::UpdateIssueFields).
    if (fieldKey == "status") {
        return true;
    }
    std::string issueTypeKey;
    if (issueTypeKeyOverride && !issueTypeKeyOverride->empty()) {
        issueTypeKey = *issueTypeKeyOverride;
    } else {
        issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    const auto it = issueEditMeta_.find(issueId);
    if (it == issueEditMeta_.end() || !it->second.loaded) {
        if (!issueTypeKey.empty()) {
            const auto byType = issueTypeEditMeta_.find(issueTypeKey);
            if (byType != issueTypeEditMeta_.end() && byType->second.loaded) {
                const auto typeFieldIt = byType->second.fieldCanEdit.find(fieldKey);
                if (typeFieldIt == byType->second.fieldCanEdit.end()) {
                    return false;
                }
                return typeFieldIt->second;
            }
        }
        return true;
    }
    const auto fieldIt = it->second.fieldCanEdit.find(fieldKey);
    if (fieldIt == it->second.fieldCanEdit.end()) {
        return false;
    }
    return fieldIt->second;
}

bool AppController::EnsureIssueEditMetaLoaded(const std::string& issueId, std::string* outError,
                                              const std::string* issueTypeKeyOverride) {
    if (outError) {
        outError->clear();
    }
    if (!JiraBackend || issueId.empty()) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto it = issueEditMeta_.find(issueId);
        if (it != issueEditMeta_.end() && it->second.loaded) {
            return true;
        }
    }
    std::string issueTypeKey;
    if (issueTypeKeyOverride && !issueTypeKeyOverride->empty()) {
        issueTypeKey = *issueTypeKeyOverride;
    } else {
        issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    }
    if (!issueTypeKey.empty()) {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto typeIt = issueTypeEditMeta_.find(issueTypeKey);
        if (typeIt != issueTypeEditMeta_.end() && typeIt->second.loaded) {
            issueEditMeta_[issueId] = typeIt->second;
            return true;
        }
    }

    const JiraConfig cfg = ConfigManager::Load();
    std::unordered_map<std::string, bool> meta;
    std::string fetchError;
    const bool ok = JiraBackend->FetchIssueEditMeta(cfg, issueId, meta, fetchError);

    IssueEditMetaCache cache;
    // Only mark loaded after a successful fetch; on failure an empty map with loaded=true made
    // CanEditFieldForIssue deny every field (missing keys) instead of staying optimistic offline.
    cache.loaded = ok;
    if (ok) {
        cache.fieldCanEdit = std::move(meta);
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        issueEditMeta_[issueId] = cache;
        if (ok && !issueTypeKey.empty()) {
            issueTypeEditMeta_[issueTypeKey] = cache;
        }
    }

    if (!ok) {
        LOG_WARN("AppController: editmeta fetch failed issue=%s err=%s", issueId.c_str(), fetchError.c_str());
        if (outError) {
            *outError = fetchError;
        }
    } else {
        requestDeferredLiveJiraBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::RefreshIssueEditMeta(const std::string& issueId, std::string* outError,
                                         const std::string* issueTypeKeyOverride) {
    std::string issueTypeKey;
    if (issueTypeKeyOverride && !issueTypeKeyOverride->empty()) {
        issueTypeKey = *issueTypeKeyOverride;
    } else {
        issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    }
    InvalidateIssueEditMeta(issueId);
    if (!issueTypeKey.empty()) {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        issueTypeEditMeta_.erase(issueTypeKey);
    }
    return EnsureIssueEditMetaLoaded(issueId, outError, &issueTypeKey);
}

void AppController::InvalidateIssueEditMeta(const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    issueEditMeta_.erase(issueId);
}

void AppController::PruneEditMetaCacheToActiveTickets() {
    const auto ticketsSnap = GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    std::unordered_set<std::string> keep;
    std::unordered_set<std::string> keepTypes;
    keep.reserve(tickets.size());
    keepTypes.reserve(tickets.size());
    for (const auto& t : tickets) {
        if (!t.id.empty()) {
            keep.insert(t.id);
        }
        const std::string typeKey = ToLowerAsciiCopy(TrimCopy(t.GetFieldValue("issuetype")));
        if (!typeKey.empty()) {
            keepTypes.insert(typeKey);
        }
    }

    std::lock_guard<std::mutex> lock(editMetaMutex_);
    for (auto it = issueEditMeta_.begin(); it != issueEditMeta_.end();) {
        if (keep.find(it->first) == keep.end()) {
            it = issueEditMeta_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = issueTypeEditMeta_.begin(); it != issueTypeEditMeta_.end();) {
        if (keepTypes.find(it->first) == keepTypes.end()) {
            it = issueTypeEditMeta_.erase(it);
        } else {
            ++it;
        }
    }
}

void AppController::WarmIssueEditMetaAsync(const std::string& issueId) {
    if (!JiraBackend || issueId.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto it = issueEditMeta_.find(issueId);
        if (it != issueEditMeta_.end() && it->second.loaded) {
            return;
        }
        if (issueEditMetaWarmupInFlight_.find(issueId) != issueEditMetaWarmupInFlight_.end()) {
            return;
        }
        issueEditMetaWarmupInFlight_.insert(issueId);
    }

    LaunchBackgroundTask([this, issueId]() {
        std::string ignored;
        EnsureIssueEditMetaLoaded(issueId, &ignored);
        {
            std::lock_guard<std::mutex> lock(editMetaMutex_);
            issueEditMetaWarmupInFlight_.erase(issueId);
        }
    });
}

bool AppController::SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues, std::string& outError) {
    outError.clear();
    if (!Backend || !Cache) {
        outError = "Backend or cache is not initialized.";
        LOG_WARN("AppController::SubmitFieldEdit skipped issue=%s field=%s: %s", issueId.c_str(), field.Id.c_str(),
                 outError.c_str());
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        LOG_WARN("AppController::SubmitFieldEdit skipped field=%s: %s", field.Id.c_str(), outError.c_str());
        return false;
    }

    const std::string fieldEditAuditOp = BackendAuditTrail::MakeOperationId("field-edit");

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    for (const auto& value : rawValues) {
        if (!value.empty()) {
            values.push_back(value);
        }
    }

    const std::shared_ptr<const std::vector<CachedTicket>> ticketsSnap = GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;

    if (IsSprintField(field)) {
        if (!JiraBackend) {
            outError = "Jira backend is not initialized.";
            return false;
        }
        if (values.empty()) {
            outError = "Clearing sprint is not supported by this action.";
            LOG_WARN("AppController::SubmitFieldEdit sprint clear not supported issue=%s field=%s", issueId.c_str(),
                     field.Id.c_str());
            return false;
        }
        const std::string sprintId = values.front();
        JiraConfig cfg = ConfigManager::Load();
        auto ticketIt = std::find_if(tickets.begin(), tickets.end(),
                                     [&](const CachedTicket& ticket) { return ticket.id == issueId; });
        BackendAuditTrail::AppendBegin("field_edit_diff", "ui", issueId, fieldEditAuditOp,
                                       nlohmann::json{{"field_id", field.Id}, {"kind", "sprint"}});
        if (!JiraBackend->AddIssueToSprint(cfg, issueId, sprintId, outError)) {
            LOG_ERROR("AppController::SubmitFieldEdit sprint update failed issue=%s field=%s sprint=%s err=%s",
                      issueId.c_str(), field.Id.c_str(), sprintId.c_str(), outError.c_str());
            BackendAuditTrail::AppendResult(
                "field_edit_diff", "ui", issueId, fieldEditAuditOp, false, outError,
                nlohmann::json{
                    {"field_id", field.Id},
                    {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                    {"after", sprintId}});
            return false;
        }
        if (ticketIt != tickets.end()) {
            CachedTicket updatedTicket = *ticketIt;
            std::string displayValue = sprintId;
            for (const auto& option : field.AllowedValueOptions) {
                if (option.Id == sprintId) {
                    displayValue = option.Value;
                    break;
                }
            }
            updatedTicket.fieldValues[field.Id] = displayValue;
            UpdateTicket(updatedTicket);
        } else {
            RefreshLocalData();
        }
        BackendAuditTrail::AppendResult(
            "field_edit_diff", "ui", issueId, fieldEditAuditOp, true, std::string(),
            nlohmann::json{{"field_id", field.Id},
                           {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                           {"after", values.empty() ? std::string() : values.front()}});
        requestDeferredLiveJiraBackendSuccessNotify_();
        return true;
    }

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outError = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        LOG_WARN("AppController::SubmitFieldEdit blocked non-editable timetracking issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    auto ticketIt =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
        const std::string editedValue = values.empty() ? std::string() : values.front();
        if (editedValue.empty()) {
            outError = "Clearing Jira timetracking estimates is not supported by this editor.";
            LOG_WARN("AppController::SubmitFieldEdit blocked timetracking clear issue=%s field=%s", issueId.c_str(),
                     field.Id.c_str());
            return false;
        }

        std::string originalEstimate =
            (ticketIt != tickets.end()) ? ticketIt->GetFieldValue("timeoriginalestimate") : std::string();
        std::string remainingEstimate =
            (ticketIt != tickets.end()) ? ticketIt->GetFieldValue("timeestimate") : std::string();
        const std::string beforeOriginalEstimate = originalEstimate;
        const std::string beforeRemainingEstimate = remainingEstimate;
        if (field.Id == "timeoriginalestimate") {
            originalEstimate = editedValue;
        } else {
            remainingEstimate = editedValue;
        }

        nlohmann::json timetrackingPayload = nlohmann::json::object();
        if (!originalEstimate.empty()) {
            timetrackingPayload["originalEstimate"] = originalEstimate;
        }
        if (!remainingEstimate.empty()) {
            timetrackingPayload["remainingEstimate"] = remainingEstimate;
        }
        if (timetrackingPayload.empty()) {
            outError = "Timetracking update requires at least one estimate value.";
            return false;
        }

        nlohmann::json fieldsPayload = nlohmann::json::object();
        fieldsPayload["timetracking"] = std::move(timetrackingPayload);
        BackendAuditTrail::AppendBegin("field_edit_diff", "ui", issueId, fieldEditAuditOp,
                                       nlohmann::json{{"field_id", "timetracking"}, {"kind", "timetracking"}});
        if (!Backend->UpdateIssueFields(issueId, fieldsPayload, outError)) {
            std::string payloadForLog;
            try {
                payloadForLog = fieldsPayload.dump();
            } catch (...) {
                payloadForLog = "(payload dump failed)";
            }
            LOG_ERROR("AppController::SubmitFieldEdit failed issue=%s field=%s tracker_error=%s request=%s",
                      issueId.c_str(), field.Id.c_str(), outError.c_str(), TruncateForLog(payloadForLog, 1200).c_str());
            BackendAuditTrail::AppendResult(
                "field_edit_diff", "ui", issueId, fieldEditAuditOp, false, outError,
                nlohmann::json{{"field_id", "timetracking"},
                               {"before", nlohmann::json{{"timeoriginalestimate", beforeOriginalEstimate},
                                                         {"timeestimate", beforeRemainingEstimate}}},
                               {"after", fieldsPayload["timetracking"]}});
            return false;
        }

        if (ticketIt != tickets.end()) {
            CachedTicket updatedTicket = *ticketIt;
            updatedTicket.fieldValues["timeoriginalestimate"] = originalEstimate;
            updatedTicket.fieldValues["timeestimate"] = remainingEstimate;
            UpdateTicket(updatedTicket);
        } else {
            RefreshLocalData();
        }
        BackendAuditTrail::AppendResult(
            "field_edit_diff", "ui", issueId, fieldEditAuditOp, true, std::string(),
            nlohmann::json{{"field_id", "timetracking"},
                           {"before", nlohmann::json{{"timeoriginalestimate", beforeOriginalEstimate},
                                                     {"timeestimate", beforeRemainingEstimate}}},
                           {"after", fieldsPayload["timetracking"]}});
        requestDeferredLiveJiraBackendSuccessNotify_();
        return true;
    }

    if (JiraBackend && !IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id)) {
        EnsureIssueEditMetaLoaded(issueId, nullptr);
    }

    if (JiraBackend && !IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id) &&
        !CanEditFieldForIssue(issueId, field.Id, &field)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        LOG_WARN("AppController::SubmitFieldEdit blocked by editmeta issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    nlohmann::json valuePayload;
    std::string buildErr;
    const JiraField jiraField = JiraTrackerFieldAdapter::ToJiraField(field);
    if (!JiraFieldPayload::BuildValue(jiraField, rawValues, valuePayload, buildErr)) {
        outError = buildErr.empty() ? std::string("Invalid field value.") : buildErr;
        LOG_WARN("AppController::SubmitFieldEdit invalid value issue=%s field=%s err=%s", issueId.c_str(),
                 field.Id.c_str(), outError.c_str());
        return false;
    }

    nlohmann::json fieldsPayload = nlohmann::json::object();
    fieldsPayload[field.Id] = valuePayload;
    BackendAuditTrail::AppendBegin("field_edit_diff", "ui", issueId, fieldEditAuditOp,
                                   nlohmann::json{{"field_id", field.Id}, {"kind", "issue_fields"}});
    bool updateOk = Backend->UpdateIssueFields(issueId, fieldsPayload, outError);
    bool didRetryAfter400 = false;
    if (!updateOk && JiraBackend && ErrorTextContainsHttpStatus(outError, 400)) {
        didRetryAfter400 = true;
        RefreshIssueEditMeta(issueId, nullptr);
        if (!CanEditFieldForIssue(issueId, field.Id, &field)) {
            outError = "Field cannot be edited for this issue (Jira edit metadata refreshed after validation failure).";
            LOG_WARN("AppController::SubmitFieldEdit blocked after editmeta refresh issue=%s field=%s", issueId.c_str(),
                     field.Id.c_str());
            BackendAuditTrail::AppendResult(
                "field_edit_diff", "ui", issueId, fieldEditAuditOp, false, outError,
                nlohmann::json{
                    {"field_id", field.Id},
                    {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                    {"after", rawValues}});
            return false;
        }
        updateOk = Backend->UpdateIssueFields(issueId, fieldsPayload, outError);
    }
    if (!updateOk) {
        std::string payloadForLog;
        try {
            payloadForLog = fieldsPayload.dump();
        } catch (...) {
            payloadForLog = "(payload dump failed)";
        }
        LOG_ERROR(
            "AppController::SubmitFieldEdit failed issue=%s field=%s retried_after_400=%d tracker_error=%s request=%s",
            issueId.c_str(), field.Id.c_str(), didRetryAfter400 ? 1 : 0, outError.c_str(),
            TruncateForLog(payloadForLog, 1200).c_str());
        BackendAuditTrail::AppendResult(
            "field_edit_diff", "ui", issueId, fieldEditAuditOp, false, outError,
            nlohmann::json{{"field_id", field.Id},
                           {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                           {"after", rawValues}});
        return false;
    }

    // Keep local cache and in-memory model in sync with the successful Jira update.
    if (ticketIt != tickets.end()) {
        CachedTicket updatedTicket = *ticketIt;

        std::string displayValue;
        if (!values.empty()) {
            for (size_t i = 0; i < values.size(); ++i) {
                const std::string displayPart =
                    JiraFieldPayload::ResolveDisplayValueForSubmittedSelection(jiraField, values[i]);
                if (i != 0) {
                    displayValue += ", ";
                }
                displayValue += displayPart;
            }
        }

        updatedTicket.fieldValues[field.Id] = displayValue;
        UpdateTicket(updatedTicket);
        BackendAuditTrail::AppendResult("field_edit_diff", "ui", issueId, fieldEditAuditOp, true, std::string(),
                                        nlohmann::json{{"field_id", field.Id},
                                                       {"before", ticketIt->GetFieldValue(field.Id)},
                                                       {"after", displayValue}});
    } else {
        RefreshLocalData();
        BackendAuditTrail::AppendResult(
            "field_edit_diff", "ui", issueId, fieldEditAuditOp, true, std::string(),
            nlohmann::json{{"field_id", field.Id}, {"before", "unknown"}, {"after", rawValues}});
    }

    requestDeferredLiveJiraBackendSuccessNotify_();
    return true;
}

bool AppController::SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& rawValues,
                                               const std::string& originalEstimateSnapshot,
                                               const std::string& remainingEstimateSnapshot,
                                               const std::string& issueTypeKeySnapshot, FieldEditResult& outResult) {
    outResult = FieldEditResult{};
    if (issueId.empty()) {
        outResult.Error = "Issue id is empty.";
        return false;
    }

    JiraClient localClient;
    JiraConfig cfg = ConfigManager::Load();
    std::vector<std::string> values;
    values.reserve(rawValues.size());
    for (const auto& value : rawValues) {
        if (!value.empty()) {
            values.push_back(value);
        }
    }

    if (IsSprintField(field)) {
        if (values.empty()) {
            outResult.Error = "Clearing sprint is not supported by this action.";
            return false;
        }
        const std::string sprintId = values.front();
        if (!localClient.AddIssueToSprint(cfg, issueId, sprintId, outResult.Error)) {
            return false;
        }
        std::string displayValue = sprintId;
        for (const auto& option : field.AllowedValueOptions) {
            if (option.Id == sprintId) {
                displayValue = option.Value;
                break;
            }
        }
        outResult.Ok = true;
        outResult.UpdatedDisplayValues[field.Id] = std::move(displayValue);
        requestDeferredLiveJiraBackendSuccessNotify_();
        return true;
    }

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outResult.Error = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        return false;
    }

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
        const std::string editedValue = values.empty() ? std::string() : values.front();
        if (editedValue.empty()) {
            outResult.Error = "Clearing Jira timetracking estimates is not supported by this editor.";
            return false;
        }
        std::string originalEstimate = originalEstimateSnapshot;
        std::string remainingEstimate = remainingEstimateSnapshot;
        if (field.Id == "timeoriginalestimate") {
            originalEstimate = editedValue;
        } else {
            remainingEstimate = editedValue;
        }

        nlohmann::json timetrackingPayload = nlohmann::json::object();
        if (!originalEstimate.empty()) {
            timetrackingPayload["originalEstimate"] = originalEstimate;
        }
        if (!remainingEstimate.empty()) {
            timetrackingPayload["remainingEstimate"] = remainingEstimate;
        }
        if (timetrackingPayload.empty()) {
            outResult.Error = "Timetracking update requires at least one estimate value.";
            return false;
        }

        nlohmann::json fieldsPayload = nlohmann::json::object();
        fieldsPayload["timetracking"] = std::move(timetrackingPayload);
        if (!localClient.UpdateIssueFields(issueId, fieldsPayload, outResult.Error)) {
            return false;
        }
        outResult.Ok = true;
        outResult.UpdatedDisplayValues["timeoriginalestimate"] = std::move(originalEstimate);
        outResult.UpdatedDisplayValues["timeestimate"] = std::move(remainingEstimate);
        requestDeferredLiveJiraBackendSuccessNotify_();
        return true;
    }

    const std::string* issueTypeKeyOpt = issueTypeKeySnapshot.empty() ? nullptr : &issueTypeKeySnapshot;

    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    if (!TryBuildFieldEditPayloadForNetwork(issueId, field, rawValues, originalEstimateSnapshot,
                                            remainingEstimateSnapshot, issueTypeKeySnapshot, fieldsPayload,
                                            displayValues, outResult.Error)) {
        return false;
    }

    bool updateOk = localClient.UpdateIssueFields(issueId, fieldsPayload, outResult.Error);
    bool didRetryAfter400 = false;
    if (!updateOk && JiraBackend && ErrorTextContainsHttpStatus(outResult.Error, 400)) {
        didRetryAfter400 = true;
        RefreshIssueEditMeta(issueId, nullptr, issueTypeKeyOpt);
        if (!CanEditFieldForIssue(issueId, field.Id, &field, issueTypeKeyOpt)) {
            outResult.Error =
                "Field cannot be edited for this issue (Jira edit metadata refreshed after validation failure).";
            LOG_WARN("AppController::SubmitFieldEditNetworkOnly blocked after editmeta refresh issue=%s field=%s",
                     issueId.c_str(), field.Id.c_str());
            return false;
        }
        updateOk = localClient.UpdateIssueFields(issueId, fieldsPayload, outResult.Error);
    }
    if (!updateOk) {
        std::string payloadForLog;
        try {
            payloadForLog = fieldsPayload.dump();
        } catch (...) {
            payloadForLog = "(payload dump failed)";
        }
        LOG_ERROR("AppController::SubmitFieldEditNetworkOnly failed issue=%s field=%s retried_after_400=%d "
                  "tracker_error=%s request=%s",
                  issueId.c_str(), field.Id.c_str(), didRetryAfter400 ? 1 : 0, outResult.Error.c_str(),
                  TruncateForLog(payloadForLog, 1200).c_str());
        return false;
    }

    outResult.Ok = true;
    outResult.UpdatedDisplayValues = std::move(displayValues);
    requestDeferredLiveJiraBackendSuccessNotify_();
    return true;
}

bool AppController::TryPrepareOfflineFieldEdit(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& rawValues,
                                               const std::string& originalEstimateSnapshot,
                                               const std::string& remainingEstimateSnapshot,
                                               const std::string& issueTypeKeySnapshot, FieldEditResult& outResult,
                                               std::string& outFieldsPayloadJson, std::string& outError) {
    outResult = FieldEditResult{};
    outFieldsPayloadJson.clear();
    outError.clear();
    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    if (!TryBuildFieldEditPayloadForNetwork(issueId, field, rawValues, originalEstimateSnapshot,
                                            remainingEstimateSnapshot, issueTypeKeySnapshot, fieldsPayload,
                                            displayValues, outError)) {
        return false;
    }
    try {
        outFieldsPayloadJson = fieldsPayload.dump();
    } catch (const std::exception& ex) {
        outError = ex.what();
        return false;
    } catch (...) {
        outError = "Failed to serialize field payload.";
        return false;
    }
    outResult.Ok = true;
    outResult.UpdatedDisplayValues = std::move(displayValues);
    return true;
}

bool AppController::ApplyFieldEditResult(const std::string& issueId, const FieldEditResult& result,
                                         std::string& outError) {
    outError.clear();
    if (!result.Ok) {
        outError = result.Error.empty() ? std::string("Failed to save field update.") : result.Error;
        return false;
    }
    if (!Cache) {
        outError = "Cache is not initialized.";
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }

    const auto ticketsSnapApply = GetActiveTicketsSnapshot();
    const auto& ticketsApply = *ticketsSnapApply;
    auto ticketIt = std::find_if(ticketsApply.begin(), ticketsApply.end(),
                                 [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (ticketIt == ticketsApply.end()) {
        RefreshLocalData();
        return true;
    }

    CachedTicket updatedTicket = *ticketIt;
    for (const auto& pair : result.UpdatedDisplayValues) {
        updatedTicket.fieldValues[pair.first] = pair.second;
    }
    UpdateTicket(updatedTicket);
    return true;
}

bool AppController::FetchIssueWatchers(const std::string& issueKey, std::vector<JiraUser>& outWatchers,
                                       std::string& outError) const {
    outWatchers.clear();
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->FetchIssueWatchers(cfg, issueKey, outWatchers, outError);
    if (!ok) {
        LOG_ERROR("AppController::FetchIssueWatchers failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveJiraBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::JiraSearchUsersByQuery(const std::string& query, std::vector<JiraUser>& outUsers,
                                           std::string& outError) const {
    outUsers.clear();
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->SearchUsersByQuery(cfg, query, outUsers, outError);
    if (!ok) {
        LOG_ERROR("AppController::JiraSearchUsersByQuery failed query=%s err=%s", TruncateForLog(query, 120).c_str(),
                  outError.c_str());
    } else {
        requestDeferredLiveJiraBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::JiraAddIssueCommentPlain(const std::string& issueKey, const std::string& plainText,
                                             std::string& outError) {
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->AddIssueCommentPlain(cfg, issueKey, plainText, outError);
    if (!ok) {
        LOG_ERROR("AppController::JiraAddIssueCommentPlain failed issue=%s err=%s", issueKey.c_str(), outError.c_str());
    } else {
        requestDeferredLiveJiraBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::JiraAddIssueCommentBlameContext(const std::string& issueKey, const std::string& p4User,
                                                    const std::string& functionName, const std::string& filePath,
                                                    const int lineNumber, const std::string& changelist,
                                                    const std::string& date, const bool approximated,
                                                    const std::string& codeSnippet, std::string& outError) {
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->AddIssueCommentBlameContext(cfg, issueKey, p4User, functionName, filePath, lineNumber,
                                                             changelist, date, approximated, codeSnippet, outError);
    if (!ok) {
        LOG_ERROR("AppController::JiraAddIssueCommentBlameContext failed issue=%s err=%s", issueKey.c_str(),
                  outError.c_str());
    } else {
        requestDeferredLiveJiraBackendSuccessNotify_();
    }
    return ok;
}

bool AppController::JiraFetchUserGroupNames(const std::string& accountId, std::vector<std::string>& outGroupNames,
                                            std::string& outError) const {
    outGroupNames.clear();
    outError.clear();
    if (!JiraBackend) {
        outError = "Jira backend is not initialized.";
        return false;
    }
    const JiraConfig cfg = ConfigManager::Load();
    const bool ok = JiraBackend->FetchUserGroupNames(cfg, accountId, outGroupNames, outError);
    if (!ok) {
        LOG_ERROR("AppController::JiraFetchUserGroupNames failed account=%s err=%s",
                  TruncateForLog(accountId, 40).c_str(), outError.c_str());
    } else {
        requestDeferredLiveJiraBackendSuccessNotify_();
    }
    return ok;
}
