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
#include "GridContextDepsAdapter.h"

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

#include "BackendAuditTrail.h"
#include "BackgroundWorkerReap.h"
#include "ConfigManager.h"

#include "Commands/BuiltinCommands.h"

#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/SmatchetScenarioRegistry.h"

#include "FieldCatalogCache.h"

#include "Ui/SmatchetFieldRender.h"

#include <ghc/filesystem.hpp>

#include "DefaultTrackerBackendFactory.h"

#include "GitHubFixtureBackend.h"

#include "ITrackerBackendFactory.h"

#include "PlaneFixtureBackend.h"

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
#include "SmatchetMergeWatchNotifyServer.h"

#include "AiTypes.h"
#include "ConfigSaveWorker.h" // not AI-gated — config saves happen regardless of feature flags
#if defined(SMATCHET_WITH_AI)
#include "AiAssistantController.h"
#include "SmatchetChatPersistWorker.h"
#include "SmatchetUiSession.h"
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

// Only referenced from the SMATCHET_WITH_LUA_AUTOMATION init path — guard the
// definition too, or Lua-off configs (UBSan clang job) fail -Werror,-Wunused-function.
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
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
#endif // SMATCHET_WITH_LUA_AUTOMATION

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

// True when absStr is inside the Lua scripts directory or the runtime-asset directory.
// Both roots are weakly-canonicalised before the case-insensitive prefix compare.
bool FieldIconPathIsAllowed(const std::string& absStr, const std::string& luaScriptsDirectory) {
    namespace fs = ghc::filesystem;
    std::error_code ec;

    if (!luaScriptsDirectory.empty()) {

        const fs::path scriptsRoot = fs::weakly_canonical(fs::path(luaScriptsDirectory), ec);

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
}

} // namespace

void AppController::SetBackendFactory(std::unique_ptr<ITrackerBackendFactory> factory) {
    backendFactory_ = std::move(factory);
}

// Out-of-line definition: map<int,...>::find binds kDefaultPaneId to a const int&
// (ODR-use), so the in-class declaration alone is not enough in C++14.
const int AppController::kDefaultPaneId = 0;

AppController::AppController() {
    // Multi-grid Slice 1 (ADR-0018): exactly one live context. Created here — not lazily —
    // so focusedContext() is valid for the controller's entire lifetime (delegators, the
    // deps adapter, and the destructor all assume the entry exists).
    gridContexts_[kDefaultPaneId] = std::make_unique<GridLiveContext>();
}

AppController::~AppController() {

    // Shutdown ordering matters here — every background thread that can post to mainThreadDispatcher
    // or read `this` via __smatchet_app must be joined BEFORE member destruction begins. This
    // matches the contract described in MainThreadDispatcher.h and AppController_LuaBindings.cpp.

    // Phase 4b of docs/plans/shipped/smatchet-merge-watcher.md — stop the merge-watch
    // HTTP server FIRST. Its listen thread queues toast-append lambdas via
    // mainThreadDispatcher; the join here guarantees no late post races
    // BeginShutdown() below.
    if (mergeWatchNotifyServer_) {
        mergeWatchNotifyServer_->Stop();
        mergeWatchNotifyServer_.reset();
    }
#if defined(SMATCHET_WITH_AI)
    // Drop the AI assistant first — its worker may still be inside SendStreaming. The
    // dtor flips the cancel atom + joins; only after the join can the main-thread
    // dispatcher safely BeginShutdown(), because callbacks already in flight will
    // hand off through `mainThreadDispatcher.PostToMainThread` (still accepting posts
    // at this instant) and the controller's join blocks until those callbacks return.
    aiAssistant_.reset();
    // Phase 3 of ai-chat-claude-desktop-parity. Stop the chat-persist worker after
    // the AI assistant finishes (no more new Enqueue calls from a streaming callback)
    // but BEFORE `mainThreadDispatcher.BeginShutdown()` and before LCM destructs at
    // member-destruction time. Stop drains pending ops within 250 ms then joins the
    // worker thread. Pillar 3 — no detached thread may outlive `Cache`.
    smatchet::ai::chat_persist::Stop();
#endif

    // Stop the coalescing config-save worker: flush pending config writes within a bounded budget,
    // then join. Not AI-gated — config saves happen regardless of feature flags. After this, any
    // further config save falls back to a synchronous write on the caller.
    smatchet::config_save::Stop();

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

    const GridLiveContext& ctx = focusedContext();

    std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);

    if (!ctx.activeTicketsPublished_) {

        ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
    }

    return ctx.activeTicketsPublished_;
}

std::vector<CachedTicket> AppController::GetActiveTickets() const {

    const GridLiveContext& ctx = focusedContext();

    std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);

    if (!ctx.activeTicketsPublished_) {

        ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
    }

    return *ctx.activeTicketsPublished_;
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

    LaunchBackgroundTask([this, toFetch]() { FetchAndCachePrefetchedTickets(toFetch); });
}

void AppController::FetchAndCachePrefetchedTickets(const std::vector<std::string>& toFetch) {
    // Latch a strong handle via atomic_load: this worker reads Backend off the UI thread,
    // which would race a live SetBackend swap on a plain .get(). The shared_ptr also keeps
    // the backend alive for the FetchIssuesForKeys call (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&focusedContext().Backend);

    if (!backend) {

        return;
    }

    TrackerConfig cfg = ConfigManager::Load();

    ViewsStore views = ConfigManager::LoadViewsOrBootstrap(cfg);

    std::string err;

    std::vector<CachedTicket> tickets;

    const bool ok = backend->Reader().FetchIssuesForKeys(cfg, toFetch, views, tickets, err);

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

    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([this, task = std::move(task), done]() mutable {
        if (!shuttingDown_.load()) {
            task();
        }
        // Mark complete LAST so the pool's reap (which joins only done workers) sees a thread
        // that's past task() — join() then returns essentially immediately.
        done->store(true, std::memory_order_release);
    });

    std::lock_guard<std::mutex> lock(backgroundWorkersMutex_);
    // Amortized mid-session reap: join + drop any previously-finished workers so the vector
    // tracks only in-flight (plus just-finished) work, not every task ever spawned.
    reapFinishedBackgroundWorkersLocked_();
    backgroundWorkers_.push_back(BackgroundWorker{std::move(worker), std::move(done)});
}

void AppController::reapFinishedBackgroundWorkersLocked_() {
    const std::thread::id selfId = std::this_thread::get_id();
    smatchet::ReapFinishedWorkers(
        backgroundWorkers_,
        [&](const BackgroundWorker& w) {
            // Reapable = task finished (done set) AND not the calling worker itself (never
            // self-join; leave that one for JoinBackgroundTasks at shutdown).
            const bool finished = w.done && w.done->load(std::memory_order_acquire);
            const bool isSelf = w.thread.joinable() && w.thread.get_id() == selfId;
            return finished && !isSelf;
        },
        [](BackgroundWorker& w) {
            if (w.thread.joinable()) {
                w.thread.join(); // done flag set → returns immediately
            }
        });
}

void AppController::RetireBackend(std::shared_ptr<ITrackerBackend> old) {
    // Defer-free (ADR 0012): keep a swapped-out backend alive until shutdown so raw subobject
    // pointers (Reader/Mutations/Connectivity) captured by in-flight workers before the live
    // tracker swap can't dangle. Drained by ~retiredBackends_ in ~AppController, after every
    // worker has been joined via JoinBackgroundTasks.
    if (!old) {
        return;
    }
    std::lock_guard<std::mutex> lk(retiredBackendsMutex_);
    retiredBackends_.push_back(std::move(old));
}

void AppController::JoinBackgroundTasks() {

    std::vector<BackgroundWorker> workers;

    {

        std::lock_guard<std::mutex> lock(backgroundWorkersMutex_);

        workers = std::move(backgroundWorkers_);
    }

    const std::thread::id selfId = std::this_thread::get_id();

    for (auto& worker : workers) {

        if (!worker.thread.joinable()) {

            continue;
        }

        if (worker.thread.get_id() == selfId) {
            // A background worker called JoinBackgroundTasks — cannot self-join.
            // Re-queue so ~AppController can join it from the main thread.
            // Detaching was the previous behaviour but let detached threads outlive
            // the controller and race against g_TrackerIssueFetchMutex teardown.
            LOG_WARN("AppController::JoinBackgroundTasks: self-join detected; re-queuing thread for main-thread join.");
            std::lock_guard<std::mutex> requeue_lock(backgroundWorkersMutex_);
            backgroundWorkers_.push_back(std::move(worker));
            continue;
        }

        worker.thread.join();
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
    // Thin bootstrap sequencer (decompose-top-20-monoliths). Each phase runs in
    // strict order; the ordering, error handling, and early-returns are identical
    // to the former 448-line monolith. `cfg` and `activeTrackerType` are the only
    // values that cross phase boundaries — InitBackends publishes both.
    InitConfig(dbPath, backendType);

    TrackerConfig cfg{};
    const std::string activeTrackerType = InitBackends(cfg);

    InitFieldCatalog(cfg, activeTrackerType);
    InitPlugins(activeTrackerType);
    InitCommands();
}

void AppController::InitConfig(const std::string& dbPath, const std::string& backendType) {
    // Record the UI thread identity first. Initialize() is invoked from main() before any
    // background thread is spawned, so this happens-before any worker that could call
    // IsOnUiThread() later. See AppController.h for the full reasoning.
    uiThreadId_ = std::this_thread::get_id();

    LOG_INFO("AppController::Initialize backendType=%s dbPath=%s", backendType.c_str(), dbPath.c_str());

    // Start the coalescing config-save worker before anything can enqueue a save. No deps (no
    // dispatcher/LCM); Stop()+join runs early in ~AppController. Not AI-gated.
    smatchet::config_save::Start();

    localCacheDbPath_ = dbPath;

    Cache = std::make_unique<LocalCacheManager>(dbPath);

#if defined(SMATCHET_WITH_AI)
    // Phase 3 of ai-chat-claude-desktop-parity. Start the single coalescing
    // chat-persist worker now that the LCM is live; Stop() runs in ~AppController
    // BEFORE any member destructs so the worker thread is joined while LCM, the
    // dispatcher, and `g_ui` are still valid. The on-append callback runs on the
    // UI thread via `mainThreadDispatcher.PostToMainThread`; it backfills the
    // parallel `assistantHistoryRowIds` so subsequent pin-toggle ops have a row
    // id to flip. Guard against shrink/clear races by re-checking the index
    // against the current vector size — clear-chat may have run between the
    // worker's INSERT and the dispatcher drain.
    smatchet::ai::chat_persist::Start(*Cache, mainThreadDispatcher, [](std::size_t idx, std::int64_t rowId) {
        if (idx < g_ui.assistantHistoryRowIds.size()) {
            g_ui.assistantHistoryRowIds[idx] = rowId;
        }
    });
#endif

    // Construct the deps adapter eagerly so OfflineQueueService + TicketSyncService can capture
    // an interface reference at construction time. The adapter is owned by this AppController
    // and outlives both services (per the destructor ordering: ~AppController joins the
    // streaming-sync worker via `CancelAndJoinActiveStreamingSync` before any member is
    // destroyed, so the adapter is live for every `deps_.X` call).
    if (!depsAdapter_) {
        depsAdapter_ = std::make_unique<GridContextDepsAdapter>(*this, focusedContext());
    }
    // Construct OfflineQueueService eagerly so the legacy-pending startup migration below
    // can write to `offlineQueue_->legacyPendingStartupBanner_` (item 12 extraction).
    if (!offlineQueue_) {
        offlineQueue_ = std::make_unique<OfflineQueueService>(*depsAdapter_);
    }
    // Construct TicketSyncService alongside — its `CancelAndJoinActiveStreamingSync` is called
    // by `RecreateLocalCacheDatabase` (which the legacy-pending cleanup below may trigger),
    // so the service must exist before that path runs (item 11 extraction).
    if (!focusedContext().ticketSync_) {
        focusedContext().ticketSync_ = std::make_unique<TicketSyncService>(*depsAdapter_);
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
}

std::string AppController::InitBackends(TrackerConfig& cfgOut) {
    TrackerConfig cfg = ConfigManager::Load();

    std::string activeTracker = cfg.TrackerType;

    if (activeTracker.empty()) {

        activeTracker = "Jira";
    }

    // Slice 2 of docs/plans/shipped/autonomous-debugging-no-creds.md — env-hook
    // override for the Plane backend. When SMATCHET_TEST_PLANE_BACKEND_FIXTURE
    // is set, swap in a fixture-driven backend factory before the default is
    // constructed. Sibling Jira/GitHub hooks land adjacent.
    if (!backendFactory_) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform read-only; _dupenv_s is MSVC-only
#endif
        const char* planeFixtureEnv = std::getenv("SMATCHET_TEST_PLANE_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (planeFixtureEnv && planeFixtureEnv[0] != '\0') {
            LOG_INFO("AppController: SMATCHET_TEST_PLANE_BACKEND_FIXTURE=%s — installing PlaneFixtureBackend factory.",
                     planeFixtureEnv);
            backendFactory_ = smatchet::plane::MakePlaneFixtureBackendFactory(std::string(planeFixtureEnv));
            activeTracker = "Plane";
            cfg.TrackerType = activeTracker;
        }
    }

    MaybeInstallGitHubFixtureFactory(activeTracker);

    if (!backendFactory_) {
        backendFactory_ = std::make_unique<DefaultTrackerBackendFactory>();
    }
    // atomic_store: off-thread workers read Backend via std::atomic_load (ADR 0012); a plain
    // assignment would data-race those reads on the shared_ptr instance (C++14).
    GridLiveContext& ctx = focusedContext();
    std::atomic_store(&ctx.Backend, std::shared_ptr<ITrackerBackend>(backendFactory_->Create(activeTracker)));
    if (!ctx.Backend) {
        LOG_ERROR("AppController: tracker backend factory returned null for type '%s'.", activeTracker.c_str());
    } else {
        LOG_INFO("AppController: %s backend initialized.", ctx.Backend->Connectivity().GetTrackerType().c_str());
    }

    const std::string activeTrackerType = ctx.Backend ? ctx.Backend->Connectivity().GetTrackerType() : "Unknown";

    RunLegacyStartupSweeps(activeTrackerType);

    // Publish the resolved config, which an env hook may have mutated, so the next phase
    // builds its field-catalog cache key from the exact TrackerConfig the backend used.
    cfgOut = cfg;
    return activeTrackerType;
}

void AppController::MaybeInstallGitHubFixtureFactory(const std::string& activeTracker) {
    // Slice 1 of docs/plans/shipped/autonomous-debugging-no-creds.md — env-hook to
    // swap the default tracker factory for a fixture-backed GitHub backend
    // when SMATCHET_TEST_GITHUB_BACKEND_FIXTURE=<path> is set AND the active
    // tracker is GitHub. Keeps the no-credentials debug loop able to drive
    // scenarios against a deterministic ticket set without consulting the PAT.
    if (backendFactory_) {
        return;
    }
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform read-only; _dupenv_s is MSVC-only
#endif
    const char* githubFixture = std::getenv("SMATCHET_TEST_GITHUB_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (!githubFixture) {
        return;
    }
    const std::string fixturePath(githubFixture);
    if (fixturePath.empty()) {
        return;
    }
    const std::string lowerActive = ToLowerAsciiCopy(activeTracker);
    if (lowerActive != "github") {
        LOG_WARN("AppController: SMATCHET_TEST_GITHUB_BACKEND_FIXTURE set but active "
                 "tracker is '%s', not 'GitHub' — ignoring fixture override.",
                 activeTracker.c_str());
        return;
    }
    LOG_INFO("AppController: SMATCHET_TEST_GITHUB_BACKEND_FIXTURE=%s — installing "
             "fixture-backed GitHub factory (no HTTP, no PAT lookup).",
             fixturePath.c_str());
    class FixtureGitHubFactory : public ITrackerBackendFactory {
      public:
        explicit FixtureGitHubFactory(const std::string& path) : path_(path) {}
        std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType) override {
            const std::string lower = ToLowerAsciiCopy(trackerType);
            if (lower == "github") {
                return std::make_unique<smatchet::github::GitHubFixtureBackend>(path_, std::string(), std::string(),
                                                                                /*includePullRequests=*/true);
            }
            // Non-GitHub backends fall through to the default factory shape.
            DefaultTrackerBackendFactory fallback;
            return fallback.Create(trackerType);
        }

      private:
        std::string path_;
    };
    backendFactory_ = std::make_unique<FixtureGitHubFactory>(fixturePath);
}

void AppController::RunLegacyStartupSweeps(const std::string& activeTrackerType) {
    // PR 5 of docs/plans/shipped/remove-global-project-key.md: one-shot legacy-project sweeps.
    // Drain legacy global project state into per-entity carriers (offline-queue payloads,
    // Plane view query JSON). Each sweep is guarded by its own `cache_meta` flag so it runs
    // exactly once per database file; subsequent launches are no-ops.
    if (offlineQueue_) {
        try {
            // Legacy carriers were removed from TrackerConfig. Pass empty values; the sweep's
            // `legacy_project_swept_v1` cache_meta marker short-circuits on already-migrated installs.
            offlineQueue_->RunLegacyProjectSweep(std::string(), std::string(), activeTrackerType);
        } catch (const std::exception& ex) {
            LOG_ERROR("AppController::Initialize legacy-project offline sweep failed: %s", ex.what());
        }
    }
    if (focusedContext().Backend && Cache && activeTrackerType == "Plane") {
        try {
            static const std::string kPlaneSweepFlag = "legacy_plane_view_swept_v1";
            if (!Cache->HasCacheMetaFlag(kPlaneSweepFlag)) {
                // The legacy `plane_project_id` carrier was removed. Walk the views once to log
                // any that still lack project scope, then set the marker so we never look again.
                PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
                const std::string backendKey = ConfigManager::NormalizeViewsBackendKey("Plane");
                auto bucketIt = disk.Backends.find(backendKey);
                if (bucketIt != disk.Backends.end()) {
                    for (const ViewDefinition& view : bucketIt->second.Views) {
                        const std::string extracted =
                            focusedContext().Backend->Connectivity().ExtractProjectFromQuery(view.Jql);
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
}

void AppController::InitFieldCatalog(const TrackerConfig& cfg, const std::string& activeTrackerType) {
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

        std::string projectKeyForCache = ResolveActiveViewProjectKeyForCatalog(activeTrackerType);
        std::string cacheKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, projectKeyForCache);
        if (!projectKeyForCache.empty() && !FieldCatalogCache::TryLoadFieldCatalogSnapshot(
                                               cacheKey, snapFields, snapComponents, snapIssueTypeMeta, snapErr)) {
            // No project-scoped snapshot yet (first run with this project, or it was evicted).
            // Fall back to the unscoped key so we still restore *some* catalog offline; the grid's
            // scoped fetch repopulates the project entry on next draw.
            projectKeyForCache.clear();
            cacheKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, projectKeyForCache);
        }

        if (FieldCatalogCache::TryLoadFieldCatalogSnapshot(cacheKey, snapFields, snapComponents, snapIssueTypeMeta,
                                                           snapErr)) {
            ApplyStartupFieldCatalogSnapshot(std::move(snapFields), std::move(snapComponents),
                                             std::move(snapIssueTypeMeta), activeTrackerType);
        }
    }
}

std::string AppController::ResolveActiveViewProjectKeyForCatalog(const std::string& activeTrackerType) const {
    // Resolve the active view's project from its JQL so the startup load hits the
    // project-scoped snapshot (which carries Phase-3 component options). Falls back to the
    // unscoped ("") key when no project resolves (filter-id / cross-project / non-`project=`
    // JQL). Mirrors the grid's StartFieldCatalogFetchAsync scoping so the two key spaces agree.
    std::string projectKeyForCache;
    if (!focusedContext().Backend) {
        return projectKeyForCache;
    }
    try {
        const PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
        const std::string backendKey = ConfigManager::NormalizeViewsBackendKey(activeTrackerType);
        const auto bucketIt = disk.Backends.find(backendKey);
        if (bucketIt != disk.Backends.end()) {
            const ViewWorkspaceState& bucket = bucketIt->second;
            const ViewDefinition* activeView = nullptr;
            for (const ViewDefinition& view : bucket.Views) {
                if (view.Id == bucket.ActiveViewId) {
                    activeView = &view;
                    break;
                }
            }
            if (activeView == nullptr && !bucket.Views.empty()) {
                activeView = &bucket.Views.front();
            }
            if (activeView != nullptr) {
                projectKeyForCache = focusedContext().Backend->Connectivity().ExtractProjectFromQuery(activeView->Jql);
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("AppController::Initialize: active-view project resolve for catalog snapshot failed: %s", ex.what());
        projectKeyForCache.clear();
    }
    return projectKeyForCache;
}

void AppController::ApplyStartupFieldCatalogSnapshot(std::vector<TrackerField> snapFields,
                                                     std::vector<TrackerComponent> snapComponents,
                                                     std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta,
                                                     const std::string& activeTrackerType) {
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
    LOG_INFO("AppController::Initialize: restored field catalog from snapshot (%zu fields)", AvailableFields.size());
}

void AppController::InitPlugins(const std::string& activeTrackerType) {
    TrackerConfig jiraCfgForEditMetaWarmup{};

    if (activeTrackerType == "Jira") {

        // Load before InitLua(): avoids parsing smatchet_config.json immediately after Lua init on MinGW release.

        jiraCfgForEditMetaWarmup = ConfigManager::Load();
    }

    InitLua();

    // Phase 4b of docs/plans/shipped/smatchet-merge-watcher.md — start the localhost
    // notify endpoint AFTER the main-thread dispatcher is initialised (it's a
    // member initialiser, ready since the AppController ctor) and BEFORE Lua
    // setup since the endpoint is independent of plugin state. Best-effort —
    // bind failure (port-in-use, no socket lib) logs WARN + Smatchet continues;
    // the daemon's shell bridge falls through to Windows native BurntToast.
    mergeWatchNotifyServer_ = std::make_unique<SmatchetMergeWatchNotifyServer>();
    mergeWatchNotifyServer_->Start(*this);

#if defined(SMATCHET_WITH_LUA_AUTOMATION)

    RunLuaSetupScript("SmatchetHooks.lua");

    automationWorker_ = std::thread(&AppController::AutomationWorkerLoop, this);

#endif

    if (activeTrackerType == "Jira") {

        WarmIssueTypeEditMetaAtStartAsync(std::move(jiraCfgForEditMetaWarmup));
    }
}

void AppController::InitCommands() {
    // Scenario runner — constructed before the registry so scenario.* commands
    // can capture a reference to it in their handlers.
    scenarioRunner_ = std::make_unique<smatchet::cmd::ScenarioRunner>();
    // Slice 5 of docs/plans/shipped/autonomous-debugging-no-creds.md — pure refactor.
    // The 14-entry RegisterFactory block lives in SmatchetScenarioRegistry.cpp
    // so adding/removing a scenario is one edit in a self-contained TU. The
    // snapshot test tests/Core/SmatchetScenarioRegistry.test.cpp pins
    // the registered name set.
    smatchet::cmd::RegisterAllScenarios(*scenarioRunner_);

    // Unified Command System — register the catalog last so handlers can capture
    // references to AppController state that's now fully wired (tracker backend,
    // Lua host, offline queue, etc.). See docs/plans/shipped/command-system-plan.md.
    try {
        commandRegistry_ = std::make_unique<smatchet::cmd::CommandRegistry>();
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
            LOG_WARN("AppController::Initialize: project-scope audit failed: unknown exception");
        }
    }

    // Seed the callstack-field syntax-highlight hint from persisted config at startup.
    // Without this, g_callstackFieldId stays empty until the Annotate window first
    // hydrates (AnnotateAnalysisUi_Config.cpp::HydrateAnnotateCfgDiskOnce) — so opening
    // a configured callstack field's grid cell / long-text editor before ever visiting
    // the Annotate window rendered it as plain markdown instead of C++-coloured. Seeding
    // here makes the hint live for the whole session regardless of UI navigation order.
    try {
        SetCallstackFieldIdHint(ConfigManager::LoadAnnotateAnalysis().CallstackTrackerFieldId);
    } catch (const std::exception& ex) {
        LOG_WARN("AppController::Initialize: callstack-field hint seed failed: %s", ex.what());
    } catch (...) {
        LOG_WARN("AppController::Initialize: callstack-field hint seed failed: unknown exception");
    }

#if defined(SMATCHET_WITH_AI)
    // Construct the Smatchet Assistant controller last so it captures a settled config
    // snapshot (ConfigManager::Load() above already populated all Ai* fields). The
    // controller spawns its worker thread in its own constructor — no further wiring
    // needed. Lifetime contract: destroyed at the top of ~AppController.
    try {
        aiAssistant_ = std::make_unique<AiAssistantController>(*this);
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::Initialize: AiAssistantController init failed: %s", ex.what());
        aiAssistant_.reset();
    } catch (...) {
        LOG_ERROR("AppController::Initialize: AiAssistantController init failed: unknown exception");
        aiAssistant_.reset();
    }
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

smatchet::cmd::CommandRegistry& AppController::Commands() {
    if (!commandRegistry_) {
        // Lazy fallback — caller invoked us before Initialize (tests, embedded hosts).
        commandRegistry_ = std::make_unique<smatchet::cmd::CommandRegistry>();
    }
    return *commandRegistry_;
}

const smatchet::cmd::CommandRegistry& AppController::Commands() const {
    if (!commandRegistry_) {
        const_cast<AppController*>(this)->commandRegistry_ = std::make_unique<smatchet::cmd::CommandRegistry>();
    }
    return *commandRegistry_;
}

smatchet::cmd::ScenarioRunner& AppController::Scenarios() {
    if (!scenarioRunner_) {
        scenarioRunner_ = std::make_unique<smatchet::cmd::ScenarioRunner>();
    }
    return *scenarioRunner_;
}

const smatchet::cmd::ScenarioRunner& AppController::Scenarios() const {
    if (!scenarioRunner_) {
        const_cast<AppController*>(this)->scenarioRunner_ = std::make_unique<smatchet::cmd::ScenarioRunner>();
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

        fs::directory_iterator it(root, ec);
        if (ec) {
            LOG_WARN("ListLuaScriptFiles: failed to enumerate %s: %s", root.string().c_str(), ec.message().c_str());
            return out;
        }
        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {

            if (ec) {

                break;
            }

            const auto& ent = *it;
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
        return FieldIconPathIsAllowed(absPath.string(), luaScriptsDirectory_);
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

#if defined(SMATCHET_WITH_AI)
void AppController::LoadAiChatMessages(std::size_t cap, std::vector<AiMessage>& outMessages,
                                       std::vector<std::int64_t>& outIds) const {
    outMessages.clear();
    outIds.clear();
    if (!Cache) {
        return;
    }
    Cache->LoadChatMessages(cap, outMessages, outIds);
}
#endif

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
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->CancelAndJoinActiveStreamingSync();
        focusedContext().ticketSync_->ResetStaleDeletionState();
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
        Cache = std::make_unique<LocalCacheManager>(localCacheDbPath_);
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

    // Latch via atomic_load: command handlers (MCP / Lua) can call this off the UI thread, so a
    // plain Backend read would race a live SetBackend swap. The shared_ptr also keeps the backend
    // alive across the blocking FetchIssues call (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&focusedContext().Backend);
    if (!backend || !Cache) {

        return pack;
    }

    std::lock_guard<std::mutex> lock(g_TrackerIssueFetchMutex);

    pack.Tickets = backend->Reader().FetchIssues(&pack.FullSyncCompleted, configOverride, viewsOverride,
                                                 &pack.FetchError, &pack.Warning);

    return pack;
}

// ApplyIssueFetchPack / CancelAndJoinActiveStreamingSync: moved to TicketSyncService in
// Phase 1A of the item 11 extraction. Thin delegators below.

void AppController::ApplyIssueFetchPack(TrackerIssueFetchPack pack) {
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->ApplyIssueFetchPack(std::move(pack));
    }
}

void AppController::CancelAndJoinActiveStreamingSync() {
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->CancelAndJoinActiveStreamingSync();
    }
}

void AppController::TickStreamingApply() {
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->TickStreamingApply();
    }
}

void AppController::SyncWithBackend(const TrackerConfig* configOverride, const ViewsStore* viewsOverride) {
    if (focusedContext().ticketSync_) {
        focusedContext().ticketSync_->SyncWithBackend(configOverride, viewsOverride);
    }
}

bool AppController::IsStreamingSyncActive() const {
    return focusedContext().ticketSync_ && focusedContext().ticketSync_->IsActive();
}
