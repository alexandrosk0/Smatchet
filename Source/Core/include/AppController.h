#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

// 1. MUST BE INCLUDED FIRST FOR GCC 13+ COMPATIBILITY
#include <limits>
#include <cstdint>

// 2. Lua / sol2 fully lifted off this header (hardening #19c). AppController.h includes
//    ZERO sol2: the sol-typed binding methods + the recorder/replay value types + the
//    McpToolDefinition type all live on AppController::Impl (src-only AppControllerImpl.h)
//    behind the ILuaBindingHost interface. The ~100 Ui/Commands includers no longer
//    transitively compile <sol/sol.hpp>. Only forward declarations are needed here.
class ILuaBindingHost;
namespace smatchet {
namespace lua {
struct ImCmd;
struct LuaFieldCacheEntry;
struct LuaWindowEntry;
struct PendingLuaWindowOp;
struct McpToolDefinition;
} // namespace lua
} // namespace smatchet

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <future>
#include <map>
#include "GridLiveContext.h"
#include "ITrackerBackend.h"
#include "SmatchetResult.h" // VoidResult (field-edit delegators — #21 AppController public-API flip)
#include "MainThreadDispatcher.h"
#include "Commands/IAppThreading.h"
#include "SmatchetMergeWatchNotifyServer.h"
#include "IssueDraft.h"
#include "IssueCreatePipeline.h"
#include "CancelToken.h"
// JiraClient.h was transitively supplying TrackerConfig and the tracker
// role-interface types (connectivity probes, fetch summaries, and others) used by
// AppController.h and its ~105 includers. Include their real homes directly here:
// ConfigManager.h and the five ITracker role interfaces, which are all cpr-free.
// That removes the heavy cpr dependency JiraClient.h dragged into every includer,
// which is the whole point of build-quality finding 3.
#include "ConfigManager.h"
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
// Fan-in Phase 2 (docs/plans/appcontroller-fan-in.md): ITrackerIssueMutations is forward-declared
// via ITrackerBackend.h (included above) — it pulls full <nlohmann/json.hpp> (inline json-constructing
// virtuals), so dropping its direct include here closes the second json door to the AppController.h
// includers. AppController.h names it only by-ref/ptr in out-of-line decls (ApplyFieldUpdate... +
// the mutations* members); the few TUs that call Mutations() include ITrackerIssueMutations.h directly.
#include "ITrackerIssueReader.h"

// Fan-in stabilization (docs/plans/appcontroller-fan-in.md, Phase 1): json_fwd, not
// json.hpp — AppController.h names nlohmann::json only by-reference in out-of-line
// method decls (no inline construction), so the forward-declaration header suffices
// and the heavy full json.hpp no longer rides the direct edge to the 114 includers.
#include <nlohmann/json_fwd.hpp>

class PluginHost;
// AiTypes.h is unconditional (POD header, no transitive includes beyond <atomic>/<memory>/etc.)
// so AppController.h consumers can use `AiContextBlock` without macro plumbing — the always-on
// stub methods take it by const-ref / return std::vector<AiContextBlock>.
#include "AiTypes.h"
#if defined(SMATCHET_WITH_AI)
// Fan-in Phase 4 (docs/plans/appcontroller-fan-in.md): forward-decl, not a full include. The
// `std::unique_ptr<AiAssistantController> aiAssistant_` member moved into the src-only
// AppController::Impl (AppControllerImpl.h includes the full type for its dtor); AppController.h only
// declares `AiAssistantController& GetAiAssistantController()` (out-of-line) + `HasAiAssistantController()`
// (by-reference / value-return), so the forward declaration suffices and AI-on includers stop
// pulling AiAssistantController.h transitively. (The old "complete type needed for the member dtor"
// rationale is stale — the member is no longer declared in this header.)
class AiAssistantController;
#endif

// TrackerConnectivityBannerForUi + TrackerIssueFetchPack moved to the leaf header
// Sync/SyncTypes.h so the Sync layer can name them without including AppController.h
// (core-include-dag Phase 3). They remain global-namespace, byte-identical.
#include "Sync/SyncTypes.h"
// The six offline-queue summary structs moved to Sync/OfflineQueueTypes.h (top-level)
// for the same reason; AppController re-exports them via using-aliases below so its
// ~113 includers keep seeing `AppController::DeadLetterRestoreSummary` etc.
#include "Sync/OfflineQueueTypes.h"
// Leaf pure header (STL-only): supplies smatchet::MembershipRemovalVerdict, named
// by-value in the membership-reconcile private method decls below (S1c-3, item 10).
#include "Sync/MembershipDiffPure.h"
// Leaf pure header (STL-only): supplies smatchet::PaneOwnedTicketIdStore, held BY VALUE as the
// per-pane owned-ticket-id member below, so it cannot be forward-declared.
#include "Sync/PaneOwnedTicketIdStore.h"
// Fan-in Phase 3 (docs/plans/appcontroller-fan-in.md): AppUpdateAsset/AppUpdateInfo (global),
// FieldEditResult, and TrackerConnectivityState (formerly nested) relocated to rank-0 leaf headers
// under Types/ so editing them no longer recompiles the ~115 AppController.h includers. The two
// formerly-nested types are re-exported via in-class `using`-aliases (below) so consumers keep
// naming `AppController::FieldEditResult` / `AppController::TrackerConnectivityState` unchanged.
#include "Types/AppUpdateTypes.h"
#include "Types/FieldEditTypes.h"
#include "Types/ConnectivityTypes.h"
#include "Types/AttachmentTypes.h"
#include "Types/HostCallbacks.h"

// Fan-in Phase 5 (docs/plans/appcontroller-fan-in-phase5-facets.md): narrow interface
// facets AppController implements so includer-clusters can depend on them instead of the
// full class. Rank-0 leaf headers (Interfaces/); see each header for its facet scope.
#include "Interfaces/IAppOfflineQueue.h"
#include "Interfaces/IAppMeta.h"
#include "Interfaces/IAppAttachments.h"
#include "Interfaces/IAppScenarios.h"
#include "Interfaces/IAppUsers.h"
#include "Interfaces/IAppDebug.h"
#include "Interfaces/IAppAutomation.h"
#include "Interfaces/IAppTicketData.h"
#include "Interfaces/IAppTicketMutations.h"
#include "Interfaces/IAppCommands.h"
#include "Interfaces/IAppScenarioHost.h"
#include "Interfaces/IAppSync.h"
#include "Interfaces/IAppFields.h"

class ITrackerBackendFactory;
class LocalCacheManager; // fan-in Phase 1: fwd-decl (was a direct heavy include); `std::unique_ptr<LocalCacheManager>
                         // Cache` member is fine with the incomplete type since ~AppController is out-of-line. Defining
                         // TUs include LocalCacheManager.h directly.
class GridContextDepsAdapter;
class OfflineQueueService;
class TicketSyncService;
class EditMetaCacheService;
class FieldEditPipelineService;
class ConnectivityMonitorService;
class AttachmentAppUpdateService;
class LuaAutomationHost;
struct TrackerActivityEntry;
struct TrackerActivityProgress;

namespace smatchet {
namespace cmd {
class CommandRegistry;
}
} // namespace smatchet
namespace smatchet {
namespace cmd {
class ScenarioRunner;
}
} // namespace smatchet

class AppController : public IAppThreading,
                      public IAppOfflineQueue,
                      public IAppMeta,
                      public IAppAttachments,
                      public IAppScenarios,
                      public IAppUsers,
                      public IAppDebug,
                      public IAppAutomation,
                      public IAppTicketData,
                      public IAppTicketMutations,
                      public IAppCommands,
                      public IAppScenarioHost,
                      public IAppSync,
                      public IAppFields {
    /// `GridContextDepsAdapter` implements `IOfflineQueueDeps` + `ITicketSyncDeps` against
    /// this AppController + one `GridLiveContext` and forwards every method either to the
    /// per-context state (`Backend`, `ActiveTickets*`) or to AppController-shared state
    /// (`Cache`, `AvailableFields`, the connectivity probe state, etc.). The previous
    /// `friend class OfflineQueueService;` + `friend class TicketSyncService;` declarations
    /// were replaced by this single friend during the item 11 / 12 Phase 2 extraction —
    /// the services now hold an `IOfflineQueueDeps& / ITicketSyncDeps&` reference and never
    /// touch AppController internals directly. Tests substitute `FakeOfflineQueueDeps` /
    /// `FakeTicketSyncDeps` so they can exercise the services without an AppController.
    friend class GridContextDepsAdapter;

  public:
    /// Creates the default (kDefaultPaneId) GridLiveContext so `focusedContext()` is valid
    /// from construction onward (multi-grid Slice 1, ADR-0018).
    AppController();
    ~AppController();

    /// Sol-free accessor to the Lua binding host (AppController::Impl implements ILuaBindingHost).
    /// Returns nullptr in the no-Lua build. Pointer return keeps sol2 out of this header — see
    /// hardening #19c. McpPlugin reads tool metadata through `GetLuaBindingHost()->GetLuaMcpTools()`.
    ILuaBindingHost* GetLuaBindingHost();

    // pImpl forward-decl made PUBLIC (#19c): the lifted Lua-binding glue free functions in
    // AppController_LuaBindings.cpp resolve `__smatchet_app_ui` to `AppController::Impl*`, so the
    // *name* must be reachable from outside the class. The full definition stays src-only
    // (AppControllerImpl.h); `impl_` itself remains a private member below.
    struct Impl;

    using FieldEditResult = ::FieldEditResult; // moved to Types/FieldEditTypes.h (fan-in Phase 3)

    /// Inject a custom tracker-backend factory. Must be called BEFORE `Initialize` to take
    /// effect on the first backend instantiation. Tests / Unreal-host embeddings use this
    /// to substitute a mock or alternative-transport client; the default standalone build
    /// leaves it null and `Initialize` lazily wires `DefaultTrackerBackendFactory`.
    void SetBackendFactory(std::unique_ptr<ITrackerBackendFactory> factory);

    /// Probe reachability/auth for `probeCfg` with a THROWAWAY backend built from it —
    /// the Preferences "Test connection" seam (P2-M12). Does not touch the live backend
    /// or the saved config. Blocking network I/O: dispatch on a worker via
    /// LaunchBackgroundTask. Requires Initialize() (backendFactory_ wired).
    TrackerReachabilityProbeResult ProbeTrackerCredentials(const TrackerConfig& probeCfg);

    void Initialize(const std::string& dbPath, const std::string& backendType);

  private:
    // Initialize() phase helpers (decompose-top-20-monoliths). Each is a private
    // bootstrap step invoked in strict order by Initialize(); ordering, error
    // handling, and early-returns are identical to the former monolith. They are
    // not re-entrant and assume single-threaded call from the UI thread before any
    // worker is spawned.

    /// Phase 1 — record the UI thread, start the config-save / chat-persist workers,
    /// open the LocalCacheManager, construct the deps adapter + offline-queue +
    /// ticket-sync + Lua host, drain buffered log sinks, and run the one-time
    /// legacy-pending offline cleanup.
    void InitConfig(const std::string& dbPath, const std::string& backendType);
    /// Eagerly construct the deps adapter + the owned single-responsibility services
    /// (connectivity / editmeta / offline-queue / field-edit / per-context ticket-sync) so every
    /// delegator + adapter override has a live target from the first frame tick. Called once from
    /// InitConfig; idempotent (`if (!x)` guards). Extracted from InitConfig (god-object
    /// decomposition) to keep that method under the function-size cap.
    void WireCoreServices();

    /// Phase 2 — load TrackerConfig, resolve the tracker backend factory (honouring
    /// the SMATCHET_TEST_*_BACKEND_FIXTURE env hooks), instantiate the backend, and
    /// run the one-time legacy-project / legacy-Plane-view sweeps. Publishes the
    /// resolved config into `cfgOut` and returns the active tracker type string.
    std::string InitBackends(TrackerConfig& cfgOut);

    /// InitBackends helper — install a fixture-backed GitHub backend factory when
    /// SMATCHET_TEST_GITHUB_BACKEND_FIXTURE is set and the active tracker is GitHub.
    /// No-op (leaving backendFactory_ untouched) otherwise.
    void MaybeInstallGitHubFixtureFactory(const std::string& activeTracker);

    /// InitBackends helper — run the one-time legacy-project / legacy-Plane-view
    /// sweeps for the resolved backend, each guarded by its own cache_meta marker.
    void RunLegacyStartupSweeps(const std::string& activeTrackerType);

    /// Phase 3 — resolve the Lua scripts directory, probe script files, refresh
    /// local data, and restore the field catalog from a local snapshot when present.
    void InitFieldCatalog(const TrackerConfig& cfg, const std::string& activeTrackerType);

    /// InitFieldCatalog helper — resolve the active view's project key from its JQL
    /// so the startup catalog snapshot loads under the project-scoped cache entry.
    /// Returns the resolved project key (empty when none resolves).
    std::string ResolveActiveViewProjectKeyForCatalog(const std::string& activeTrackerType) const;

    /// InitFieldCatalog helper — apply a loaded field-catalog snapshot to the live
    /// AvailableFields/Components/IssueTypeMeta state and publish the offline warning.
    void ApplyStartupFieldCatalogSnapshot(std::vector<TrackerField> snapFields,
                                          std::vector<TrackerComponent> snapComponents,
                                          std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta,
                                          const std::string& activeTrackerType);

    /// Phase 4 — initialise Lua, start the merge-watch notify endpoint, run the Lua
    /// setup script + automation worker, and warm the Jira issue-type edit-meta.
    void InitPlugins(const std::string& activeTrackerType);

    /// Phase 5 — construct the scenario runner + command registry, run the one-time
    /// views-without-project-scope audit, seed the callstack-field hint, and (in AI
    /// builds) construct the assistant controller.
    void InitCommands();

  public:
    /// True iff the calling thread is the one that called `Initialize` (the UI thread).
    /// Used by Command handlers to decide whether to mutate UI state inline (safe) or post
    /// the mutation to `mainThreadDispatcher` (required when called from an MCP / Lua worker).
    /// Recorded once in `Initialize`; reads are atomic loads — safe from any thread.
    bool IsOnUiThread() const override;

    /// IMainThreadPoster — post `fn` to the UI thread by delegating to
    /// `mainThreadDispatcher`. Lets command helpers marshal onto the UI thread
    /// through the interface without depending on AppController (core-include-dag
    /// Phase 2 — severs the Commands -> AppController include back-edge).
    void PostToMainThread(std::function<void()> fn) override;

    /// Unified Command System registry. See docs/plans/shipped/command-system-plan.md.
    /// Lifetime: created in `Initialize`; the same instance feeds the CLI, the
    /// MCP plugin's tools/list + tools/call, the Lua `commands.invoke` binding,
    /// and the in-app Ctrl+Shift+P palette.
    smatchet::cmd::CommandRegistry& Commands() override;
    const smatchet::cmd::CommandRegistry& Commands() const override;

    /// Scenario runner — feeds from scenario.run / scenario.cancel / scenario.list.
    /// Tick is driven per-frame by SmatchetUI::Draw.
    smatchet::cmd::ScenarioRunner& Scenarios() override;
    const smatchet::cmd::ScenarioRunner& Scenarios() const override;

    /** Path passed to `Initialize` (may be relative to the process working directory). */
    const std::string& GetLocalCacheDbPath() const { return localCacheDbPath_; }
    /** Absolute path for display; falls back to the raw path if resolution fails. */
    std::string GetResolvedLocalCacheDbPath() const;
    /**
     * UI thread: closes SQLite, deletes the cache file (and WAL sidecars), opens a new empty database,
     * clears in-memory tickets, and resets streaming sync state. On success, call `SyncWithBackend`
     * to refill from the tracker.
     */
    VoidResult RecreateLocalCacheDatabase() override;

    /// Bucket-E (ImGui Test Engine) opt-in: ensure a live LocalCacheManager + the
    /// owned offline-queue service exist so test scenarios that exercise the
    /// offline-queue UI (e.g. DataDependentWindowsSmoke case 8) can drive a real
    /// `QueueCreateOffline` write instead of falling back to the empty-guard path.
    /// Opens a throwaway in-memory SQLite cache only when `Cache` is currently unset
    /// (a normal boot already has a live file-backed cache, in which case this is a
    /// no-op beyond ensuring `WireCoreServices()` ran). UI-thread only; intended to
    /// be called from UiTestScenario::OnStart under the
    /// SMATCHET_UITEST_WITH_LOCAL_CACHE=1 env opt-in. Returns true if a live cache is
    /// present on return.
    bool EnsureLocalCacheForUiTest();

#if defined(SMATCHET_WITH_AI)
    /// Phase 3 of ai-chat-claude-desktop-parity. Thin pass-through to
    /// `LocalCacheManager::LoadChatMessages` so the AI panel's UI-side hydration path
    /// can pull persisted chat without needing direct access to the private `Cache`
    /// member. Output vectors are parallel — `outIds[i]` is the SQLite row id for
    /// `outMessages[i]`. No-op (clears both vectors) if `Cache` is unset.
    void LoadAiChatMessages(std::size_t cap, std::vector<AiMessage>& outMessages,
                            std::vector<std::int64_t>& outIds) const;
#endif

    /// Worker-to-UI-thread deferred task queue (BACKLOG_CODE_REVIEW.md §6.1). Post lambdas here from any
    /// thread; SmatchetUI::Draw drains them at the top of each frame. Use instead of ad-hoc atomics.
    MainThreadDispatcher mainThreadDispatcher;

    /// Phase 4b of docs/plans/shipped/smatchet-merge-watcher.md — localhost HTTP receiver for
    /// the merge-watcher daemon's in-app toast notifications. Bound 127.0.0.1:7679;
    /// HTTP runs on cpp-httplib worker thread; toast appends post via
    /// mainThreadDispatcher. Started in Initialize(); stopped at the top of
    /// ~AppController BEFORE the dispatcher's BeginShutdown so in-flight POST
    /// callbacks observe a live dispatcher. Best-effort — bind failure logs WARN +
    /// continues (daemon falls back to Windows native toast).
    /// Full SmatchetMergeWatchNotifyServer header included below (not forward-
    /// declared) so unique_ptr's destructor instantiation sees the complete
    /// type in every TU that includes AppController.h (some consumers stack-
    /// allocate or unique_ptr<AppController>).
    std::unique_ptr<SmatchetMergeWatchNotifyServer> mergeWatchNotifyServer_;

    /** Call from plugins in OnEarlyInit only (before Initialize completes InitLua). */
    void AddAutomationLogSink(std::function<void(const std::string&)> sink) override;
    /** Drop all sinks. Call before destroying plugins to avoid dangling `[this]` captures. */
    void ClearAutomationLogSinks();

    /// Register a sink that receives Lua error messages (automation runner + setup script
    /// failures). Called with a clean message string — no `[LUA]` / `[ERROR]` prefix; sink
    /// is responsible for presentation. Safe to call from any thread (the background
    /// automation worker may invoke it); sinks themselves are expected to be UI-thread-safe
    /// (e.g. via mainThreadDispatcher). Call from OnEarlyInit only.
    void AddAutomationErrorSink(std::function<void(const std::string&)> sink) override;

    /// Atomically reads and clears the "open Scripting window" request that the background
    /// automation worker sets on a Lua error. UI-thread: call once per frame in OnDraw.
    /// Returns true if the plugin should bring the Scripting window to the foreground.
    bool ConsumeScriptingWindowRequest() override;

    /**
     * Optional host callback for launching URLs.
     * Use this when embedding in Unreal (avoids OS-level `system("start")` calls).
     */
    void SetOpenUrlHandler(std::function<void(const std::string&)> handler);

    /** Opens a URL using the handler if set; otherwise falls back to default browser. */
    void OpenUrl(const std::string& url) const;

    /**
     * Optional host callback to hide the embedded overlay (e.g. Unreal plugin toggles Slate visibility).
     * Set by SmatchetImGuiHost; no-op when unset (standalone / tests).
     */
    void SetCloseEmbeddedUiHandler(std::function<void()> handler);
    void CloseEmbeddedUi();
    void SetRequestAppQuitHandler(std::function<void()> handler);
    void RequestAppQuit() const override;

    /** Standalone / embedded host: set so Preferences can start or stop MCP without app restart. */
    void SetRuntimePluginHost(PluginHost* host);
    PluginHost* RuntimePluginHost() const { return runtimePluginHost_; }

#if defined(SMATCHET_WITH_MCP)
    /** Bounded ring buffer of MCP-related actions (thread-safe). */
    void AppendMcpActivity(const std::string& line);
    std::vector<std::string> CopyMcpActivityLog() const override;
    /** MCP HTTP server: any routed request after auth gate (worker threads). */
    void NotifyMcpClientHttpActivity();
    /** @return false if no client request has been recorded yet this process. */
    bool TryGetMcpLastClientHttpActivity(std::chrono::steady_clock::time_point* out) const override;
    /** Increments once per MCP HTTP request after the auth pre-hook (distinct from activity-log lines). */
    std::uint64_t GetMcpHttpTrafficEpoch() const;
#endif

#if defined(SMATCHET_WITH_AI)
    /// Smatchet Assistant — Phase B. Owns the worker thread, conversation history, and the
    /// per-turn cancel atom. Constructed in `Initialize`; destroyed at the *top* of
    /// `~AppController` before any other join so its worker observes a live
    /// `mainThreadDispatcher` while in-flight callbacks drain.
    AiAssistantController& GetAiAssistantController();
    /// Phase B safety probe — false during early init before the controller is wired.
    /// Out-of-line (pImpl #19b): `aiAssistant_` now lives in the src-only AppController::Impl.
    bool HasAiAssistantController() const;
#endif

    /// Always-on no-op stubs (gated only on the implementation side via SMATCHET_WITH_AI).
    /// Phase E Lua glue calls these unconditionally; the OFF build silently no-ops so the
    /// Lua binder TU does not need a parallel SMATCHET_WITH_AI gate. Phase B: delegate to
    /// `aiAssistant_` when present; Phase C extends with real context-block plumbing.
    void AddAiContext(const AiContextBlock& block);
    void ClearAiContext();
    std::vector<AiContextBlock> GetAiContext() const;
    /// Equivalent to `aiAssistant_->Submit(prompt, {})` plus a UiDrawSession turn-gen bump.
    /// UI-thread only.
    void PromptAi(const std::string& prompt);

    /**
     * Optional host callback for showing tracker attachments inside Unreal.
     * If set, Smatchet will download the attachment bytes and save them to a local temp file,
     * then call this handler with the file path.
     */
    // Fan-in Phase 3b (docs/plans/appcontroller-fan-in.md): AttachmentDescriptor + the host-callback
    // aliases moved to Types/AttachmentTypes.h; re-exported in-class so consumers keep naming
    // AppController::AttachmentDescriptor / AppController::Attachment*Handler unchanged.
    using AttachmentViewerHandler = ::AttachmentViewerHandler;
    void SetAttachmentViewerHandler(AttachmentViewerHandler handler);
    using AttachmentPreviewHandler = ::AttachmentPreviewHandler;
    void SetAttachmentPreviewHandler(AttachmentPreviewHandler handler);
    using AttachmentDescriptor = ::AttachmentDescriptor;
    using AttachmentCollectionHandler = ::AttachmentCollectionHandler;
    void SetAttachmentCollectionHandler(AttachmentCollectionHandler handler);
    void ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments);

    /**
     * Optional host hook for native multi-file open (new-issue attachments, etc.).
     * Invoked on the UI thread; implementation may block (e.g. Win32 IFileDialog).
     * If unset, RequestOpenFilePaths completes with an empty vector.
     */
    using OpenFilePathsHandler = ::OpenFilePathsHandler; // moved to Types/AttachmentTypes.h (fan-in Phase 3b)
    void SetOpenFilePathsHandler(OpenFilePathsHandler handler);
    void RequestOpenFilePaths(bool allowMultiple, const std::string& initialDirectoryUtf8,
                              std::function<void(std::vector<std::string>)> onComplete) const;

    /**
     * Open an attachment (image/pdf/etc) without requiring Basic Auth headers in the browser.
     * - If AttachmentViewerHandler is set: downloads to a temp file and calls the handler.
     * - Otherwise, for image mime types: downloads and offers in-app preview handler.
     * - If no host/in-app handler path is available: falls back to OpenUrl(url).
     */
    void OpenAttachment(const std::string& url, const std::string& filename, const std::string& mimeType) override;
    /** Forwarder to AttachmentAppUpdateService::OpenAttachmentInSystemViewer; see that
     *  header for the outcome contract. Blocking network I/O. */
    // SMATCHET_DEVIATION(rule=duplication; reason=forwarder mirrors service header; owner=ui; revisit=dup-scoping)
    bool OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename, const std::string& mimeType,
                                      std::string* outError = nullptr, bool* outFellBackToUrl = nullptr);
    bool DownloadAttachmentForPreview(const std::string& url, const std::string& filename, const std::string& mimeType,
                                      std::string* outError) override; // default (=nullptr) lives on IAppAttachments
    std::string GetAppVersion() const override;
    std::string GetGitHubReleaseRepo() const override;
    AppUpdateInfo CheckForAppUpdate(bool includePrerelease) const override; // default (=false) lives on IAppMeta
    /// Downloads + launches the installer. Blocking — callers must dispatch this on a worker
    /// thread via `LaunchBackgroundTask`. The optional `cancelFlag` is polled inside the cpr
    /// write callback; when set to `true` the download aborts cleanly and the partial file is
    /// removed. On cancel returns `VoidResult::Err("Download cancelled.")`; `VoidOk()` on launch.
    VoidResult DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl, const std::string& assetName,
                                                std::shared_ptr<std::atomic<bool>> cancelFlag = {}) const;

    /// Opens the libraries + registers the binding surface on the member `lua` state.
    /// InitLuaCore / InitLuaUi / PrepareFreshLuaState / ReplayActiveSetupScripts moved onto
    /// AppController::Impl (hardening #19c) — they are sol-typed and no longer visible here.
    void InitLua();

    /// Resolve a script basename against the configured scripts directory. Rejects traversal /
    /// absolute / drive-qualified names, and fails closed (empty) when no scripts directory is
    /// configured — there is deliberately no cwd-relative fallback (an untrusted working
    /// directory's .lua files must never resolve for execution).
    std::string ResolveLuaScriptPath(const std::string& filename) const;
    /** Basenames of `*.lua` files in the configured scripts directory (non-recursive; empty when
     * no scripts directory is configured — no cwd-relative fallback). */
    std::vector<std::string> ListLuaScriptFiles() const;

    // --- First-run Lua script consent gate ---------------------------------------------------
    // A Scripts/*.lua file may only be executed once the user has approved its exact content
    // (path + sha-256). New or content-changed scripts are BLOCKED (fail closed) so a shared /
    // silently-dropped script cannot run the app's high-privilege Lua bindings unseen. See
    // docs/agent-rules and the LuaScriptConsent.h pure core.

    /// Consent decision for a resolved Scripts/*.lua path. Reads the file, fingerprints it, and
    /// checks it against the persisted approvals (honoring the config kill-switch + the
    /// SMATCHET_LUA_CONSENT=off env override). Returns true if execution may proceed. On refusal
    /// returns false with a user-facing reason in outReason; when interactive it also raises the
    /// scripting-window request so the UI can surface the approval affordance. Fail-closed: an
    /// unreadable / oversized file is refused.
    bool IsLuaScriptConsented(const std::string& resolvedPath, bool interactive, std::string& outReason);

    /// Approve a Scripts/*.lua file BY BASENAME at its current content, persisting the fingerprint
    /// so subsequent runs are permitted (until the content changes). Returns false with outError
    /// on an invalid name / unreadable file. Deliberate user action (CLI / console / palette).
    /// IAppAutomation facet override.
    VoidResult ApproveLuaScript(const std::string& scriptName) override;

    /// Revoke any approval for a Scripts/*.lua basename, so its next run is blocked again.
    /// IAppAutomation facet override.
    VoidResult RevokeLuaScript(const std::string& scriptName) override;

    /// Resolved paths of every currently-approved Lua script. IAppAutomation facet override.
    std::vector<std::string> ListApprovedLuaScriptPaths() const override;

    /// One-time migration: if the consent list has never been initialized, seed it with the
    /// fingerprints of the scripts that already exist in the scripts directory (trust-on-adoption
    /// so existing setups keep working) and mark it initialized. No-op afterwards. Runs at startup.
    void SeedLuaScriptConsentIfNeeded();

    /**
     * Resolve a URL or local path for field icons / Lua `imgui.image`.
     * Allows http(s) URLs; local files must lie under the Lua scripts directory or the runtime asset directory.
     */
    std::string ResolveFieldIconAssetPath(const std::string& pathOrUrl) const;

    // processAll=true runs the script across every loaded ticket (ignores selectedIds).
    // With an empty selectedIds and processAll=false the job refuses to run (Issue #824):
    // no silent mass-modify, no silent no-op.
    // processAll default is intentionally mirrored on BOTH IAppAutomation::RunAutoScript and
    // this override: LuaConsolePlugin.cpp calls through a concrete AppController& with the
    // 2-arg form (static binding needs the default here), while automation.* calls through the
    // facet (needs it there). Keep the two in sync if the default ever changes.
    void RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds,
                       bool processAll = false) override;
    void RunFlatScriptAsync(const std::string& scriptPath) override;

    std::string GetAutomationScriptContent();
    bool SaveAutomationScriptContent(const std::string& content, std::string& outError);

    /** Run a Lua file once (e.g. SmatchetHooks.lua) to register UI hooks; errors go to automation log sinks. */
    void RunLuaSetupScript(const std::string& scriptPath) override;

    /** Present with or without Lua build; no-op / empty when `SMATCHET_WITH_LUA_AUTOMATION` is off. */
    std::vector<std::string> GetLuaTicketActionNames() const;
    void ExecuteLuaTicketAction(const std::string& name, const std::string& issueId);
    std::vector<std::string> GetLuaGlobalActionNames() const override;
    void ExecuteLuaGlobalAction(const std::string& name);
    /**
     * Run a one-off Lua chunk from the automation UI (same globals as hooks: smatchet, ui, tracker, …).
     * On failure sets @p outError; on success clears @p outError and may set @p outResultSummary from the
     * first return value (short string / JSON, truncated when long).
     */
    bool ExecuteLuaConsoleSnippet(const std::string& code, std::string& outError,
                                  std::string& outResultSummary) override;
    /** Lua `register_field_icon_map`; returns false when Lua automation is disabled. */
    bool TryGetFieldIconMapTarget(const std::string& fieldId, const TrackerField* field, const std::string& rawValue,
                                  std::string& outPathOrUrl) const override;

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    // NOTE (hardening #19c): every sol-typed Lua binding method (LuaGetTicketBind /
    // LuaDecodeJsonBind / LuaCreateIssueBind / LuaMcpRegisterToolBind / the UI binds /
    // ParseMcpToolDef / GetLuaMcpTools / ResolveLuaFieldProvider / Init* / the automation
    // quartet) moved onto AppController::Impl (AppControllerImpl.h) behind ILuaBindingHost.
    // Only the sol-FREE Lua API stays on AppController; the Impl forwards to these via app_.

    /** Active-ticket snapshot for `smatchet.get_active_tickets` (sol-free; Impl forwards here). */
    std::vector<CachedTicket> LuaGetActiveTicketsBind();
    void ClearLuaTicketContextGlue();

    std::string ExecuteLuaMcpTool(const std::string& name, const std::string& paramsJson, std::string& outError);
    std::string ExecuteLuaSnippetForMcp(const std::string& code, const nlohmann::json& args, std::string& outError);
    std::string ExecuteLuaScriptForMcp(const std::string& scriptName, const nlohmann::json& args,
                                       std::string& outError);
    void DrawLuaWindows();

    /// DrawLuaWindows helper — (re-)record one Lua window's draw fn into its cached cmd-list,
    /// updating the cache generations and error state. Runs only on a dirty / gen-mismatch frame.
    void RecordLuaWindow(smatchet::lua::LuaWindowEntry& w, std::uint64_t curDataGen, std::uint64_t curProviderGen);

    /// TryRenderCachedLuaField helper — surface a Lua cell-provider error to the persistent
    /// errors panel, scrolling log, and auto-open the Scripting window.
    void SurfaceLuaFieldError(const std::string& fieldName, const std::string& issueId, const std::string& errMsg);
#endif

    /**
     * Recorded-cmd-list cell entry. When a Lua provider is registered for `fieldId` (or for
     * the display name resolved via `fieldMeta->Name`), invokes it on cache miss to build a
     * draw recording, then replays the cached recording on every subsequent frame until one
     * of the cache-key inputs (rawValue, fieldName, intAvailWidth, isReadOnly, providerGen)
     * changes. The 6th + 7th provider args are `isReadOnly` (combined catalog + editmeta +
     * grid-level `allowEdits`) and the `draw` recorder.
     *
     * Declared outside the SMATCHET_WITH_LUA_AUTOMATION guard so unconditional call sites
     * (TicketFieldEditor) link in the stub build; stub returns false. See
     * docs/plans/shipped/lua-recorded-cmd-list.md § Removal of legacy.
     *
     * @return true if the handler ran and returned a Lua-truthy value (cell fully handled).
     */
    bool TryRenderCachedLuaField(const std::string& fieldId, const CachedTicket& ticket, const std::string& rawValue,
                                 float availWidth, const TrackerField* fieldMeta, bool allowEdits);

    /**
     * Bumps `luaWindowDataGen_`. Cached Lua windows whose `cachedDataGen` lags the bump
     * re-record on their next paint. Stub is empty (no Lua → no windows to dirty).
     *
     * Single hook site: `RefreshLocalData()` in `AppController_CatalogAndFieldEdit.cpp`.
     * `TicketSyncService` coalesces a fetch session's many `ApplyIssueFetchPack` calls into
     * one bump at session end. See plan §Invalidation strategy.
     */
    void NotifyLuaTicketDataChanged();

    /// Lua-driven window dirty bump. Safe from any thread: off-UI hops the dispatcher; on-UI
    /// mid-iteration enqueues onto pendingLuaWindowOps_ (drained after the loop, same frame).
    /// Declared unconditionally so non-Lua call sites (future MCP / Unreal hooks) can also
    /// drive window invalidation; stub is empty in the no-Lua build.
    void LuaUiInvalidateWindowBind(const std::string& name);

    /// Scenario hook: register a globally-visible Lua function as a cached field-display
    /// provider for `fieldId`. Looks up `_G[luaFnName]` and binds it equivalently to
    /// `register_field_display_cached(fieldId, luaFnName)`. Used by perf / fuzz scenarios
    /// that need to exercise a Lua-driven render path without requiring manual script edits.
    /// If the function is missing, probes candidate paths (luaScriptsDirectory_, CWD-relative
    /// scripts/, walk up 3 levels) for `SmatchetHooks.lua` plus every name in `extraScripts`,
    /// then `lua.script_file`s the first hit in the global env so subsequent `_G[name]`
    /// lookup succeeds. Returns false on missing-fn / not-callable / Lua-disabled; populates
    /// `outError`. No-op stub in the no-Lua build.
    bool ScenarioRegisterLuaCachedProvider(const std::string& fieldId, const std::string& luaFnName,
                                           const std::vector<std::string>& extraScripts,
                                           std::string& outError) override;
    /// Convenience overload — equivalent to passing an empty `extraScripts`.
    bool ScenarioRegisterLuaCachedProvider(const std::string& fieldId, const std::string& luaFnName,
                                           std::string& outError) override;
    /// Inverse of `ScenarioRegisterLuaCachedProvider`. Restores the user-side provider that
    /// was displaced at register time (if any), or erases the entry if no prior existed. Also
    /// drops every cache entry for that field so the restored provider re-records cleanly.
    /// No-op if not registered via the scenario surface, or in the no-Lua build.
    void ScenarioUnregisterLuaCachedProvider(const std::string& fieldId) override;

    /// Scenario hook: clear every `luaFieldCache_` entry so subsequent cells re-record. Used
    /// by fuzz scenarios that need to exercise the recorder path each frame (otherwise the
    /// cache hit-rate becomes 100% after first paint and only the initial visible cells are
    /// fuzzed). No-op stub in the no-Lua build.
    void ScenarioInvalidateLuaFieldCache() override;

    // LuaUiInvalidateFieldCacheBind(sol::optional<std::string>, sol::optional<std::string>) moved
    // onto AppController::Impl (#19c) — its sol::optional signature can't be declared here.

    /**
     * Sync issues from the tracker into the local cache.
     * Pass the in-memory UI config + views store when syncing from the app so JQL/fields match
     * the active view without relying on an immediate disk round-trip.
     */
    void SyncWithBackend(const TrackerConfig* configOverride = nullptr,
                         const ViewsStore* viewsOverride = nullptr) override;

    /** Process queued streaming ticket batches on the UI thread (frame-budgeted). */
    void TickStreamingApply();

    /** Clears the live ticket-sync warning banner (e.g. before kicking off a background fetch). */
    void ClearLastTrackerTicketSyncWarning();

    /**
     * Runs FetchIssues under an internal mutex (safe with concurrent UI-triggered syncs).
     * Does not touch the SQLite cache; pair with ApplyIssueFetchPack on the UI thread.
     */
    TrackerIssueFetchPack FetchIssuesForActiveView(const TrackerConfig* configOverride = nullptr,
                                                   const ViewsStore* viewsOverride = nullptr) override;

    /** Merges fetch results into the local cache and updates connectivity banners. */
    void ApplyIssueFetchPack(TrackerIssueFetchPack pack);

    void RefreshLocalData() override;
    // Generation-checked refreshes (issue #1081) go through RefreshLocalDataCheckedImpl_,
    // private on purpose: every checked caller must pass the GridLiveContext it latched the
    // generation from (UpdateTicket inline; replay workers via the friend
    // GridContextDepsAdapter). A ctx-less public overload re-resolved focusedContext() at
    // apply time — per-context generation counters can be equal-by-coincidence across panes,
    // passing the gate while focus moved.
    /** Reload ActiveTickets from cache and kick per-issue-type editmeta warmup (same tail as SyncWithBackend). */
    void RefreshLocalDataAndWarmIssueTypeMeta();

    void UpdateTicket(const CachedTicket& ticket);

    std::vector<CachedTicket> GetActiveTickets() const;
    /** Cheap read: shared_ptr to last published ticket list (thread-safe with MCP / workers). */
    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const override;
    std::uint64_t GetActiveTicketsRevision() const { return focusedContext().ActiveTicketsRevision.load(); }
    /// De-inlined as of item 11 Phase 1C: the streaming-sync state lives on TicketSyncService.
    /// Defined in AppController.cpp where TicketSyncService.h is included; delegates to
    /// `ticketSync_->IsActive()`.
    bool IsStreamingSyncActive() const;

    /** Bumped when the field catalog changes (fetch, error clear, etc.); UI sort cache should invalidate. */
    std::uint64_t GetFieldCatalogRevision() const { return fieldCatalog().TrackerFieldCatalogRevision.load(); }

    bool RefreshFieldCatalog(const TrackerConfig& cfg) override;
    /** Refetch the catalog scoped to a specific project. The project key is plumbed to
     *  the fetcher via a transient capture on the backend cfg snapshot (see TrackerFieldCatalog
     *  and PlaneClient single-capture sites) and used by SetFieldCatalog to land the snapshot
     *  under the right per-project cache entry. Empty projectKey ≡ unscoped, identical to the
     *  no-argument overload. */
    bool RefreshFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey);
    bool FetchFieldCatalog(const TrackerConfig& cfg, TrackerFieldCatalogResult& outCatalog,
                           std::string& outError) const;
    /** Scoped fetch overload. Used by the startup / view-switch catalog refresh in SmatchetUI
     *  to plumb the active view's project key into createmeta + status enrichment so
     *  priority/status dropdowns render with their option lists. Empty projectKey ≡ unscoped,
     *  identical to the 2-arg overload. */
    bool FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,
                           TrackerFieldCatalogResult& outCatalog, std::string& outError) const;

    /** Resolve a raw tracker value (e.g. accountId, label UUID) to a display name. */
    std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                    const std::string& value) const;

    std::string BuildIssueBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const;
    static std::string BuildJqlSearchUrl(const TrackerConfig& cfg, const std::string& jql);

    const std::vector<TrackerField>& GetAvailableFields() const override { return fieldCatalog().AvailableFields; }
    const std::vector<TrackerComponent>& GetAvailableComponents() const { return fieldCatalog().AvailableComponents; }
    /// Last-fetched user catalog. May be empty before the first catalog fetch completes
    /// or when the active backend doesn't surface a users endpoint.
    const std::vector<TrackerUser>& GetAvailableUsers() const { return fieldCatalog().AvailableUsers; }
    const std::string& GetFieldCatalogError() const override { return fieldCatalog().LastTrackerFieldCatalogError; }
    const std::string& GetFieldCatalogWarning() const { return fieldCatalog().LastTrackerFieldCatalogWarning; }
    /** Set when a live JQL refresh failed with a transport-style error; UI may show cached tickets.
     *  Returns a const reference (callers bind a reference); delegates to ConnectivityMonitorService.
     *  De-inlined (Phase 3) — the body lives in AppController.cpp where the service is a complete type. */
    const std::string& GetLastTicketSyncWarning() const override;

    /**
     * One banner for field-catalog error/warning, ticket-list cache warning, and optional session note
     * (e.g. Views dashboard users-fetch warning). Prefer this over separate `GetFieldCatalogWarning` /
     * `GetLastTicketSyncWarning` lines in headers.
     */
    TrackerConnectivityBannerForUi
    GetTrackerConnectivityBannerForUi(const std::string* sessionCatalogNote = nullptr) const;

    /** Last outcome of periodic tracker probe (UI thread). Defined in Types/ConnectivityTypes.h. */
    using TrackerConnectivityState = ::TrackerConnectivityState; // moved (fan-in Phase 3)
    /** Rate-limited background probe; updates connectivity state and recovery latch. */
    void TickTrackerConnectivityMonitor(const TrackerConfig& cfg);
    /** Latest reachability from background probe (or after a successful live backend request).
     *  De-inlined (Phase 3) — delegates to ConnectivityMonitorService (a complete type only in the .cpp). */
    TrackerConnectivityState GetLastTrackerConnectivityState() const override;
    /**
     * One-shot: true when reachability improved to authenticated-reachable (including from
     * transport-down, service-unavailable, or auth/config errors, and cold-start when a catalog
     * offline banner is still set). Clears ticket sync + field-catalog warnings and nudges
     * offline replay timers. UI should run catalog refetch + `SyncWithCurrentView` on the same frame.
     */
    bool ConsumeTrackerConnectivityRecovery();
    /**
     * One-shot: true after a successful live `SyncWithBackend` issue fetch cleared a stale offline
     * field-catalog banner. UI should set `triggerCatalogRefetch` (same as connectivity recovery).
     */
    bool ConsumeFieldCatalogRefetchAfterLiveTicketSync();
    /**
     * Main-thread: Dispatches any deferred UI notifications or results from background
     * tracker HTTP work (including from background workers). Call once per frame
     * early in `SmatchetUI::Draw`.
     */
    /** @return true if a deferred notify was applied this call (live tracker request succeeded). */
    bool ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny();
    void SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                         const std::string& error, bool errorTransient = false);
    void SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                         std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error,
                         bool errorTransient = false);
    /// SetFieldCatalog helper — handle the non-empty-error branch (transport-error
    /// snapshot restore vs hard catalog clear) and publish the matching warning/error
    /// state. `catalogPlane` mirrors the caller's tracker-kind classification.
    void HandleFieldCatalogError(const std::string& error, bool errorTransient, const std::string& catalogCacheKey,
                                 bool catalogPlane);
    /// Pin the project key the next SetFieldCatalog() snapshot saves under. The grid's scoped
    /// catalog fetch resolves a project from the active-view JQL but applies the result through
    /// SetFieldCatalog() (not RefreshFieldCatalog()), so without this hint the scoped result would
    /// persist under the unscoped ("") key and component options would be lost on next startup.
    /// Pass the same project key the fetch was scoped to (empty = unscoped). Guarded by
    /// availableFieldsMutex_.
    void SetCurrentCatalogProject(const std::string& projectKey);
    /// Replace the cached user list (e.g. after a successful `FetchUsers`). Surfaces to
    /// `GetAvailableUsers()` for the JQL autocomplete to suggest assignees / reporters.
    /// Idempotent; safe to call with an empty vector to clear the cache.
    void SetAvailableUsers(std::vector<TrackerUser> users);

    const std::vector<TrackerIssueTypeCreateMeta>& GetTrackerIssueTypeCreateMeta() const {
        return fieldCatalog().AvailableIssueTypeMeta;
    }

    /** Read-only accessor used by UI sites (e.g. `ResolveProjectForDraft`) to call
     *  `ITrackerConnectivity::ExtractProjectFromQuery` / `GetTrackerType`. May be null before
     *  `Initialize` has wired up the factory. Do not retain the pointer past the current frame. */
    // All reads of the `Backend` member go through std::atomic_load and all writes through
    // std::atomic_store (ADR 0012): the slot is reassigned live on a tracker swap, and a
    // shared_ptr *instance* is not itself thread-safe to copy/assign concurrently (C++14).
    const ITrackerBackend* GetTrackerBackend() const { return std::atomic_load(&focusedContext().Backend).get(); }
    // Non-const accessor for callers that invoke mutating client methods (e.g.
    // ListProjects() which populates a per-client in-memory cache).
    ITrackerBackend* GetTrackerBackendMutable() { return std::atomic_load(&focusedContext().Backend).get(); }
    /** Strong (shared) handle to the active backend, for OFF-THREAD work that must
     *  outlive a live tracker swap. `Backend` is reassigned live on a tracker change
     *  (`SyncWithBackend`→`SetBackend`), which frees the old object; a worker that
     *  captured only a raw pointer would dangle. Capture this `shared_ptr` instead so
     *  the old backend stays alive until the worker drops it. Atomic-loaded so the
     *  snapshot itself can't race the swap. See ADR 0012. */
    std::shared_ptr<ITrackerBackend> BackendShared() const { return std::atomic_load(&focusedContext().Backend); }

    // ---- Create issue flow -------------------------------------------------

    /**
     * Build a draft seeded from the last ticket currently displayed + the
     * configured TrackerConfig defaults. Safe to call from the UI thread.
     */
    IssueDraft BuildDraftFromLastTicket(const TrackerConfig& cfg) const;

    /**
     * Resolve the required-field set for a draft using cached createmeta.
     * Falls back to the hard minimum (project/issuetype/summary) if unknown.
     */
    RequiredFieldSet GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                         const std::string& issueTypeName) const;

    /**
     * Fire-and-forget create. Seeds the cache with the new issue on success and
     * publishes a refreshed snapshot. Returns a future so bulk-import callers
     * can await per-row completions.
     *
     * `cancel` is a cooperative cancel handle (WS-A): when signalled before the
     * worker reaches the create, the worker returns a benign cancelled result
     * without running the network create or the expensive post-create refresh /
     * hydration — so a bulk-import `.clear()` or app shutdown drains promptly.
     * Defaulted so non-bulk callers (Lua) need not supply one.
     */
    // CancelToken default mirrored on IAppTicketMutations::CreateIssueAsync (kept here too:
    // concrete-typed callers — AppController_LuaBindings, SmatchetNewIssueDraftUi — use the
    // 1-arg form, which static-binds this declaration's default).
    std::future<IssueCreateResult>
    CreateIssueAsync(const IssueDraft& draft, smatchet::ui::CancelToken cancel = smatchet::ui::CancelToken()) override;

    /**
     * Persist `draft` to SQLite and return the queued row id. Useful when the
     * user wants to stage creates before going online, or when a create fails
     * due to connectivity errors. Replayed by `TickOfflineCreates`.
     */
    std::int64_t QueueCreateOffline(const IssueDraft& draft) override;

    /**
     * Replay any queued offline creates. No-op when the queue is empty or the
     * backend is unreachable. Intended to be polled from the main tick.
     */
    void TickOfflineCreates() override;

    /** Current depth of the offline create queue (SQLite row count). */
    size_t GetPendingCreateCount() const override;
    /** Active offline create rows (`pending_creates`), oldest first. */
    std::vector<PendingCreate> GetPendingCreates() const override;
    size_t GetDeadPendingCreateCount() const;
    std::vector<DeadPendingCreate> GetDeadPendingCreates() const override;

    using DeadLetterRestoreSummary = ::DeadLetterRestoreSummary; // moved to Sync/OfflineQueueTypes.h
    /** Move selected dead-letter rows back to the active offline queue (attempts reset to 0). */
    DeadLetterRestoreSummary RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds);
    /** One-shot startup message from legacy max-attempt pending drop; empty if none. */
    std::string TakeLegacyPendingStartupBanner();

    using DeadLetterDeleteSummary = ::DeadLetterDeleteSummary; // moved to Sync/OfflineQueueTypes.h
    /** Permanently remove dead-letter rows by `pending_creates_dead.dead_id`. */
    DeadLetterDeleteSummary DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds) override;

    using PendingQueueDeleteSummary = ::PendingQueueDeleteSummary; // moved to Sync/OfflineQueueTypes.h
    /** Permanently remove active offline-queue rows by `pending_creates.id`. */
    PendingQueueDeleteSummary DeletePendingCreates(const std::vector<std::int64_t>& pendingIds);

    /** Safe field families for offline-queued field edits (transport failures only). */
    static bool FieldEditSupportsOfflineQueue(const TrackerField& field);

    /**
     * Persist a tracker field payload for later replay when connectivity returns.
     * @param fieldsPayloadJson JSON object map (field id -> backend-specific value).
     */
    std::int64_t QueueFieldEditOffline(const std::string& issueKey, const std::string& fieldId,
                                       const std::string& fieldsPayloadJson, std::string& outError,
                                       const std::string& originalRichValue = std::string(),
                                       const std::string& originalValue = std::string(), bool hasOriginalValue = false);

    /** Replay queued offline field edits (rate-limited; called from UI tick). */
    void TickOfflineFieldEdits() override;

    std::vector<PendingFieldEditRecord> GetPendingFieldEdits() const override;
    std::vector<DeadPendingFieldEdit> GetDeadPendingFieldEdits() const override;
    /// Replace the queued payload with a user-resolved version and clear the conflict flag.
    /// The edit will be retried on the next TickOfflineFieldEdits pass. `kind` (text|scalar|
    /// unverified, per ADR-0016) selects how the resolution is applied: `text` reconverts
    /// `resolvedValue` Markdown→ADF/HTML via `richKind` into the payload key; `scalar` writes
    /// `resolvedValue` into the payload key verbatim (no conversion); `unverified` ("Force Mine")
    /// ignores `resolvedValue`, replays the existing queued payload unchanged, and only clears
    /// the conflict state + bases.
    void ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedValue, const std::string& richKind,
                                  const std::string& kind = std::string("text"));

    using PendingFieldEditDeleteSummary = ::PendingFieldEditDeleteSummary; // moved to Sync/OfflineQueueTypes.h
    PendingFieldEditDeleteSummary DeletePendingFieldEdits(const std::vector<std::int64_t>& ids);

    using DeadFieldEditDeleteSummary = ::DeadFieldEditDeleteSummary; // moved to Sync/OfflineQueueTypes.h
    DeadFieldEditDeleteSummary DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds) override;

    using DeadFieldEditRestoreSummary = ::DeadFieldEditRestoreSummary; // moved to Sync/OfflineQueueTypes.h
    /** Move selected dead-letter field-edit rows back to the active queue (attempts reset to
     * 0), each keeping its ORIGINAL `backend_key` — the field-edit twin of
     * `RestoreDeadPendingCreates`. */
    DeadFieldEditRestoreSummary RestoreDeadPendingFieldEdits(const std::vector<std::int64_t>& originalIds);

    /**
     * Background-fetch issues by key (Jira search) and merge into the local cache.
     * Used so bulk-import update rows can show field diffs when keys are outside the current JQL.
     */
    void PrefetchIssueTicketsForKeys(const std::vector<std::string>& issueKeys, bool includeAlreadyActive = false);
    bool IsBulkImportPrefetchInFlight(const std::string& issueKey) const;

    const TrackerField* FindFieldById(const std::string& fieldId) const override;

    /** Component options valid for one Jira project key (e.g. "PROJ"), warmed async for cross-project
     *  grid views. Returns a by-value copy taken under availableFieldsMutex_; empty when the project
     *  has not been warmed yet (caller falls back to the global components catalog). */
    std::vector<TrackerFieldOption> GetComponentOptionsForProject(const std::string& projectKey) const;

    /** True once a component fetch for `projectKey` has SUCCEEDED (the key is present in
     *  projectComponentOptions_), regardless of how many components it returned. Lets the editor
     *  distinguish "not yet loaded" (show "Loading components…") from "loaded but genuinely empty"
     *  (show "(no options)"). Read under availableFieldsMutex_. */
    bool IsProjectComponentsLoaded(const std::string& projectKey) const;

    /** Lazily fetch one Jira project's component options into projectComponentOptions_ when the
     *  eager warm (WarmIssueTypeEditMetaAtStartAsync) missed it (race, or a project loaded after the
     *  warm ran). Non-blocking: checks the per-project map + in-flight set under availableFieldsMutex_
     *  and launches a background fetch; the HTTP runs on the worker. No-op when projectKey is empty,
     *  already cached, or already in-flight. The components editor calls this instead of falling back
     *  to the cross-project global catalog union. */
    void EnsureProjectComponentsLoaded(const std::string& projectKey);

    /**
     * Per-issue tracker edit metadata: true if the field may be edited for this issue.
     *
     * For Jira, we handle special cases:
     * `status`: never allow direct edit via field editmeta (Jira does not list status
     * like a normal settable field; updates use transitions).
     * `priority`: if editmeta is loaded but omits `priority`, allow edit
     * (Jira omits it inconsistently).
     *
     * Returns true when editmeta is not loaded yet (optimistic) or for
     * non-Jira backends (e.g. Plane). After a failed editmeta fetch for an issue, returns false for fields not in the
     * bypass list.
     * @param fieldMeta optional catalog row for fieldId (avoids lookup; same as nullptr + catalog).
     */
    bool CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                              const TrackerField* fieldMeta = nullptr,
                              const std::string* issueTypeKeyOverride = nullptr) const;

    // Field-edit pipeline delegators — forward to `fieldEdit_` (FieldEditPipelineService, god-object
    // decomposition Phase 2). Signatures preserved verbatim from the pre-extraction surface
    // (SubmitFieldEdit's especially — the Lua forwarder + BuiltinCommands depend on it).
    // ApplyFieldUpdateWithEditMetaRetry + SubmitSprint/TimetrackingFieldEditNetworkOnly had no
    // external callers and are now service-private (not re-exposed here).
    VoidResult SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                               const std::vector<std::string>& rawValues) override;
    // clang-format off
    // SMATCHET_DEVIATION(rule=duplication; reason=these field-edit delegator declarations are BY DESIGN a verbatim signature mirror of FieldEditPipelineService's SubmitFieldEdit/SubmitFieldEditNetworkOnly (the god-object decomposition Phase 2 forwarders — "Signature preserved verbatim" on both sides); the pre-existing structural clone only crossed the delta-scanner threshold because adding `override` to SubmitFieldEdit (fan-in Phase 5) re-hashed the token window; owner=orchestrator; revisit=if the delegators are ever removed or the service signatures diverge)
    // clang-format on
    FieldEditResult SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& rawValues,
                                               const std::string& originalEstimateSnapshot,
                                               const std::string& remainingEstimateSnapshot,
                                               const std::string& issueTypeKeySnapshot);

    /**
     * Build the Jira fields payload + optimistic display map without calling the network.
     * Used when a network save failed with a transport error and the edit should be queued offline.
     */
    bool TryPrepareOfflineFieldEdit(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues,
                                    const std::string& originalEstimateSnapshot,
                                    const std::string& remainingEstimateSnapshot,
                                    const std::string& issueTypeKeySnapshot, FieldEditResult& outResult,
                                    std::string& outFieldsPayloadJson, std::string& outError);
    VoidResult ApplyFieldEditResult(const std::string& issueId, const FieldEditResult& result);
    /** Best-effort async warmup so edit controls can reflect per-issue permissions sooner. */
    void WarmIssueEditMetaAsync(const std::string& issueId);

    // Editmeta-cache delegators — forward to `editMeta_` (EditMetaCacheService, god-object
    // decomposition Phase 1). Signatures preserved verbatim from the pre-extraction surface; see
    // EditMetaCacheService.h for the issueTypeKeyOverride / configSnapshot contract and the
    // VoidResult (Ok / Err) semantics.
    // SMATCHET_DEVIATION(rule=duplication): this delegator block MUST mirror EditMetaCacheService.h's
    // public signatures verbatim (that is the god-object-decomposition contract — the public surface
    // is unchanged by the extraction); abstracting across the two would defeat the mirror's purpose.
    VoidResult EnsureIssueEditMetaLoaded(const std::string& issueId, const std::string* issueTypeKeyOverride = nullptr,
                                         const TrackerConfig* configSnapshot = nullptr);
    VoidResult RefreshIssueEditMeta(const std::string& issueId, const std::string* issueTypeKeyOverride = nullptr);
    void InvalidateIssueEditMeta(const std::string& issueId);
    void PruneEditMetaCacheToActiveTickets();
    /** @param trackerCfgForWorker credentials/settings copy for background fetch (never ConfigManager::Load inside
     * worker). */
    void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker);

    Result<std::vector<TrackerUser>> FetchIssueWatchers(const std::string& issueKey) const override;

    // Returns VoidResult (has_value() on success; error() carries the user-facing message). Plain
    // method — no IApp* interface mirror. Guard ordering + strings are pinned by the pure seam in
    // Tracker/CollaborationPreconditionPure.h (CollaborationPreconditionPure.test.cpp).
    VoidResult AddIssueWatcher(const std::string& issueKey);

    Result<TrackerIssueVotes> FetchIssueVotes(const std::string& issueKey) const override;

    Result<std::vector<TrackerUser>> SearchUsersByQuery(const std::string& query) const override;
    Result<std::vector<TrackerUser>> FetchUsersByAccountIds(const std::vector<std::string>& accountIds) const override;

    VoidResult AddIssueCommentPlain(const std::string& issueKey, const std::string& plainText) override;

    /// issue-comments PR-A — off-UI read wrapper around
    /// `ITrackerCollaboration::FetchIssueComments`. Latches the focused backend,
    /// returns the backend-agnostic comment list on success. No read-only guard
    /// (pure read). `error()` carries the user-facing message on no backend / no
    /// collaboration support / backend error.
    Result<std::vector<TrackerIssueComment>> FetchIssueComments(const std::string& issueKey);

    /// issue-comments fix (#1291, extended) — push a freshly-fetched comment thread into the
    /// cached ticket: the numeric `comments` count AND the flattened `comment` tooltip blob
    /// (FormatCommentBlob), so the grid Comments cell and its hover tooltip update after a
    /// modal fetch / post or a first-hover lazy fetch, without a full re-sync. The blob is
    /// built HERE, once per fetch — the grid cell only references the stored string per
    /// frame. UI-thread only (mirrors the optimistic-update path). No-op when both values
    /// are unchanged.
    void UpdateCachedCommentsFromThread(const std::string& issueId, const std::vector<TrackerIssueComment>& comments);

    VoidResult SubmitWorklog(const std::string& issueId, const std::string& timeSpent, const std::string& timeRemaining,
                             const std::string& adjustEstimate, const std::string& workDescription,
                             const std::string& startedDate) override;

    // Returns VoidResult (has_value() on success; error() carries the user-facing message). Plain
    // method — no IApp* interface mirror. Preflight ordering + strings are the pure seam in
    // Tracker/CollaborationPreconditionPure.h (CollaborationPreconditionPure.test.cpp).
    VoidResult AddIssueCommentAnnotateContext(const std::string& issueKey, const std::string& p4User,
                                              const std::string& functionName, const std::string& filePath,
                                              int lineNumber, const std::string& changelist, const std::string& date,
                                              bool approximated, const std::string& codeSnippet);

    /// Group names for `accountId` via the focused backend's ITrackerActivity. `error()` carries the
    /// user-facing message on no backend / no activity support / backend error. Off-UI (blocks on HTTP).
    Result<std::vector<std::string>> FetchUserGroupNames(const std::string& accountId) const;

    // --- Per-pane activity delegators (user-info-window Slice 3, plan item 15) ----------
    // The User Info window opens FROM a grid pane: each delegator resolves THAT pane's
    // backend (gridContexts_ lookup, falling back to the focused context for an unknown /
    // retired pane id) and null-checks its Activity() role — the FetchUserGroupNames guard
    // pattern, per-pane. The fetches block on HTTP — worker thread only (Pillar 2).

    /// Whether `paneId`'s backend exposes the activity role (UI gates the window on this).
    bool PaneSupportsActivity(const std::string& paneId) const;

    /// Recent tracker activity for `accountId` via the pane's ITrackerActivity (see that
    /// header for the dayFrom/dayTo/projectScope and `progress` contracts).
    Result<std::vector<TrackerActivityEntry>>
    FetchPaneUserActivity(const std::string& paneId, const std::string& accountId, const std::string& dayFrom,
                          const std::string& dayTo, const std::string& projectScope,
                          TrackerActivityProgress& progress) const;

    /// Cancel the pane's in-flight FetchPaneUserActivity (safe from any thread; no-op
    /// when the pane has no activity role).
    void ClearPaneUserActivity(const std::string& paneId) const;

    /// Group-member lookup through the pane context's lazy GridContextGroupRoster cache
    /// (hit = no HTTP; miss = ITrackerActivity::FetchGroupMembers, then cached). `error()`
    /// carries the user-facing message on no backend / no activity support / backend error.
    Result<std::vector<TrackerUser>> FetchPaneGroupMembers(const std::string& paneId, const std::string& groupName);

  private:
    // DR6: shared_ptr (not unique_ptr) so off-UI-thread workers (Lua automation / MCP pool,
    // off-thread offline-queue create) can atomic_load a snapshot and hold the cache alive
    // across their whole use while the UI thread swaps it in RecreateLocalCacheDatabase. Writes
    // are UI-thread-only via std::atomic_store; off-thread reads via std::atomic_load. Mirrors
    // the ADR-0012 Backend atomic-swap pattern (GridLiveContext::Backend).
    std::shared_ptr<LocalCacheManager> Cache;
    std::unique_ptr<ITrackerBackendFactory>
        backendFactory_; ///< Lazy-initialized in `Initialize` if not pre-set via `SetBackendFactory`.
    // `Backend` moved into GridLiveContext (multi-grid Slice 1, ADR-0018) — access via
    // `focusedContext().Backend`, keeping the ADR-0012 atomic_load/atomic_store discipline.
    /// Defer-free graveyard (ADR 0012): a live tracker swap retires the OLD backend here
    /// instead of freeing it, so raw subobject pointers (Reader/Mutations/Connectivity)
    /// captured by in-flight workers before the swap stay valid. Drained only in
    /// `~AppController` (after `JoinBackgroundTasks` joins all workers). Tracker switches are
    /// rare user actions, so this holds at most a handful of small backend objects per session.
    std::mutex retiredBackendsMutex_;
    std::vector<std::shared_ptr<ITrackerBackend>> retiredBackends_;

    /// Shutdown/abort cancel token handed to each backend's Mutations() at create time
    /// (ITrackerIssueMutations::SetMutationCancelToken). Raised in `~AppController` BEFORE the bounded
    /// automation-worker join so an in-flight blocking tracker mutation (UpdateIssueFields) inside an
    /// automation job aborts promptly — the worker unwinds and the unconditional join() completes fast
    /// (no .detach). Mutation-path twin of the search-path cancel plumbing (#1529). Allocated once in
    /// the ctor; a single shared_ptr<atomic<bool>> shared across backend swaps.
    std::shared_ptr<std::atomic<bool>> automationShutdownCancel_;

  public:
    /// Retire a swapped-out backend into the defer-free graveyard (see `retiredBackends_`).
    /// Called by `GridContextDepsAdapter::SetBackend`. Thread-safe.
    void RetireBackend(std::shared_ptr<ITrackerBackend> old);

  private:
    /// Implements `IOfflineQueueDeps` + `ITicketSyncDeps`. Constructed eagerly in `Initialize`
    /// before `offlineQueue_` / the context's `ticketSync_` so they can capture an interface
    /// reference at construction time. The adapter never outlives this AppController (owned by
    /// it), and the `GridLiveContext&` it stores refers to the default context created in the
    /// constructor and destroyed only by ~AppController (declared below the adapter, so the
    /// context — and its TicketSyncService — is destroyed first).
    std::unique_ptr<GridContextDepsAdapter> depsAdapter_;
    /// Owns the offline-create / offline-field-edit replay queues and their dead-letter management.
    /// Constructed lazily in `Initialize`. Public AppController methods (`QueueCreateOffline`,
    /// `GetPendingCreates`, etc.) are thin delegators that forward to this service. See
    /// BACKLOG_CODE_REVIEW.md §1.7 / §7 item 12.
    std::unique_ptr<OfflineQueueService> offlineQueue_;
    /// Owns the per-issue + per-issue-type editmeta cache (`editMetaMutex_` + its three
    /// containers) and the load/refresh/invalidate/prune/warm methods. Constructed eagerly in
    /// `Initialize` after `depsAdapter_`. Public AppController methods (`CanEditFieldForIssue`,
    /// `EnsureIssueEditMetaLoaded`, etc.) are thin delegators forwarding to this service. See the
    /// AppController god-object decomposition plan (Phase 1).
    std::unique_ptr<EditMetaCacheService> editMeta_;
    /// Owns the field-edit network pipeline (SubmitFieldEdit / SubmitFieldEditNetworkOnly /
    /// TryPrepareOfflineFieldEdit / ApplyFieldEditResult + their branch helpers). Constructed
    /// eagerly in `Initialize` AFTER `editMeta_` (it holds an `EditMetaCacheService&` directly) so
    /// it destructs before editMeta_ — the ref outlives it. Public AppController field-edit methods
    /// are thin delegators forwarding to this service. See the AppController god-object
    /// decomposition plan (Phase 2).
    std::unique_ptr<FieldEditPipelineService> fieldEdit_;
    /// GLOBAL singleton owning the tracker connectivity-probe FSM (probe state + recovery latch +
    /// the live ticket-sync warning + the per-frame offline banner formatter). Constructed eagerly
    /// in `Initialize` after `depsAdapter_`; every per-pane adapter forwards its connectivity writes
    /// into this one instance (N writers, one service). Public AppController connectivity methods are
    /// thin delegators forwarding here. See the god-object decomposition plan (Phase 3).
    std::unique_ptr<ConnectivityMonitorService> connectivity_;
    /// Owns the attachment-open + GitHub app-update logic (ShowAttachmentCollection / OpenAttachment /
    /// OpenAttachmentInSystemViewer / DownloadAttachmentForPreview + GetAppVersion /
    /// GetGitHubReleaseRepo / CheckForAppUpdate / DownloadAndLaunchInstallerUpdate). Constructed
    /// eagerly in `Initialize` after `depsAdapter_`. Public AppController methods are thin delegators
    /// forwarding here. See the AppController god-object decomposition plan (Phase 4).
    std::unique_ptr<AttachmentAppUpdateService> attachmentAppUpdate_;
    /// Default pane id ("main" — matches ConfigManager_Panes bootstrap). The default
    /// context is PERMANENT: created in the constructor, never retired (offlineQueue_
    /// holds a deps-adapter reference chain into it), so focusedContext() fallback and
    /// every delegator stay valid for the controller's whole lifetime.
    static const std::string kDefaultPaneId;
    /// PANE-FROZEN deps adapters, one per live pane INCLUDING the default one (`depsAdapter_`
    /// above is the separate focus-following adapter the process-wide services use). Each pane's
    /// TicketSyncService must keep talking to the context it was created for even while another
    /// pane holds focus. Declared BEFORE `gridContexts_` so each context (whose TicketSyncService
    /// teardown joins the sync worker and may call back into its deps) is destroyed BEFORE the
    /// adapter it references.
    std::map<std::string, std::unique_ptr<GridContextDepsAdapter>> paneAdapters_;
    /// Per-pane live engine bundles (backend + TicketSyncService + ActiveTickets snapshot +
    /// field catalog; see GridLiveContext.h), keyed by GridPane id. Visibility-driven
    /// lifecycle (multi-grid Slice 3, plan item 17): visible pane → EnsurePaneContextLive;
    /// pane not drawn → no new syncs; hidden past the grace window → retired (backend to the
    /// ADR-0012 graveyard) by TickAllContexts. The kDefaultPaneId entry is permanent.
    std::map<std::string, std::unique_ptr<GridLiveContext>> gridContexts_;
    /// Guards the STRUCTURE of gridContexts_ (find / emplace / erase / iterate), distinct from
    /// the per-context content mutexes (groupRoster.rosterMutex_, activeTicketsMutex_,
    /// fieldCatalog.availableFieldsMutex_) which guard a single context's payload. Issue #1457:
    /// the User Info worker (FetchPaneGroupMembers / FetchPaneUserActivity off std::async) resolves
    /// a context by id concurrently with the UI thread's retire-erase / EnsurePaneContextLive
    /// emplace, so every off-UI-thread map access takes this mutex. Lock ordering is map-mutex
    /// OUTERMOST then a per-context mutex; the worker snapshots the raw pointer under this mutex and
    /// releases it BEFORE taking the per-context mutex, so it never holds both at once.
    mutable std::mutex gridContextsMutex_;

    /// The context global actions target (permanent focused-pane semantics, ADR-0018).
    /// Cached raw pointer (focused-pane lookups sit under per-frame delegators — keep O(1));
    /// re-pointed by SetFocusedPane / EnsurePaneContextLive / context retirement, and falls
    /// back to the permanent default context when the focused pane has no live context
    /// (design addendum § 6.2).
    GridLiveContext& focusedContext() { return *focusedContextPtr_.load(); }
    const GridLiveContext& focusedContext() const { return *focusedContextPtr_.load(); }
    /// `paneId`'s live context, falling back to focusedContext() when the pane has none
    /// (unknown id / retired mid-flight). Never null — the default context is permanent.
    GridLiveContext& paneContextOrFocused_(const std::string& paneId);
    const GridLiveContext& paneContextOrFocused_(const std::string& paneId) const;
    /// Ticket ids held by every live context OTHER than `self` that shares `self`'s cache
    /// backend key. Backs `ITicketSyncDeps::TicketIdsRetainedByOtherContexts` so a pane's
    /// full-sync stale-deletion does not delete rows a sibling pane is displaying (the SQLite
    /// cache is one shared backend-keyed namespace, ADR-0018 decision 4, while each pane syncs
    /// its own query). Follows the issue-#1457 lock order: snapshot the context pointers under
    /// gridContextsMutex_, release it, THEN take each per-context activeTicketsMutex_.
    std::vector<std::string> CollectTicketIdsRetainedByOtherContexts(const GridLiveContext& self) const;
    /// Published roster snapshot of every LIVE context, across all cache namespaces. Backs the
    /// editmeta-cache prune: that cache is process-wide, so pruning it to ONE pane's snapshot
    /// evicts entries the other panes' open editors still need. Returns shared_ptr snapshots (not
    /// ids) because the prune also unions the issue-type keys off each ticket.
    std::vector<std::shared_ptr<const std::vector<CachedTicket>>> CollectActiveTicketSnapshotsAcrossContexts() const;
    /// --- Per-pane owned-ticket-id sets (multi-pane cache scoping) --------------------------
    /// The SQLite cache is ONE backend-keyed namespace shared by every pane (ADR-0018 decision
    /// 4), so `GetAllTickets(backendKey)` returns the union of all panes' queries. These record
    /// which ids each pane's own sync actually kept, so a namespace-wide read can be filtered
    /// back down to the pane that asked. Keyed `backendKey + '\n' + paneId`; survives LRU
    /// eviction of a hidden pane's context (the context is gone but its rows are still in the
    /// shared cache and must not be stale-deleted by a sibling), and tombstoned on 30 s
    /// retirement via ForgetPaneOwnedTicketIds. Thin forwarders onto the header-only store, which
    /// owns the (innermost) lock and is where the state machine is unit-tested.
    void SetPaneOwnedTicketIds(const std::string& backendKey, const std::string& paneId,
                               const std::vector<std::string>& ids);
    /// False = this pane has NEVER recorded a set (true cold start → the caller may fall back to
    /// the whole namespace). True with an empty `outIds` = a retired-and-revived pane, which must
    /// render empty until its own sync lands (issue #2063).
    bool TryGetPaneOwnedTicketIds(const std::string& backendKey, const std::string& paneId,
                                  std::vector<std::string>& outIds) const;
    /// Atomic single-id append (issue #2050) — re-reads the entry under the store's lock, so a
    /// concurrent PublishOwnedTicketIds is extended rather than clobbered by a stale copy.
    /// No-op (false) when the pane has no recorded set yet.
    bool AddPaneOwnedTicketId(const std::string& backendKey, const std::string& paneId, const std::string& id);
    /// Subtract specific ids from one pane's recorded set without disturbing the rest — used when
    /// a 404 reconcile purges cache rows, so the set stops whitelisting (and pinning) dead ids.
    void DropPaneOwnedTicketIds(const std::string& backendKey, const std::string& paneId,
                                const std::vector<std::string>& ids);
    /// Keyed by pane id ACROSS every backend key: a pane that switched tracker kind filed its set
    /// under the key live at publish time, not the one live at retirement (issue #2049).
    void ForgetPaneOwnedTicketIds(const std::string& paneId);
    /// Owns the innermost lock in the hierarchy — never take gridContextsMutex_ or a per-context
    /// mutex while inside one of its methods.
    smatchet::PaneOwnedTicketIdStore paneOwnedTicketIds_;
    /// Shared body of the RefreshLocalData paths (issue #1081): null = unchecked UI-thread
    /// refresh; non-null = drop the replace (under ctx.activeTicketsMutex_) when ctx's
    /// backendGeneration_ no longer matches the captured value. `ctx` MUST be the context the
    /// generation was captured from — callers latch it once and do
    /// capture + check + apply on the SAME context. Worker-safe: a reference latched before
    /// retirement stays valid via the retiredContexts_ husk graveyard (until ~AppController).
    /// The full-table cache read runs OUTSIDE the mutex; only the re-check + swap-in lock it.
    /// `admitId` (empty = none) is a ticket id that must survive the pane-scoping filter even
    /// though this pane's last recorded sync set predates it — the row UpdateTicket just saved.
    void RefreshLocalDataCheckedImpl_(GridLiveContext& ctx, const std::uint64_t* capturedBackendGeneration,
                                      const std::string& admitId = std::string());
    /// Re-resolve focusedContextPtr_ from focusedPaneId_ (default-context fallback).
    void refreshFocusedContextPtr_();
    /// Atomic: workers (MCP / Lua / replay) read focusedContext() while the UI thread
    /// re-points it on a pane-focus switch. The pointee outlives any latched read — retired
    /// contexts park as defer-free husks in retiredContexts_ until ~AppController.
    std::atomic<GridLiveContext*> focusedContextPtr_{nullptr};
    std::string focusedPaneId_;
    /// Defer-free husks of retired pane contexts (sync torn down, tickets cleared, backend
    /// moved to retiredBackends_) — the ADR-0012 graveyard pattern applied to contexts so a
    /// worker that latched focusedContextPtr_ pre-switch never dereferences freed memory.
    std::vector<std::unique_ptr<GridLiveContext>> retiredContexts_;

  public:
    /// UI thread, pane-CREATION path. Erase every owned-ticket-id record for `paneId`, tombstone
    /// included — the counterpart to the private ForgetPaneOwnedTicketIds, which deliberately
    /// LEAVES a tombstone at retirement so a revived pane renders empty rather than inheriting
    /// the namespace-wide cold-start seed.
    /// Public because the pane-creation chokepoint lives in the UI layer
    /// (SmatchetGridPaneWindows::ApplyPaneAddAndCloseRequests), and because pane ids are
    /// RECYCLED: GenerateUniquePaneId hands back the lowest unused id, so a new pane routinely
    /// lands on a closed pane's id. Without this it inherits that pane's tombstone, is denied its
    /// cold-start seed, and stays blank until a non-empty full sync completes — never, if the
    /// user is offline or the tracker is down.
    void ErasePaneOwnedTicketIds(const std::string& paneId);

    // --- Multi-grid Slice 3: visibility-driven context lifecycle (plan item 17) ---------
    /// UI thread. Record which pane drives global actions; falls back to the default
    /// context when that pane has no live context yet.
    void SetFocusedPane(const std::string& paneId);
    /// UI thread. Ensure a live GridLiveContext exists for `paneId` (constructs the
    /// per-context deps adapter + TicketSyncService on first sight — backend instantiation
    /// happens inside the first sync's SwapBackendIfTrackerChanged, off the UI thread) and
    /// stamp it visible this frame. A brand-new context whose `backendKey` matches the
    /// default context's gets the default's field catalog copied in (one-time, pane-show —
    /// duplicate/same-backend panes render dropdowns immediately instead of raw values).
    /// Returns the context (never null after return).
    GridLiveContext* EnsurePaneContextLive(const std::string& paneId, const std::string& backendKey) override;
    /// UI thread. One-shot per context generation: kick the pane's FIRST sync against its
    /// own (config, views) pair. The smatchet_views.json bucket load runs on a worker
    /// (Pillar 2 — no disk I/O on the UI thread), then hops back via mainThreadDispatcher
    /// to start the sync on the pane's own TicketSyncService.
    void EnsurePaneLiveSyncStarted(const std::string& paneId, const TrackerConfig& paneCfg, const std::string& viewId);
    /// UI thread. True when `paneId` has a live GridLiveContext whose own first sync was
    /// already kicked (EnsurePaneLiveSyncStarted latch). Consumed by the pane-focus-switch
    /// path: a sync-live pane's data is already fresh from its OWN context, so adopting its
    /// view on focus must not kick a redundant SyncWithBackend (Slice-3 follow-up).
    bool IsPaneSyncLive(const std::string& paneId) const;
    /// UI thread. The JQL the pane context's most-recent kicked sync used (empty when the
    /// pane has no live context or its sync failed — the session-end deps hook clears it).
    /// The focus-switch path compares it against the adopted view's saved JQL: a mismatch
    /// (view edited after the context synced, or a failed first sync) re-kicks instead of
    /// adopting stale rows (review MEDIUM-1/2).
    std::string GetPaneLastSyncedJql(const std::string& paneId) const;
    /// UI thread. Stamp the pane context as sync-kicked for `jql`. Called by the pane
    /// focus-switch path when it decides to KICK (mismatch/cold) so the next focus switch
    /// onto the same pane sees a matching JQL and adopts without a redundant re-fetch.
    void RecordPaneSyncKick(const std::string& paneId, const std::string& jql);
    /// Any pane's live published snapshot (null when the pane has no live context yet).
    std::shared_ptr<const std::vector<CachedTicket>> GetPaneTicketsSnapshot(const std::string& paneId) const;
    /// Per-pane ActiveTickets revision (0 when the pane has no live context).
    std::uint64_t GetPaneTicketsRevision(const std::string& paneId) const;
    /// The pane context's OWN resolved ViewDefinition (from its backend bucket), published
    /// by the pane's first-sync worker; null until that sync lands. Lets a cross-backend
    /// pane build ITS OWN columns even when the focused ViewState bucket can't see its view
    /// (multi-grid Slice 4 cold-start frozen-capture hole). UI thread only.
    std::shared_ptr<const ViewDefinition> GetPaneResolvedView(const std::string& paneId) const;

    // --- Per-pane field-catalog READ routing (per-pane-catalog-value-read-routing) ----------
    // The single focused catalog (GetAvailableFields / GetFieldCatalogRevision above) is
    // insufficient for a cross-backend pane: it renders Jira field metadata against a Plane
    // pane's raw values. These per-pane variants resolve THROUGH the pane's OWN
    // GridLiveContext::fieldCatalog (populated by EnsurePaneContextLive's same-backend seed and
    // the per-pane first-sync catalog fetch), so a cross-backend pane resolves its option
    // labels / display names against its own backend's catalog. All UI-thread only, O(map
    // lookup) — meant to be resolved ONCE per pane per frame (SmatchetUI::resolvePaneCatalog),
    // never per cell (Pillar 1). A pane with no live/populated context falls back to the
    // focused catalog (identical to today's shared read — never worse).
    /// Per-context field-catalog revision (0 when the pane has no live context). Bumped by the
    /// pane's catalog seed/fetch; the read-routing cache keys on it so same-backend duplicate
    /// panes rebuild when the focused catalog refetch re-seeds them (staleness invalidation).
    std::uint64_t GetPaneFieldCatalogRevision(const std::string& paneId) const;
    /// True once the pane context's OWN catalog has been populated (seed or fetch). While
    /// false the read-routing must use the focused catalog (the pane context is still empty).
    bool IsPaneFieldCatalogPopulated(const std::string& paneId) const;
    /// Snapshot COPY of the pane context's AvailableFields (mutex-guarded copy — the focused
    /// GetAvailableFields returns a bare reference, but a non-focused context's catalog may be
    /// rewritten off-thread by its fetch worker, so callers get a copy to build a
    /// TrackerFieldCatalogIndex from safely). Falls back to a copy of the focused catalog when
    /// the pane has no populated context.
    std::vector<TrackerField> GetPaneAvailableFields(const std::string& paneId) const;
    /// Kick a sync on ONE pane's context with its own (config, views) pair — the per-pane
    /// twin of SyncWithBackend (which targets the focused context).
    void SyncPaneWithBackend(const std::string& paneId, const TrackerConfig* configOverride,
                             const ViewsStore* viewsOverride);
    /// Once per frame (replaces the bare TickStreamingApply call): drain every live
    /// context's streaming applies under one shared deadline (rotating start order so a
    /// busy early context cannot systematically starve later ones), then retire contexts
    /// hidden past the grace window whose sync is idle. See plan § Performance / item 18.
    void TickAllContexts();

    // --- Ticket-change monitor (ticket-change-monitor plan, S1c-2, items 8/10/17) --------
    /// UI thread, once per frame (hosted next to TickTrackerConnectivityMonitor in SmatchetUI).
    /// For each recently-visible live pane whose change-poll gate is open
    /// (smatchet::ShouldPollForChanges — monitor enabled, backend reachable, window focused,
    /// pane recently visible, sync idle, interval elapsed), dispatch ONE off-thread change probe
    /// (FetchIssuesChangedSince) and stamp nextChangePollAt forward as the in-flight guard. The
    /// first poll per context establishes a silent baseline (seeds the anchor, no fetch/toast).
    /// Results hop back to applyChangeProbeOnMainThread_. No-op when cfg disables the monitor.
    void TickChangeMonitors(const TrackerConfig& cfg);
    /// Any-thread setter for the OS window focus signal (plan item 17). The Standalone host
    /// feeds GLFW_FOCUSED each frame; targets with no focus signal leave it true. Read by
    /// TickChangeMonitors' poll gate so a backgrounded window stops polling.
    void SetWindowFocused(bool focused);

  private:
    /// Off-thread (worker) half of the membership reconcile (S1c-3, item 10): fetch the view's
    /// current keys, diff against `cachedKeys` (the pane's cache keys snapshotted on the UI thread
    /// at dispatch), and ProbeIssueExists each vanished key (capped per cycle), returning one
    /// smatchet::MembershipRemovalVerdict per probed key. Static — touches no AppController instance
    /// state, so it is safe on the background task thread; the verdicts are applied on the UI thread
    /// by applyChangeProbeOnMainThread_. `paneId` is for logging only.
    static std::vector<smatchet::MembershipRemovalVerdict>
    computeMembershipRemovals_(ITrackerIssueReader& reader, const TrackerConfig& cfg, const ViewsStore& views,
                               const std::vector<std::string>& cachedKeys, const std::string& paneId);
    /// Main-thread completion of a change probe: re-find the pane context (dropped if retired)
    /// and re-check its backendGeneration_ against `capturedGeneration` (dropped if swapped
    /// mid-flight), diff the fetched rows against the pane's current ActiveTickets cache
    /// (DiffChangedTickets over the resolved salient roster), fold in the membership `verdicts`
    /// (ClassifyMembershipRemovals → erase resident rows from ActiveTickets, drop the shared cache
    /// row on each 404, republish + bump revision), notify on any salient delta, then advance
    /// changeSinceAnchor and re-stamp nextChangePollAt. UI thread.
    void applyChangeProbeOnMainThread_(const std::string& paneId, std::vector<CachedTicket> fetched,
                                       std::vector<smatchet::MembershipRemovalVerdict> verdicts,
                                       std::chrono::system_clock::time_point polledAt,
                                       std::uint64_t capturedGeneration);
    /// OS window focus latch (plan item 17). Defaults true so targets without a focus signal
    /// always poll. Written by SetWindowFocused (any thread), read by TickChangeMonitors.
    std::atomic<bool> windowFocused_{true};

    /// EnsurePaneLiveSyncStarted's main-thread completion: capture-then-check the backend
    /// generation (issue #1081 — stale kick dropped + latch re-armed), then kick the live
    /// fetch and seed the cleared ActiveTickets from the durable cache snapshot. UI thread.
    void applyPaneSyncKickOnMainThread_(const std::string& paneId, TrackerConfig cfgCopy, const ViewsStore& views,
                                        const std::string& viewId, std::uint64_t capturedGeneration,
                                        std::vector<CachedTicket> seedTickets);
    /// Populate a pane context's OWN fieldCatalog after its first sync created the backend
    /// (per-pane-catalog-value-read-routing). Cross-backend panes start with an empty catalog
    /// (the same-backend seed in EnsurePaneContextLive doesn't apply), so their grid renders
    /// raw field values until focus refetches the focused catalog. This kicks an off-thread
    /// catalog fetch against the pane's OWN backend + config and writes the result INTO the
    /// captured GridLiveContext (NOT fieldCatalog()), gated by the #1081 kick-time generation
    /// check so a mid-flight backend swap / context retirement drops the stale apply. UI thread
    /// only; a no-op when the pane already has a populated catalog (same-backend seed) or is
    /// backend-less. cfgCopy carries the pane's own backend endpoint fields (flat TrackerConfig
    /// holds every backend's endpoint simultaneously — no per-backend endpoint plumbing needed).
    void populatePaneCatalogAfterSync_(const std::string& paneId, const TrackerConfig& cfgCopy,
                                       const std::string& projectKey, std::uint64_t capturedGeneration);
    /// Main-thread apply of populatePaneCatalogAfterSync_'s off-thread fetch: re-validate the
    /// captured context is still current (present + generation unchanged), then write the fields
    /// into ITS fieldCatalog under the context mutex and bump the per-context revision.
    void applyPaneCatalogOnMainThread_(const std::string& paneId, std::uint64_t capturedGeneration,
                                       std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                                       std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta);
    /// TickAllContexts phase 2 — retire non-default contexts hidden longer than
    /// kHiddenContextGraceMs whose sync is idle (backend → ADR-0012 graveyard).
    void retireExpiredHiddenContexts_(std::chrono::steady_clock::time_point now);
    /// TickAllContexts phase 3 — hidden-pane LRU memory cap (multi-grid Slice 5a, plan item 21):
    /// when more than hiddenPaneResidentCap_ HIDDEN contexts still hold an in-memory ticket
    /// snapshot, drop the least-recently-visible idle one's ActiveTickets (rows survive in
    /// tickets_v2 — re-showing re-seeds losslessly via EnsurePaneLiveSyncStarted). Visible /
    /// focused / busy-sync contexts are never evicted. `now` distinguishes visible-this-frame
    /// (lastVisibleAt fresh) from hidden.
    void evictHiddenPanesOverCap_();
    std::size_t tickRotation_ = 0;
    /// Monotonic source for GridLiveContext::lastVisibleOrder (UI thread only). Bumped in
    /// EnsurePaneContextLive each frame a pane is visible so the LRU eviction order is the
    /// reverse visibility order.
    std::uint64_t paneVisibilityClock_ = 0;
    /// Per-FRAME counter (bumped once at the top of TickAllContexts, UI thread). A pane stamps
    /// GridLiveContext::lastVisibleFrame with it when drawn; the hidden-pane cap treats a pane
    /// as visible when its stamp is within one frame of this — FPS-independent, unlike the
    /// wall-clock lastVisibleAt window (which misclassifies a visible pane as hidden at low FPS).
    std::uint64_t paneFrameClock_ = 0;
    /// Max HIDDEN contexts that may retain an in-memory ticket snapshot before the LRU cap
    /// frees the least-recently-visible one. Resolved from cfg.HiddenPaneResidentCap at
    /// InitConfig (0/negative → default 4). Visible + focused panes are exempt.
    std::size_t hiddenPaneResidentCap_ = 4;
    // `luaHost_` (unique_ptr<LuaAutomationHost>) moved into AppController::Impl (pImpl #19b).
    /// Unified Command System registry — owns the catalog of named commands and dispatches them to
    /// CLI / MCP / Lua / Palette callers. Constructed eagerly in `Initialize` after the tracker
    /// backend so handlers can capture `*this` and safely call AppController methods.
    std::unique_ptr<smatchet::cmd::CommandRegistry> commandRegistry_;
    std::unique_ptr<smatchet::cmd::ScenarioRunner> scenarioRunner_;
    // ActiveTickets + activeTicketsPublished_ + ActiveTicketsRevision (and their
    // activeTicketsMutex_) moved into GridLiveContext (multi-grid Slice 1, ADR-0018) —
    // access via `focusedContext()`.
    // The in-memory field-catalog block (TrackerFieldCatalogRevision / AvailableFields /
    // AvailableComponents / AvailableIssueTypeMeta / AvailableUsers / catalog error+warning /
    // currentCatalogProjectKey_ / projectComponentOptions_ + in-flight/backoff sets and
    // availableFieldsMutex_) moved into GridLiveContext::fieldCatalog (multi-grid Slice 3,
    // plan item 17 + slice1-design § 3.1): it was semantically single-backend, so two live
    // different-backend panes would have overwritten each other's catalog. Access via the
    // fieldCatalog() accessor below (focused-context routing — same delegator semantics as
    // the rest of the engine state, ADR-0018).
    GridContextFieldCatalog& fieldCatalog() { return focusedContext().fieldCatalog; }
    const GridContextFieldCatalog& fieldCatalog() const { return focusedContext().fieldCatalog; }
    // LastTrackerTicketSyncWarning moved into ConnectivityMonitorService (Phase 3) as its single
    // owner; reached via `connectivity_->LastTicketSyncWarning()` / `SetLastTicketSyncWarning(...)`.
    // `AutomationLogSinks` moved to LuaAutomationHost in Phase 1A of the item 14 extraction.
    /// Log sinks registered via AddAutomationLogSink before luaHost_ is constructed
    /// (i.e. during OnEarlyInit which fires before Initialize). Drained into luaHost_ once
    /// Initialize constructs it; cleared immediately after.
    std::vector<std::function<void(const std::string&)>> pendingLogSinks_;

    /// Error sinks registered via AddAutomationErrorSink — called on any Lua error in
    /// the automation runner or setup path. Unconditional (no Lua guard) so the plugin can
    /// register regardless of build config. Accessed on the UI thread (registration in
    /// OnEarlyInit) and on the background automation worker thread (call time) — the
    /// sinks themselves must be UI-thread-safe (e.g. post via mainThreadDispatcher).
    std::vector<std::function<void(const std::string&)>> errorSinks_;
    /// Atomically set by the background automation worker on Lua error; consumed once per
    /// frame by LuaConsolePlugin::OnDraw to open + focus the Scripting window.
    std::atomic<bool> scriptingWindowOpenRequested_{false};
    // Phase 0 (appcontroller-god-object-decomposition): the 7 optional host callbacks are grouped
    // into one HostCallbacks struct (Types/HostCallbacks.h). The 7 public setters above are
    // unchanged and assign into this member; consumers read through it. Publish-once discipline
    // (host sets once at setup on the UI thread; read on the UI thread) is unchanged.
    HostCallbacks hostCallbacks_;

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    // Recorder + replay value types live in `AppController_LuaTypes.h`; the `AppController::`
    // `using` aliases were removed in hardening #19c (they aliased sol-backed types and forced
    // sol2 into this header). Call sites now use the canonical `smatchet::lua::*` names, and the
    // alias shorthands live on AppController::Impl. ApplyOrQueueLuaWindowOp takes the value type
    // by a forward-declared name (complete only in the binding TUs).
  public:
    /// UI-thread helper: applies a window register / unregister / invalidate op immediately
    /// when DrawLuaWindows is not iterating, otherwise enqueues onto pendingLuaWindowOps_
    /// for in-frame drain. Off-thread callers must hop the dispatcher BEFORE this.
    void ApplyOrQueueLuaWindowOp(smatchet::lua::PendingLuaWindowOp op);

  private:
    // pImpl (hardening #19, step 19b): the sol-typed STORAGE for this Lua block (`sol::state lua`
    // + the three `sol::protected_function` maps + the field-cache / window / mcp-tool / action /
    // icon-map containers + their mutexes) moved into `struct AppController::Impl` (defined in
    // AppControllerImpl.h, included only by the AppController*.cpp TUs). The member-order /
    // shutdown-destruction invariant (`sol::state lua` declared FIRST so it tears down LAST,
    // after the protected_function containers) is preserved verbatim inside Impl. Only these
    // NON-sol-typed generation/iteration scalars stay here (hot, read per-cell; no sol2 needed):
    /// Bumped on provider (un)register. Init to 1 so cached entries with `providerGen=0`
    /// always miss on first compare.
    std::atomic<std::uint64_t> luaProviderGen_{1};
    /// Bumped by NotifyLuaTicketDataChanged; cells use per-entry comparison instead.
    std::atomic<std::uint64_t> luaWindowDataGen_{1};
    /// True while inside DrawLuaWindows iteration. Callbacks fired during replay route
    /// register/unregister/invalidate ops into pendingLuaWindowOps_ instead of mutating
    /// luaWindows_ directly. Plain bool — UI-thread-only.
    bool inDrawLuaWindows_ = false;

#endif

  private:
    /// Coalesced fetch-session window-dirty signal. TicketSyncService flips this in each
    /// ApplyIssueFetchPack + stale-deletion + streaming-state-changed scope, then fires
    /// NotifyLuaTicketDataChanged() once at session end. Plain bool — UI-thread only. Lives
    /// outside the Lua guard so TicketSyncService (friend) reads/writes it unconditionally;
    /// NotifyLuaTicketDataChanged is a no-op in the stub build.
    bool pendingLuaWindowBump_ = false;

    /** Absolute path to the `Scripts` folder (trailing slash), or empty to use `Scripts/` relative to cwd. */
    std::string luaScriptsDirectory_;

    /// Background-task body of PrefetchIssueTicketsForKeys: fetch the keys off the UI thread, clear
    /// their in-flight markers, persist results to cache, and refresh local data. Runs off-thread.
    void FetchAndCachePrefetchedTickets(const std::vector<std::string>& toFetch);

    /// Memoised result of `ResolveFieldIconAssetPath` keyed on the raw path-or-url input.
    /// Resolution does 2-3 `fs::weakly_canonical` syscalls on identical inputs hot-path-per-frame
    /// from `LuaDrawList::Replay`. Both base directories (`luaScriptsDirectory_`,
    /// `ConfigManager::GetRuntimeAssetDirectory`) are set once at startup and never re-assigned,
    /// so the function is pure w.r.t. its input string for the process lifetime. UI-thread only.
    static constexpr std::size_t kFieldIconAssetPathCacheCap = 256;
    mutable std::unordered_map<std::string, std::string> fieldIconAssetPathCache_;

    // The IssueEditMetaCache struct + editMetaMutex_ + its three containers + the editmeta
    // load/refresh/invalidate/prune/warm methods + ResolveIssueTypeKeyForIssue +
    // WarmIssueTypeEditMetaWorker moved into EditMetaCacheService (god-object decomposition
    // Phase 1). Accessed via `editMeta_`; the public delegators below preserve the prior surface.

    // Take the caller's already-latched catalog (#823): fieldCatalog() re-resolves
    // focusedContextPtr_ per call, so re-resolving inside the helper could insert the synthetic
    // field into a different context than the caller populated. Caller passes its latch.
    void EnsureCatalogHistoryField(GridContextFieldCatalog& cat);
    void EnsureCatalogCommentsField(GridContextFieldCatalog& cat);
    // issue-comments fix (#1291) — erase Jira's legacy system `comment` field (ADF blob) from the
    // catalog so it stops duplicating the synthetic `comments` count column in the Fields picker.
    // Same latched-catalog contract as the Ensure* helpers above. The blob survives in per-ticket
    // fieldValues["comment"] (Jira mapper, catalog-independent) for the Comments-cell hover tooltip.
    void EraseCatalogLegacyCommentField(GridContextFieldCatalog& cat);
    // TryBuildFieldEditPayloadForNetwork + SubmitFieldEditCtx + the three SubmitFieldEdit branch
    // helpers (Sprint/Timetracking/Regular) moved into FieldEditPipelineService (god-object
    // decomposition Phase 2). Accessed via `fieldEdit_`; the public delegators above preserve the
    // prior surface.

  public:
    /// IAppThreading — spawn `task` on a tracked background thread. Threads are
    /// joined either when the producer completes or in `JoinBackgroundTasks`
    /// before destruction — never detached. Post results back to the UI thread
    /// via `PostToMainThread` inside `task`.
    /// Public so non-member callers (grid field-edit pipeline, etc.) can
    /// dispatch HTTP / SQLite work off the UI thread without re-implementing
    /// thread-bookkeeping.
    void LaunchBackgroundTask(std::function<void()> task) override;

    /// True once shutdown has begun. Public so `EditMetaCacheService` (via the deps adapter) can
    /// poll it from its warm worker to bail out of long loops (god-object decomposition Phase 1).
    bool IsShuttingDown() const { return shuttingDown_.load(); }

  private:
    void JoinBackgroundTasks();
    /// Join + erase any background workers whose task has completed (their `done` flag is set,
    /// so the join returns immediately — no UI-thread stall). Called on each new launch so the
    /// worker vector stays bounded mid-session instead of accumulating dead-but-joinable
    /// std::thread objects until shutdown (memory-budget-and-lifetime-hardening § Phase 4).
    /// Caller MUST hold `backgroundWorkersMutex_`.
    void reapFinishedBackgroundWorkersLocked_();

    // The connectivity-probe FSM (DrainProbeFuture / ApplyProbeResult / MapReachabilityProbeKind /
    // IsConnectivityDegradedForProbeInterval / PushOfflineReplayTimersDuringTransportOutage /
    // requestDeferred / applyLive) and its 9 state members were lifted into ConnectivityMonitorService
    // (god-object decomposition Phase 3). They live on `connectivity_` below; the public connectivity
    // methods above are thin delegators forwarding into that service.

    // The deferred-success-notify forwarder kept on AppController so its many in-class callers (and
    // the adapter's CONST RequestDeferredLiveTrackerBackendSuccessNotify override) stay one-line; it
    // forwards into the global connectivity service.
    void requestDeferredLiveTrackerBackendSuccessNotify_() const;
    // Kept as a delegator: the adapter's ITicketSyncDeps::PushOfflineReplayTimersDuringTransportOutage
    // override forwards here, and AppController routes it on into the connectivity service (which adds
    // the transport-down delay + the null-queue-guarded deps push).
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now);

    std::atomic<bool> shuttingDown_{false};
    /// One tracked background worker. `done` flips true when the task returns, so the pool can
    /// reap finished threads mid-session (a finished thread's join() is instant) instead of
    /// letting joinable-dead std::thread objects pile up until shutdown.
    struct BackgroundWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<BackgroundWorker> backgroundWorkers_;
    mutable std::mutex backgroundWorkersMutex_;

    // Offline-replay throttle + in-flight guards + legacyPendingStartupBanner_ all moved to
    // OfflineQueueService (item 12 extraction phases 1A / 1C). Accessed via offlineQueue_.

    mutable std::mutex bulkImportPrefetchKeysMutex_;
    std::unordered_set<std::string> bulkImportPrefetchKeysInFlight_;

    PluginHost* runtimePluginHost_ = nullptr;

#if defined(SMATCHET_WITH_MCP)
    // `kMcpActivityLogMax` + `mcpActivityMutex_` + `mcpActivityLog_` moved into
    // AppController::Impl (pImpl #19b). The HTTP-traffic tracking atomics below stay (hot,
    // lock-free, read on the UI thread; not part of the activity-log storage).
    /** `steady_clock` epoch offset in nanoseconds; 0 means no client HTTP activity yet. */
    std::atomic<std::uint64_t> mcpLastClientHttpActivityNs_{0};
    std::atomic<std::uint64_t> mcpHttpTrafficEpoch_{0};
#endif

#if defined(SMATCHET_WITH_AI)
    // Smatchet Assistant — Phase B. `aiAssistant_` (unique_ptr<AiAssistantController>) moved into
    // AppController::Impl (pImpl #19b). Lifetime contract unchanged: constructed at the end of
    // `Initialize`; destroyed at the *top* of `~AppController` (impl_->aiAssistant_.reset())
    // BEFORE mainThreadDispatcher.BeginShutdown() fires.
#endif

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    struct AutomationJob {
        enum class Type { RunAutoScript, TicketAction, GlobalAction, RunFlatScript };
        Type type;
        std::string scriptPathOrActionName;
        std::vector<std::string> selectedIds;
        std::string targetIssueId;
        // Explicit opt-in to run across ALL loaded tickets when selectedIds is empty.
        // Default false: empty selection + !processAll is a refusal (no silent mass-modify,
        // no silent no-op). See RunAutomationAutoScript and Issue #824.
        bool processAll = false;
    };
    // The automation queue + worker member STORAGE (automationJobMutex_ / automationJobCv_ /
    // automationJobs_ / automationWorker_ / automationWorkerShuttingDown_ / activeSetupScripts_)
    // moved into AppController::Impl (pImpl #19b), AND the sol-typed worker methods themselves
    // (AutomationWorkerLoop / RunAutomationJob / RunAutomation{AutoScript,FlatScript,ActionCall})
    // moved onto Impl in #19c — they take sol::state& / sol::environment& and cannot be declared
    // here without sol2. `AutomationJob` (sol-free POD) stays on AppController so the queue
    // storage in Impl + the RunAutoScript / RunFlatScriptAsync enqueuers reference one type.
#endif

  private:
    // StreamingSyncState struct, currentFetchRequestId_, activeStreamingSync_,
    // hasPendingSyncRequest_, pendingConfig_, pendingViews_, isDeletingStale_,
    // staleIdsToDelete_, totalStaleToDelete_, staleDeletedSoFar_ — all moved to
    // TicketSyncService in Phase 1C of the item 11 extraction. CancelAndJoinActiveStreamingSync
    // and StartStreamingSync (private) moved to TicketSyncService too — the public
    // SyncWithBackend remains on AppController as a thin delegator.

    void CancelAndJoinActiveStreamingSync();

    std::string localCacheDbPath_;

    /// Set once in `Initialize` (which runs on the UI thread) and read by `IsOnUiThread`
    /// from any thread. `std::thread::id` is trivially copyable; atomic load via std::atomic
    /// of the same type is well-supported but std::thread::id isn't an atomic-friendly type
    /// on all toolchains — so we wrap it in a tiny mutex-free pattern: the value is written
    /// exactly once before any reader exists (Initialize is called before any worker is
    /// spawned), and never mutated afterwards. Reads are race-free under the "publish once,
    /// read many" pattern.
    std::thread::id uiThreadId_{};

    // pImpl (hardening #19) — the COLD, sol2-/subsystem-heavy state lives in
    // `struct Impl` (defined in the src-only header AppControllerImpl.h, included
    // by the AppController*.cpp TUs where its full member types, including sol2,
    // are visible). Lifting it out of this header is what lets AppController.h
    // stop including <sol/sol.hpp> (step 19c), so the ~100 header includers no
    // longer pull sol2 into their compile. Hot, per-frame members stay inline
    // above (zero perf delta). MUST be declared LAST: it is constructed last and
    // destroyed first, and its ctor/dtor are out-of-line in AppController.cpp
    // where Impl is a complete type (incomplete-type unique_ptr discipline).
    // (`struct Impl;` forward-declared publicly near the top — see GetLuaBindingHost.)
    std::unique_ptr<Impl> impl_;
};

#endif
