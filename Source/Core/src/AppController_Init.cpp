// AppController_Init.cpp — bootstrap / initialization cluster extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/appcontroller-service-extraction.md). Holds the strict-order
// startup sequence: Initialize -> WireCoreServices -> InitConfig -> InitBackends
// -> InitFieldCatalog -> InitPlugins -> InitCommands, plus the GitHub-fixture /
// legacy-startup-sweep / field-catalog-snapshot helpers they call. Method
// DECLARATIONS stay in AppController.h; only the definitions moved here, so
// linkage and behavior are identical. Includes are curated to the headers the
// bootstrap bodies reference (matches the AppController_Connectivity.cpp /
// _CatalogAndFieldEdit.cpp companion-TU idiom; no winsock preamble needed — this
// TU pulls no cpr/curl header).
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining AppController bootstrap methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on
#include "AppControllerImpl.h"
#include "GridContextDepsAdapter.h"
#include "LocalCacheManager.h" // direct: AppController.h fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls Cache-> methods.

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

// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=a companion TU of the AppController god-class necessarily shares its subsystem include set (ConfigManager / backends / owned services); no shared header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when AppController.h fan-in is narrowed per ADR-0020 / debt.md)
// clang-format on
#include "ConfigManager.h"
#include "ConfigSaveWorker.h" // not AI-gated — config saves happen regardless of feature flags

#include "Commands/BuiltinCommands.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/SmatchetScenarioRegistry.h"

#include "FieldCatalogCache.h"
#include "JqlProjectScope.h"

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
#include "TicketSyncService.h"

#include "Logger.h"
#include "StringUtil.h"
#include "UiThreadAffinity.h"
#include "Views.h"

#include "SmatchetUI.h"
#include "SmatchetToast.h"
#include "SmatchetMergeWatchNotifyServer.h"
#include "Ui/SmatchetFieldRender.h" // RunLegacyStartupSweeps calls the free fn SetCallstackFieldIdHint declared here

#include <ghc/filesystem.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

#include "AiTypes.h"
#if defined(SMATCHET_WITH_AI)
#include "AiAssistantController.h"
#include "AiAssistantUiStateAdapter.h"
#include "SmatchetChatPersistWorker.h"
#include "SmatchetUiSession.h"
#endif

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

void AppController::WireCoreServices() {
    // Construct the deps adapter eagerly so the owned services can capture an interface reference at
    // construction time. The adapter is owned by this AppController and outlives every service (per
    // the destructor ordering: ~AppController joins the streaming-sync worker via
    // `CancelAndJoinActiveStreamingSync` before any member is destroyed, so the adapter is live for
    // every `deps_.X` call). All constructions are idempotent (`if (!x)` guards).
    if (!depsAdapter_) {
        depsAdapter_ = std::make_unique<GridContextDepsAdapter>(*this, focusedContext());
    }
    // GLOBAL ConnectivityMonitorService — every connectivity delegator (TickTrackerConnectivityMonitor,
    // the per-frame banner, the recovery/refetch latches) and the adapter's three re-pointed
    // ITicketSyncDeps connectivity setters need a live target from the first frame tick (Phase 3).
    // One instance; every per-pane adapter forwards its connectivity writes here (N writers, one
    // service). Holds the deps-adapter ref.
    if (!connectivity_) {
        connectivity_ = std::make_unique<ConnectivityMonitorService>(*depsAdapter_);
    }
    // AttachmentAppUpdateService — the attachment-open + GitHub app-update delegators (OpenAttachment,
    // ShowAttachmentCollection, CheckForAppUpdate, DownloadAndLaunchInstallerUpdate, …) forward here.
    // Holds the deps-adapter ref; no mutex, no per-context state, so construction order vs the other
    // services is immaterial beyond depsAdapter_ existing (Phase 4).
    if (!attachmentAppUpdate_) {
        attachmentAppUpdate_ = std::make_unique<AttachmentAppUpdateService>(*depsAdapter_);
    }
    // EditMetaCacheService — every editmeta delegator (CanEditFieldForIssue, EnsureIssueEditMetaLoaded,
    // the warm-start path, …) needs a live target from the first tick (Phase 1). Holds the adapter ref.
    if (!editMeta_) {
        editMeta_ = std::make_unique<EditMetaCacheService>(*depsAdapter_);
    }
    // OfflineQueueService — the legacy-pending startup migration below writes to
    // `offlineQueue_->legacyPendingStartupBanner_` (item 12 extraction).
    if (!offlineQueue_) {
        offlineQueue_ = std::make_unique<OfflineQueueService>(*depsAdapter_);
    }
    // FieldEditPipelineService — every field-edit delegator (SubmitFieldEdit, SubmitFieldEditNetworkOnly,
    // TryPrepareOfflineFieldEdit, ApplyFieldEditResult) needs a live target from the first tick (Phase 2).
    // Holds the deps adapter + EditMetaCacheService by reference — both constructed above, both outlive it.
    if (!fieldEdit_) {
        fieldEdit_ = std::make_unique<FieldEditPipelineService>(*depsAdapter_, *editMeta_);
    }
    // TicketSyncService — its `CancelAndJoinActiveStreamingSync` is called by RecreateLocalCacheDatabase
    // (which the legacy-pending cleanup may trigger), so it must exist before that path runs (item 11).
    if (!focusedContext().ticketSync_) {
        focusedContext().ticketSync_ = std::make_unique<TicketSyncService>(*depsAdapter_);
    }
}

void AppController::InitConfig(const std::string& dbPath, const std::string& backendType) {
    // Record the UI thread identity first. Initialize() is invoked from main() before any
    // background thread is spawned, so this happens-before any worker that could call
    // IsOnUiThread() later. See AppController.h for the full reasoning.
    uiThreadId_ = std::this_thread::get_id();
    // Same publish-once capture into the low-layer registry the sub-AppController blocking
    // wrappers (ConfigManager / P4) query — they can't reach this private uiThreadId_.
    UiThreadAffinity::SetUiThread();

    LOG_INFO("AppController::Initialize backendType=%s dbPath=%s", backendType.c_str(), dbPath.c_str());

    // Start the coalescing config-save worker before anything can enqueue a save. No deps (no
    // dispatcher/LCM); Stop()+join runs early in ~AppController. Not AI-gated.
    smatchet::config_save::Start();

    localCacheDbPath_ = dbPath;

    // DR6: publish the initial cache via atomic_store — off-thread workers snapshot with
    // atomic_load, mirroring the ADR-0012 Backend atomic-swap discipline.
    std::atomic_store(&Cache, std::make_shared<LocalCacheManager>(dbPath));

    // Multi-grid Slice 1b (ADR-0018 decision 4): seed the default context's cache namespace
    // from the Initialize parameter so nothing observes an empty key during this phase.
    // The one-time legacy→v2 ticket copy + pending-queue key stamp run in InitBackends AFTER
    // the authoritative key is resolved from cfg.TrackerType + env hooks (CR-948-1: the
    // Initialize parameter can diverge from the live key — embedded host / standalone CLI —
    // and stamping legacy rows with the parameter's key would orphan them from every read
    // the live path performs). Nothing reads the ticket tables before InitBackends (first
    // ticket read is RefreshLocalData in InitFieldCatalog) and no replay tick runs yet, so
    // deferring the migrations to the resolved key is safe.
    focusedContext().SetCacheBackendKey(ConfigManager::NormalizeViewsBackendKey(backendType));

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

    // Construct the deps adapter + owned single-responsibility services (extracted to keep
    // InitConfig under the function-size cap — god-object decomposition).
    WireCoreServices();
    // Construct LuaAutomationHost so `AddAutomationLogSink` calls from plugins'
    // OnEarlyInit have a target (item 14 extraction, Phase 1A).
    if (!impl_->luaHost_) {
        impl_->luaHost_ = std::make_unique<LuaAutomationHost>();
    }
    // Drain sinks buffered by AddAutomationLogSink calls during OnEarlyInit (which runs
    // before Initialize). From this point forward AddAutomationLogSink forwards directly.
    for (auto& s : pendingLogSinks_) {
        impl_->luaHost_->AddAutomationLogSink(std::move(s));
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

    // Hidden-pane LRU cap (multi-grid Slice 5a, plan item 21): cache the config knob once so
    // TickAllContexts never reads config on the per-frame path. 0/negative → default 4.
    hiddenPaneResidentCap_ = cfg.HiddenPaneResidentCap > 0 ? static_cast<std::size_t>(cfg.HiddenPaneResidentCap) : 4;

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

    // Slice 4 of docs/plans/linear-tracker-backend.md — the Linear sibling
    // of the Plane fixture hook above. SMATCHET_TEST_LINEAR_BACKEND_FIXTURE=<path>
    // swaps in a fixture-driven Linear backend (no GraphQL, no API key) so the
    // zero-credentials scenario replay covers the Linear read path too.
    if (!backendFactory_) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform read-only; _dupenv_s is MSVC-only
#endif
        const char* linearFixtureEnv = std::getenv("SMATCHET_TEST_LINEAR_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        if (linearFixtureEnv && linearFixtureEnv[0] != '\0') {
            LOG_INFO(
                "AppController: SMATCHET_TEST_LINEAR_BACKEND_FIXTURE=%s — installing LinearFixtureBackend factory.",
                linearFixtureEnv);
            backendFactory_ = smatchet::linear::MakeLinearFixtureBackendFactory(std::string(linearFixtureEnv));
            activeTracker = "Linear";
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
    // Re-stamp the cache namespace with the RESOLVED tracker — an env fixture hook above may
    // have overridden the configured type (multi-grid Slice 1b).
    ctx.SetCacheBackendKey(ConfigManager::NormalizeViewsBackendKey(activeTracker));
    // One-time legacy migrations run HERE, against the authoritative resolved key (CR-948-1):
    // this is the same key every live read/write path queries (mirrors what
    // RecreateLocalCacheDatabase already does). Must stay BEFORE RunLegacyStartupSweeps (which
    // archives pending rows — archived rows must carry stamped keys) and BEFORE the first
    // ticket read (RefreshLocalData in InitFieldCatalog) / any replay tick.
    if (Cache) {
        const std::string resolvedCacheKey = ctx.CacheBackendKeyCopy();
        try {
            // Legacy rows were necessarily cached against the then-only configured backend.
            (void)Cache->RunOneTimeTicketsV2CopyMigration(resolvedCacheKey);
        } catch (const std::exception& ex) {
            // Pillar 3 graceful degradation — a failed copy leaves the flag unset
            // (transactional), so the next launch retries; the session continues with
            // whatever v2 already holds.
            LOG_ERROR("AppController::InitBackends tickets_v2 copy migration failed: %s", ex.what());
        }
        // Multi-grid Slice 1c: backfill the pending-queue backend_key columns once — legacy
        // queue rows were necessarily queued against the then-only configured backend.
        try {
            (void)Cache->RunOneTimePendingQueueBackendKeyStamp(resolvedCacheKey);
        } catch (const std::exception& ex) {
            // Same graceful-degradation contract as the copy migration above: the
            // transactional stamp leaves the flag unset on failure, so the next launch retries.
            LOG_ERROR("AppController::InitBackends pending-queue backend_key stamp failed: %s", ex.what());
        }
    }
    std::atomic_store(&ctx.Backend, std::shared_ptr<ITrackerBackend>(backendFactory_->Create(activeTracker, cfg)));
    if (!ctx.Backend) {
        LOG_ERROR("AppController: tracker backend factory returned null for type '%s'.", activeTracker.c_str());
    } else {
        // Thread the app-lifetime shutdown cancel token into the new backend's mutation path, so a
        // blocking UpdateIssueFields running inside an automation worker aborts when shutdown raises
        // the token (see ~AppController). No-op on backends without a cancellable mutation path.
        if (ITrackerIssueMutations* mutations = ctx.Backend->Mutations()) {
            mutations->SetMutationCancelToken(automationShutdownCancel_);
        }
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
        std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType, const TrackerConfig& cfg) override {
            const std::string lower = ToLowerAsciiCopy(trackerType);
            if (lower == "github") {
                return std::make_unique<smatchet::github::GitHubFixtureBackend>(path_, std::string(), std::string(),
                                                                                /*includePullRequests=*/true);
            }
            // Non-GitHub backends fall through to the default factory shape.
            DefaultTrackerBackendFactory fallback;
            return fallback.Create(trackerType, cfg);
        }

      private:
        std::string path_;
    };
    backendFactory_ = std::make_unique<FixtureGitHubFactory>(fixturePath);
}

void AppController::RunLegacyStartupSweeps(const std::string& activeTrackerType) {
    // remove-global-project-key.md: one-shot legacy-project sweeps.
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
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would split this compound write across two contexts (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
    cat.AvailableFields = std::move(snapFields);
    cat.AvailableComponents = std::move(snapComponents);
    cat.AvailableIssueTypeMeta = std::move(snapIssueTypeMeta);
    cat.fieldCatalogEverLoaded_ = true;
    cat.LastTrackerFieldCatalogError.clear();

    if (activeTrackerType == "Plane") {
        cat.LastTrackerFieldCatalogWarning =
            "Working offline: Plane field catalog loaded from local snapshot until a live refresh succeeds.";
    } else {
        cat.LastTrackerFieldCatalogWarning =
            "Working offline: tracker field catalog loaded from local snapshot until a live refresh succeeds.";
    }

    if (activeTrackerType == "Jira") {
        for (auto& field : cat.AvailableFields) {
            if (field.Id == "timespent" || field.Id == "aggregatetimeoriginalestimate" ||
                field.Id == "aggregatetimeestimate" || field.Id == "aggregatetimespent") {
                field.ReadOnly = true;
            }
        }
        EraseCatalogLegacyCommentField(cat);
        EnsureCatalogHistoryField(cat);
        EnsureCatalogCommentsField(cat);
    }

    cat.TrackerFieldCatalogRevision.fetch_add(1);
    LOG_INFO("AppController::Initialize: restored field catalog from snapshot (%zu fields)",
             cat.AvailableFields.size());
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
    // a bind failure (port-in-use, or no socket lib) logs a WARN and Smatchet
    // keeps running, so the daemon's shell bridge falls through to Windows native BurntToast.
    mergeWatchNotifyServer_ = std::make_unique<SmatchetMergeWatchNotifyServer>();
    mergeWatchNotifyServer_->Start(*this);

#if defined(SMATCHET_WITH_LUA_AUTOMATION)

    RunLuaSetupScript("SmatchetHooks.lua");

    impl_->automationWorker_ = std::thread(&AppController::Impl::AutomationWorkerLoop, impl_.get());

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
    // project scope. With no global project key, those views
    // broaden to "all projects you can read".
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
        impl_->aiAssistant_ =
            std::make_unique<AiAssistantController>(mainThreadDispatcher, GetGlobalAiAssistantUiState());
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::Initialize: AiAssistantController init failed: %s", ex.what());
        impl_->aiAssistant_.reset();
    } catch (...) {
        LOG_ERROR("AppController::Initialize: AiAssistantController init failed: unknown exception");
        impl_->aiAssistant_.reset();
    }
#endif
}
