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
#include "AppControllerImpl.h"
#include "GridContextDepsAdapter.h"
#include "GridPaneEvictionPolicy.h"
#include "LocalCacheManager.h" // direct: AppController.h now fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls Cache-> methods.
#include <nlohmann/json.hpp> // direct: AppController.h dropped json.hpp for json_fwd (fan-in Phase 1); this TU constructs nlohmann::json.

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
#include "BackgroundTaskFirewall.h"
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
#include "ITrackerIssueMutations.h"

#include "LinearFixtureBackend.h"

#include "PlaneFixtureBackend.h"

#include "LuaAutomationHost.h"

#include "OfflineQueueService.h"

#include "EditMetaCacheService.h"
#include "FieldEditPipelineService.h"
#include "ConnectivityMonitorService.h"
#include "AttachmentAppUpdateService.h"

#include "PaneSyncKickPolicy.h"

#include "SalientRosterResolve.h"
#include "Sync/MembershipDiffPure.h"
#include "Sync/TicketChangeDiffPure.h"
#include "SmatchetTicketChangeNotifications.h"

#include "TicketSyncService.h"

#include "TrackerHttpUtils.h"

#include "JqlProjectScope.h"
#include "Logger.h"

#include "StringUtil.h"
#include "UiThreadAffinity.h"

#include "Views.h"

#include "SmatchetUI.h"
#include "UiPerfMonitor.h" // SMATCHET_UI_PERF_SCOPE for TickAllContexts (multi-grid concurrent-sync perf scope)

#include "SmatchetToast.h"
#include "SmatchetMergeWatchNotifyServer.h"

#include "AiTypes.h"
#include "ConfigSaveWorker.h" // not AI-gated — config saves happen regardless of feature flags
#if defined(SMATCHET_WITH_AI)
#include "AiAssistantController.h"
#include "AiAssistantUiStateAdapter.h"
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

// Out-of-line definition (ODR-use in map lookups; C++14).
const std::string AppController::kDefaultPaneId = "main";

// pImpl (hardening #19): COLD, sol2-/subsystem-heavy state moved off AppController.h so the
// header no longer needs <sol/sol.hpp>. `struct AppController::Impl` is now defined in the
// src-only AppControllerImpl.h (included above) where its sol2 / subsystem member types are
// complete. The out-of-line ctor/dtor here keep the incomplete-type unique_ptr<Impl> in the
// header legal.
AppController::AppController()
    : automationShutdownCancel_(std::make_shared<std::atomic<bool>>(false)), impl_(std::make_unique<Impl>(*this)) {
    // Multi-grid (ADR-0018): the default context is created here — not lazily — and is
    // PERMANENT, so focusedContext()'s fallback stays valid for the controller's entire
    // lifetime (delegators, the deps adapter, and the destructor all assume it exists).
    gridContexts_[kDefaultPaneId] = std::make_unique<GridLiveContext>();
    focusedPaneId_ = kDefaultPaneId;
    focusedContextPtr_ = gridContexts_.find(kDefaultPaneId)->second.get();
}

// Sol-free accessor to the Lua binding host. AppController::Impl implements ILuaBindingHost in
// the Lua build; returns nullptr in the no-Lua build (Impl has no such base there). Keeps sol2
// out of AppController.h — hardening #19c. (ILuaBindingHost is complete here via AppControllerImpl.h.)
ILuaBindingHost* AppController::GetLuaBindingHost() {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    return impl_.get();
#else
    return nullptr;
#endif
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
    impl_->aiAssistant_.reset();
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

        std::lock_guard<std::mutex> lock(impl_->automationJobMutex_);

        impl_->automationWorkerShuttingDown_.store(true);
    }

    // Abort any in-flight blocking tracker mutation the worker is parked inside (UpdateIssueFields →
    // TrackerPut/Patch/PostLogged). The retry/backoff loop polls this token before each attempt and at
    // 50 ms granularity during backoff (#1529), so a job blocked in synchronous tracker glue — which
    // executes no Lua and so never fires the count-hook — stops retrying immediately and unwinds. A
    // single already-issued HTTP request still runs to its own kTrackerOverallTimeoutMs, but the
    // pathological case (up to maxAttempts × timeout of serial retries) is gone, so the unconditional
    // join() below completes within one request timeout at most — not the multi-retry worst case — and
    // still without .detach (the worker returns from the cancelled call and breaks its loop).
    automationShutdownCancel_->store(true);

    impl_->automationJobCv_.notify_all();

    if (impl_->automationWorker_.joinable()) {
        // Bounded, observable shutdown wait. The count-hook releases a pure-Lua job
        // the instant automationWorkerShuttingDown_ is seen, but a job blocked inside
        // synchronous C++ glue (a tracker HTTP PUT — see LuaAutomationHookPolicyPure.h)
        // executes no Lua instructions, so the hook never fires and an unbounded join
        // would hang here until the HTTP timeout with no diagnostic. Wait on the
        // worker-exited flag up to a deadline; if it overruns, log a loud WARN naming
        // the hang, then still join — abandoning the thread (.detach) is banned and
        // unsafe (it could touch freed Impl state), so the warned bounded wait is the
        // safe terminal: the join still completes once the blocking call returns.
        constexpr std::chrono::seconds kAutomationJoinWarnDeadline(5);
        {
            std::unique_lock<std::mutex> lock(impl_->automationJobMutex_);
            const bool exited = impl_->automationJobCv_.wait_for(
                lock, kAutomationJoinWarnDeadline, [this]() { return impl_->automationWorkerExited_.load(); });
            if (!exited) {
                LOG_WARN("AppController shutdown: automation worker did not exit within %llds — likely blocked in "
                         "synchronous tracker glue (e.g. an in-flight HTTP request). Waiting for it to return; "
                         "shutdown is bounded by that call's own timeout, not hung.",
                         static_cast<long long>(kAutomationJoinWarnDeadline.count()));
            }
        }
        impl_->automationWorker_.join();
    }

#endif

    // Join UiDrawSession std::async futures that captured this controller (via static `g_ui`) before

    // tearing down members other threads may still touch.

    DrainUiDrawSessionFuturesBeforeAppTeardown(*this);

    shuttingDown_.store(true);

    // Drain the in-flight connectivity probe future before joining the background pool — the FSM's
    // std::async worker must not outlive the service (Phase 3 R7). Ordering preserved from the prior
    // DrainTrackerConnectivityProbeFuture() site.
    if (connectivity_) {
        connectivity_->DrainProbeFuture();
    }

    JoinBackgroundTasks();

    // A4 (BACKLOG_CODE_REVIEW.md): every background thread that can emit a log line is joined
    // above, so flush the async file sink now — this guarantees the whole shutdown-sequence log
    // trail reaches disk before member destruction and the riskier late-teardown steps
    // (mainWindow/pluginHost reset, static-destructor time) run. The abrupt-CRASH path is
    // deliberately NOT flushed through the logger: SmatchetCrashHandler is async-signal-safe and
    // must not take the file-sink mutex / wait on its ack condvar mid-crash (deadlock risk) — a
    // crash captures its trail via that handler's separate async-safe crash sink instead.
    Logger::Instance().FlushFileSink();
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

    auto fetchResult = backend->Reader().FetchIssuesForKeys(cfg, toFetch, views);
    const bool ok = static_cast<bool>(fetchResult);
    if (ok) {
        tickets = std::move(fetchResult.value());
    } else {
        err = fetchResult.error().Detail;
    }

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

    // Mutex-guarded copy — this runs on a background worker while the UI thread may swap
    // the tracker (and re-stamp the key) concurrently (multi-grid Slice 1b).
    const std::string cacheBackendKey = focusedContext().CacheBackendKeyCopy();

    for (const auto& t : tickets) {

        Cache->SaveTicket(cacheBackendKey, t);
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
            // An exception escaping a worker thread function calls std::terminate
            // (the UI-thread SEH guard does NOT cover worker threads) — a single
            // throw in any background task (e.g. a backend-switch race in the
            // sync worker, a JSON-parse failure on an unexpected API body) takes
            // the whole app down. Contain it: log + abandon the one task, app
            // stays up (Pillar-3 never-crash / graceful degradation). Firewall
            // algorithm is the pure BackgroundTaskFirewall.h (unit-tested).
            std::string what;
            const smatchet::BackgroundTaskOutcome outcome = smatchet::RunBackgroundTaskFirewalled(task, what);
            if (outcome == smatchet::BackgroundTaskOutcome::StdException) {
                LOG_ERROR("AppController: background task threw std::exception: %s — task abandoned, app continues.",
                          what.c_str());
            } else if (outcome == smatchet::BackgroundTaskOutcome::UnknownException) {
                LOG_ERROR("AppController: background task threw a non-std exception — task abandoned, app continues.");
            }
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

    hostCallbacks_.OpenUrl = std::move(handler);
}

void AppController::SetCloseEmbeddedUiHandler(std::function<void()> handler) {

    hostCallbacks_.CloseEmbeddedUi = std::move(handler);
}

void AppController::CloseEmbeddedUi() {

    if (hostCallbacks_.CloseEmbeddedUi) {

        hostCallbacks_.CloseEmbeddedUi();
    }
}

void AppController::SetRequestAppQuitHandler(std::function<void()> handler) {
    hostCallbacks_.RequestAppQuit = std::move(handler);
}

void AppController::RequestAppQuit() const {

    if (hostCallbacks_.RequestAppQuit) {

        hostCallbacks_.RequestAppQuit();
    }
}

void AppController::SetRuntimePluginHost(PluginHost* host) { runtimePluginHost_ = host; }

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

    if (hostCallbacks_.OpenUrl) {

        hostCallbacks_.OpenUrl(url);

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
    if (impl_->luaHost_) {
        impl_->luaHost_->AddAutomationLogSink(std::move(sink));
    } else {
        // OnEarlyInit fires before Initialize constructs luaHost_. Buffer the sink;
        // Initialize drains pendingLogSinks_ into luaHost_ immediately after construction.
        if (sink) {
            pendingLogSinks_.push_back(std::move(sink));
        }
    }
}

void AppController::ClearAutomationLogSinks() {
    if (impl_->luaHost_) {
        impl_->luaHost_->ClearAutomationLogSinks();
    }
}

void AppController::AddAutomationErrorSink(std::function<void(const std::string&)> sink) {
    errorSinks_.push_back(std::move(sink));
}

bool AppController::ConsumeScriptingWindowRequest() { return scriptingWindowOpenRequested_.exchange(false); }

void AppController::SetAttachmentViewerHandler(AttachmentViewerHandler handler) {

    hostCallbacks_.AttachmentViewer = std::move(handler);
}

void AppController::SetAttachmentPreviewHandler(AttachmentPreviewHandler handler) {

    hostCallbacks_.AttachmentPreview = std::move(handler);
}

void AppController::SetAttachmentCollectionHandler(AttachmentCollectionHandler handler) {

    hostCallbacks_.AttachmentCollection = std::move(handler);
}

void AppController::SetOpenFilePathsHandler(OpenFilePathsHandler handler) {

    hostCallbacks_.OpenFilePaths = std::move(handler);
}

void AppController::RequestOpenFilePaths(bool allowMultiple, const std::string& initialDirectoryUtf8,

                                         std::function<void(std::vector<std::string>)> onComplete) const {

    if (!onComplete) {

        return;
    }

    if (hostCallbacks_.OpenFilePaths) {

        hostCallbacks_.OpenFilePaths(allowMultiple, initialDirectoryUtf8, std::move(onComplete));

        return;
    }

    onComplete({});
}

// App-update surface — thin delegators forwarding into AttachmentAppUpdateService (god-object
// decomposition Phase 4). The full implementations (semantic-version parse/compare, the GitHub
// releases query, the installer download + launch) moved verbatim into AttachmentAppUpdateService.cpp
// along with their file-local helpers; the public signatures are preserved so the UI / command /
// diagnostics call sites compile unchanged.
std::string AppController::GetAppVersion() const { return attachmentAppUpdate_->GetAppVersion(); }

std::string AppController::GetGitHubReleaseRepo() const { return attachmentAppUpdate_->GetGitHubReleaseRepo(); }

AppUpdateInfo AppController::CheckForAppUpdate(bool includePrerelease) const {
    return attachmentAppUpdate_->CheckForAppUpdate(includePrerelease);
}

bool AppController::DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl, const std::string& assetName,
                                                     std::string& outError,
                                                     std::shared_ptr<std::atomic<bool>> cancelFlag) const {
    return attachmentAppUpdate_->DownloadAndLaunchInstallerUpdate(downloadUrl, assetName, outError, cancelFlag);
}

bool AppController::IsOnUiThread() const {
    // uiThreadId_ is written once in Initialize before any worker is spawned, then never
    // mutated. Reads from worker threads are race-free under publish-once semantics.
    return std::this_thread::get_id() == uiThreadId_;
}

void AppController::PostToMainThread(std::function<void()> fn) {
    // IMainThreadPoster — delegate to the concrete dispatcher. Lets the Commands/
    // helper templates marshal onto the UI thread through the interface without
    // including AppController.h (core-include-dag Phase 2).
    mainThreadDispatcher.PostToMainThread(std::move(fn));
}

#if defined(SMATCHET_WITH_AI)
bool AppController::HasAiAssistantController() const { return impl_->aiAssistant_ != nullptr; }

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
    return *impl_->aiAssistant_; // pre-condition: HasAiAssistantController() == true
}
#endif

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
    //
    // DR6: quiesce EVERY live pane's streaming-sync worker, not just the focused one. Each
    // non-focused GridLiveContext runs its own TicketSyncService std::thread that dereferences
    // Cache; those must be joined before Cache.reset() below or they race the freed cache.
    // gridContexts_ is a UI-thread-owned map and RecreateLocalCacheDatabase runs on the UI
    // thread, so iterating it here is free of concurrent structural mutation. Per context the
    // cancel-and-join is idempotent (a second call finds no active thread).
    for (auto& entry : gridContexts_) {
        if (entry.second && entry.second->ticketSync_) {
            entry.second->ticketSync_->CancelAndJoinActiveStreamingSync();
            entry.second->ticketSync_->ResetStaleDeletionState();
        }
    }
    // Explicit focused pass in case focusedContextPtr_ currently resolves to a retired husk
    // (ADR-0012 graveyard) that is no longer in the live gridContexts_ map. Idempotent with
    // the loop above when focused is a live entry.
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
        // Fresh DB — nothing to copy, but stamp the tickets_v2 migration flag so the one-time
        // sweep never runs against rows written after this point (multi-grid Slice 1b).
        (void)Cache->RunOneTimeTicketsV2CopyMigration(focusedContext().CacheBackendKeyCopy());
        // Same for the pending-queue backend_key stamp (Slice 1c) — fresh DB, nothing to stamp,
        // but consume the one-time flag so it never runs against rows written after this point.
        (void)Cache->RunOneTimePendingQueueBackendKeyStamp(focusedContext().CacheBackendKeyCopy());
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

bool AppController::EnsureLocalCacheForUiTest() {
    // Bucket-E opt-in (SMATCHET_UITEST_WITH_LOCAL_CACHE=1). A normal boot already
    // opened a file-backed cache in InitConfig; only stand one up when it is unset
    // so this is a true no-op in the common case. The in-memory db lives for the
    // AppController's lifetime (torn down with `Cache`), so the scenario's offline
    // writes never touch the developer profile or any file.
    if (!Cache) {
        try {
            Cache = std::make_unique<LocalCacheManager>(":memory:");
            LOG_INFO("EnsureLocalCacheForUiTest: opened throwaway in-memory LocalCacheManager");
        } catch (const std::exception& ex) {
            LOG_ERROR("EnsureLocalCacheForUiTest: failed to open in-memory cache: %s", ex.what());
            return false;
        }
    }
    // Idempotent (`if (!x)` guards inside) — guarantees offlineQueue_ exists so
    // QueueCreateOffline routes to a live cache instead of returning 0.
    WireCoreServices();
    // A fresh bucket-E spawn child has no setup config file, so ConfigManager::Load()
    // applies the first-run safety default ReadOnlyMode=true (ConfigManager.cpp). That
    // default trips the very first guard in OfflineQueueService::QueueCreateOffline
    // (return 0 under read-only), which would keep the case-8 offline-create populated
    // path SKIPping even with a live cache. This opt-in seam already means "this UI test
    // wants to exercise offline writes", so clear read-only + persist + invalidate so the
    // next ConfigManager::Load() inside the guard observes it. Throwaway profile only
    // (SMATCHET_USER_DATA tmpdir under the harness) — never the developer's real config.
    {
        TrackerConfig cfg = ConfigManager::Load();
        if (cfg.ReadOnlyMode) {
            cfg.ReadOnlyMode = false;
            ConfigManager::Save(cfg);
            ConfigManager::InvalidateCache();
            LOG_INFO("EnsureLocalCacheForUiTest: cleared first-run ReadOnlyMode default so "
                     "offline-create populated path can activate");
        }
    }
    return Cache != nullptr;
}

void AppController::ClearLastTrackerTicketSyncWarning() {
    if (connectivity_) {
        connectivity_->ClearLastTicketSyncWarning();
    }
}

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
    // Classify at the composition seam (N12 slice 1): downstream consumers branch on the flag.
    // Slice 2 replaces this text sniff with structured TrackerError classification per backend.
    pack.FetchErrorTransient = !pack.FetchError.empty() && IsTrackerTransportErrorText(pack.FetchError);

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
