#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

// 1. MUST BE INCLUDED FIRST FOR GCC 13+ COMPATIBILITY
#include <limits>
#include <cstdint>

// 2. Lua / sol2 (optional build — see SMATCHET_WITH_LUA_AUTOMATION in CMake)
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#include "ILuaBindingHost.h"
#endif
// 2b. Lua recorder + replay value types (extracted from this header — see § A5
//     of docs/plans/shipped/large-files-and-phase-2.md). Self-guarded by the same
//     SMATCHET_WITH_LUA_AUTOMATION macro.
#include "AppController_LuaTypes.h"

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
#include "LocalCacheManager.h"
#include "ITrackerBackend.h"
#include "MainThreadDispatcher.h"
#include "SmatchetMergeWatchNotifyServer.h"
#include "IssueDraft.h"
#include "IssueCreatePipeline.h"
#include "JiraClient.h"

#include <nlohmann/json.hpp>

class PluginHost;
// AiTypes.h is unconditional (POD header, no transitive includes beyond <atomic>/<memory>/etc.)
// so AppController.h consumers can use `AiContextBlock` without macro plumbing — the always-on
// stub methods take it by const-ref / return std::vector<AiContextBlock>.
#include "AiTypes.h"
#if defined(SMATCHET_WITH_AI)
// Phase B: AiAssistantController is a complete type here so `std::unique_ptr<AiAssistantController>`
// can be destroyed by the (implicit-on-translation-unit) default deleter when consumers include
// this header. Without the full type, every TU that includes AppController.h would have to
// either provide a dtor or run into the unique_ptr<incomplete-type> sizeof error.
#include "AiAssistantController.h"
#endif

/** Single consolidated tracker degraded/offline banner for main windows (replaces stacked warnings). */
struct TrackerConnectivityBannerForUi {
    enum class Level { None, Warning, Error };
    Level Kind = Level::None;
    std::string Message;
};

/** Raw tracker issue fetch result; apply on the UI thread via AppController::ApplyIssueFetchPack. */
struct TrackerIssueFetchPack {
    std::vector<CachedTicket> Tickets;
    bool FullSyncCompleted = false;
    std::string FetchError;
    /// Soft caveat (e.g. pagination cap). See TrackerIssueFetchSummary::Warning.
    std::string Warning;
};

struct AppUpdateAsset {
    std::string Name;
    std::string DownloadUrl;
};

struct AppUpdateInfo {
    bool Ok = false;
    bool UpdateAvailable = false;
    std::string CurrentVersion;
    std::string LatestVersion;
    std::string ReleaseTag;
    std::string ReleaseUrl;
    std::string ReleaseNotes;
    std::string Error;
    AppUpdateAsset InstallerAsset;
};

class ITrackerBackendFactory;
class AppControllerDepsAdapter;
class OfflineQueueService;
class TicketSyncService;
class LuaAutomationHost;

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

class AppController
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    : public ILuaBindingHost
#endif
{
    /// `AppControllerDepsAdapter` implements `IOfflineQueueDeps` + `ITicketSyncDeps` against
    /// this AppController and forwards every method to AppController-private state (`Cache`,
    /// `Backend`, `AvailableFields`, the connectivity probe state, etc.). The previous
    /// `friend class OfflineQueueService;` + `friend class TicketSyncService;` declarations
    /// were replaced by this single friend during the item 11 / 12 Phase 2 extraction —
    /// the services now hold an `IOfflineQueueDeps& / ITicketSyncDeps&` reference and never
    /// touch AppController internals directly. Tests substitute `FakeOfflineQueueDeps` /
    /// `FakeTicketSyncDeps` so they can exercise the services without an AppController.
    friend class AppControllerDepsAdapter;

  public:
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    ~AppController() override;
#else
    ~AppController();
#endif

    struct FieldEditResult {
        bool Ok = false;
        std::string Error;
        std::unordered_map<std::string, std::string> UpdatedDisplayValues;
    };

    /// Inject a custom tracker-backend factory. Must be called BEFORE `Initialize` to take
    /// effect on the first backend instantiation. Tests / Unreal-host embeddings use this
    /// to substitute a mock or alternative-transport client; the default standalone build
    /// leaves it null and `Initialize` lazily wires `DefaultTrackerBackendFactory`.
    void SetBackendFactory(std::unique_ptr<ITrackerBackendFactory> factory);

    void Initialize(const std::string& dbPath, const std::string& backendType);

    /// True iff the calling thread is the one that called `Initialize` (the UI thread).
    /// Used by Command handlers to decide whether to mutate UI state inline (safe) or post
    /// the mutation to `mainThreadDispatcher` (required when called from an MCP / Lua worker).
    /// Recorded once in `Initialize`; reads are atomic loads — safe from any thread.
    bool IsOnUiThread() const;

    /// Unified Command System registry. See docs/plans/shipped/command-system-plan.md.
    /// Lifetime: created in `Initialize`; the same instance feeds the CLI, the
    /// MCP plugin's tools/list + tools/call, the Lua `commands.invoke` binding,
    /// and the in-app Ctrl+Shift+P palette.
    smatchet::cmd::CommandRegistry& Commands();
    const smatchet::cmd::CommandRegistry& Commands() const;

    /// Scenario runner — feeds from scenario.run / scenario.cancel / scenario.list.
    /// Tick is driven per-frame by SmatchetUI::Draw.
    smatchet::cmd::ScenarioRunner& Scenarios();
    const smatchet::cmd::ScenarioRunner& Scenarios() const;

    /** Path passed to `Initialize` (may be relative to the process working directory). */
    const std::string& GetLocalCacheDbPath() const { return localCacheDbPath_; }
    /** Absolute path for display; falls back to the raw path if resolution fails. */
    std::string GetResolvedLocalCacheDbPath() const;
    /**
     * UI thread: closes SQLite, deletes the cache file (and WAL sidecars), opens a new empty database,
     * clears in-memory tickets, and resets streaming sync state. On success, call `SyncWithBackend`
     * to refill from the tracker.
     */
    bool RecreateLocalCacheDatabase(std::string& outError);

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
    void AddAutomationLogSink(std::function<void(const std::string&)> sink);
    /** Drop all sinks. Call before destroying plugins to avoid dangling `[this]` captures. */
    void ClearAutomationLogSinks();

    /// Register a sink that receives Lua error messages (automation runner + setup script
    /// failures). Called with a clean message string — no `[LUA]` / `[ERROR]` prefix; sink
    /// is responsible for presentation. Safe to call from any thread (the background
    /// automation worker may invoke it); sinks themselves are expected to be UI-thread-safe
    /// (e.g. via mainThreadDispatcher). Call from OnEarlyInit only.
    void AddAutomationErrorSink(std::function<void(const std::string&)> sink);

    /// Atomically reads and clears the "open Scripting window" request that the background
    /// automation worker sets on a Lua error. UI-thread: call once per frame in OnDraw.
    /// Returns true if the plugin should bring the Scripting window to the foreground.
    bool ConsumeScriptingWindowRequest();

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
    void RequestAppQuit() const;

    /** Standalone / embedded host: set so Preferences can start or stop MCP without app restart. */
    void SetRuntimePluginHost(PluginHost* host);
    PluginHost* RuntimePluginHost() const { return runtimePluginHost_; }

#if defined(SMATCHET_WITH_MCP)
    /** Bounded ring buffer of MCP-related actions (thread-safe). */
    void AppendMcpActivity(const std::string& line);
    std::vector<std::string> CopyMcpActivityLog() const;
    /** MCP HTTP server: any routed request after auth gate (worker threads). */
    void NotifyMcpClientHttpActivity();
    /** @return false if no client request has been recorded yet this process. */
    bool TryGetMcpLastClientHttpActivity(std::chrono::steady_clock::time_point* out) const;
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
    bool HasAiAssistantController() const { return aiAssistant_ != nullptr; }
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
    using AttachmentViewerHandler =
        std::function<void(const std::string& localPath, const std::string& mimeType, const std::string& filename)>;
    void SetAttachmentViewerHandler(AttachmentViewerHandler handler);
    using AttachmentPreviewHandler = std::function<bool(const std::string& localPath, const std::string& mimeType,
                                                        const std::string& filename, const std::string& sourceUrl)>;
    void SetAttachmentPreviewHandler(AttachmentPreviewHandler handler);
    struct AttachmentDescriptor {
        std::string Filename;
        std::string Url;
        std::string MimeType;
    };
    using AttachmentCollectionHandler = std::function<void(const std::vector<AttachmentDescriptor>& attachments)>;
    void SetAttachmentCollectionHandler(AttachmentCollectionHandler handler);
    void ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments);

    /**
     * Optional host hook for native multi-file open (new-issue attachments, etc.).
     * Invoked on the UI thread; implementation may block (e.g. Win32 IFileDialog).
     * If unset, RequestOpenFilePaths completes with an empty vector.
     */
    using OpenFilePathsHandler =
        std::function<void(bool allowMultiple, const std::string& initialDirectoryUtf8,
                           std::function<void(std::vector<std::string> absolutePathsUtf8)> onComplete)>;
    void SetOpenFilePathsHandler(OpenFilePathsHandler handler);
    void RequestOpenFilePaths(bool allowMultiple, const std::string& initialDirectoryUtf8,
                              std::function<void(std::vector<std::string>)> onComplete) const;

    /**
     * Open an attachment (image/pdf/etc) without requiring Basic Auth headers in the browser.
     * - If AttachmentViewerHandler is set: downloads to a temp file and calls the handler.
     * - Otherwise, for image mime types: downloads and offers in-app preview handler.
     * - If no host/in-app handler path is available: falls back to OpenUrl(url).
     */
    void OpenAttachment(const std::string& url, const std::string& filename, const std::string& mimeType);
    /** Download to temp then open local file in OS default app (matches Unreal attachment viewer). */
    void OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename, const std::string& mimeType);
    bool DownloadAttachmentForPreview(const std::string& url, const std::string& filename, const std::string& mimeType,
                                      std::string* outError = nullptr);
    std::string GetAppVersion() const;
    std::string GetGitHubReleaseRepo() const;
    AppUpdateInfo CheckForAppUpdate(bool includePrerelease = false) const;
    /// Downloads + launches the installer. Blocking — callers must dispatch this on a worker
    /// thread via `LaunchBackgroundTask`. The optional `cancelFlag` is polled inside the cpr
    /// write callback; when set to `true` the download aborts cleanly and the partial file is
    /// removed. On cancel returns `false` with `outError == "Download cancelled."`.
    bool DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl, const std::string& assetName,
                                          std::string& outError,
                                          std::shared_ptr<std::atomic<bool>> cancelFlag = {}) const;

    void InitLua();
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    void InitLuaCore(sol::state& state);
    void InitLuaUi(sol::state& state);
    /// Build a fresh per-call sol::state for off-UI-thread (MCP / automation
    /// worker) Lua execution: InitLuaCore + the `__smatchet_app_ui` alias. The
    /// returned state shares no lua_State with the UI-thread `lua` member, which
    /// is the whole point — running two threads through one lua_State is UB.
    /// See docs/plans/shipped/mcp-lua-fresh-state-race.md.
    void PrepareFreshLuaState(sol::state& state);
    /// Replay the registered setup scripts (`activeSetupScripts_`) onto `state`
    /// under `sandbox` so global helpers / actions / mcp.register_tool definitions
    /// are present. Snapshots `activeSetupScripts_` under `automationJobMutex_`.
    void ReplayActiveSetupScripts(sol::state& state, sol::environment& sandbox);
#endif

    std::string ResolveLuaScriptPath(const std::string& filename) const;
    /** Basenames of `*.lua` files in the configured scripts directory (non-recursive). */
    std::vector<std::string> ListLuaScriptFiles() const;

    /**
     * Resolve a URL or local path for field icons / Lua `imgui.image`.
     * Allows http(s) URLs; local files must lie under the Lua scripts directory or the runtime asset directory.
     */
    std::string ResolveFieldIconAssetPath(const std::string& pathOrUrl) const;

    void RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds);
    void RunFlatScriptAsync(const std::string& scriptPath);

    std::string GetAutomationScriptContent();
    bool SaveAutomationScriptContent(const std::string& content, std::string& outError);

    /** Run a Lua file once (e.g. SmatchetHooks.lua) to register UI hooks; errors go to automation log sinks. */
    void RunLuaSetupScript(const std::string& scriptPath);

    /** Present with or without Lua build; no-op / empty when `SMATCHET_WITH_LUA_AUTOMATION` is off. */
    std::vector<std::string> GetLuaTicketActionNames() const;
    void ExecuteLuaTicketAction(const std::string& name, const std::string& issueId);
    std::vector<std::string> GetLuaGlobalActionNames() const;
    void ExecuteLuaGlobalAction(const std::string& name);
    /**
     * Run a one-off Lua chunk from the automation UI (same globals as hooks: smatchet, ui, tracker, …).
     * On failure sets @p outError; on success clears @p outError and may set @p outResultSummary from the
     * first return value (short string / JSON, truncated when long).
     */
    bool ExecuteLuaConsoleSnippet(const std::string& code, std::string& outError, std::string& outResultSummary);
    /** Lua `register_field_icon_map`; returns false when Lua automation is disabled. */
    bool TryGetFieldIconMapTarget(const std::string& fieldId, const TrackerField* field, const std::string& rawValue,
                                  std::string& outPathOrUrl) const;

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    /**
     * Lua InitLua uses one functor struct per binding (see AppController_LuaBindings.cpp) so sol2/GCC never
     * merges unrelated lambdas or member-wrappers that share the same demangled metatable name.
     */
    void LuaLogInfoBind(const std::string& msg) override;
    std::tuple<sol::object, std::string> LuaGetTicketBind(sol::state_view sv, const std::string& issueId) override;
    std::tuple<sol::object, std::string> LuaDecodeJsonBind(sol::state_view sv, const std::string& s) override;
    /// Recorded-command-list cell renderer: Lua provider returns a static draw recording that
    /// the C++ side replays every frame until one of the cache-key inputs changes. See
    /// docs/plans/shipped/lua-recorded-cmd-list.md.
    void LuaRegisterFieldDisplayCachedBind(const std::string& fieldId, sol::function fn);
    void LuaUnregisterFieldDisplayCachedBind(const std::string& fieldId);
    void LuaRegisterFieldDisplayCachedByNameBind(const std::string& displayName, sol::function fn);
    void LuaUnregisterFieldDisplayCachedByNameBind(const std::string& displayName);
    void LuaRegisterFieldIconMapBind(const std::string& fieldKey, sol::table map, sol::optional<bool> byName);
    void LuaUnregisterFieldIconMapBind(const std::string& fieldKey, sol::optional<bool> byName);
    void LuaImGuiTextBind(const std::string& s);
    void LuaImGuiTextUnformattedBind(const std::string& s);
    bool LuaImGuiImageBind(const std::string& path, float w, float h);
    /// Window register / unregister: thread-safe via mainThreadDispatcher when off the UI
    /// thread, mid-iteration-safe via pendingLuaWindowOps_ when re-entered from a callback.
    void LuaUiRegisterWindowBind(const std::string& name, sol::function drawFn);
    void LuaUiUnregisterWindowBind(const std::string& name);
    void LuaUiRegisterTicketActionBind(const std::string& name, const std::string& callbackFuncName);
    void LuaUiRegisterGlobalActionBind(const std::string& name, const std::string& callbackFuncName);
    void LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) override;
    std::vector<CachedTicket> LuaGetActiveTicketsBind() override;
    /** Live create or offline queue from a Lua spec table; see LUA_GUIDE.md. */
    std::tuple<sol::object, std::string> LuaCreateIssueBind(sol::state_view sv, sol::table spec) override;
    void ClearLuaTicketContextGlue();

    // --- ILuaBindingHost forwarders (interface required for the lifted InitLuaCore
    // glue functions in AppController_LuaBindingsCore.cpp). ---
    smatchet::cmd::CommandRegistry& LuaCommands() override { return Commands(); }
    AppController* AppForCommandContext() override { return this; }

    struct McpToolDefinition {
        std::string name;
        std::string description;
        nlohmann::json parametersSchema;
        sol::protected_function callback;
    };
    /// Parse a Lua `mcp.register_tool` definition table into the name / description /
    /// parametersSchema fields of `out` (does NOT set `callback`). Shared by
    /// LuaMcpRegisterToolBind and the per-call register_tool override that
    /// ExecuteLuaMcpTool installs on its fresh state.
    static void ParseMcpToolDef(const sol::table& toolDef, McpToolDefinition& out);
    /** Thread-safe snapshot (e.g. MCP server thread vs Lua registration on the app thread). */
    std::vector<McpToolDefinition> GetLuaMcpTools() const;
    std::string ExecuteLuaMcpTool(const std::string& name, const std::string& paramsJson, std::string& outError);
    std::string ExecuteLuaSnippetForMcp(const std::string& code, const nlohmann::json& args, std::string& outError);
    std::string ExecuteLuaScriptForMcp(const std::string& scriptName, const nlohmann::json& args,
                                       std::string& outError);
    void DrawLuaWindows();
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
                                           const std::vector<std::string>& extraScripts, std::string& outError);
    /// Convenience overload — equivalent to passing an empty `extraScripts`.
    bool ScenarioRegisterLuaCachedProvider(const std::string& fieldId, const std::string& luaFnName,
                                           std::string& outError);
    /// Inverse of `ScenarioRegisterLuaCachedProvider`. Restores the user-side provider that
    /// was displaced at register time (if any), or erases the entry if no prior existed. Also
    /// drops every cache entry for that field so the restored provider re-records cleanly.
    /// No-op if not registered via the scenario surface, or in the no-Lua build.
    void ScenarioUnregisterLuaCachedProvider(const std::string& fieldId);

    /// Scenario hook: clear every `luaFieldCache_` entry so subsequent cells re-record. Used
    /// by fuzz scenarios that need to exercise the recorder path each frame (otherwise the
    /// cache hit-rate becomes 100% after first paint and only the initial visible cells are
    /// fuzzed). No-op stub in the no-Lua build.
    void ScenarioInvalidateLuaFieldCache();

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    /// Drop entries from luaFieldCache_. No-arg = drop all; (ticketId) = drop one ticket's
    /// entries; (ticketId, fieldId) = drop a single cell. Off-UI hops the dispatcher.
    /// Lua-only because the sol::optional<std::string> overload threads through sol2.
    void LuaUiInvalidateFieldCacheBind(sol::optional<std::string> ticketId, sol::optional<std::string> fieldId);

#endif

    /**
     * Sync issues from the tracker into the local cache.
     * Pass the in-memory UI config + views store when syncing from the app so JQL/fields match
     * the active view without relying on an immediate disk round-trip.
     */
    void SyncWithBackend(const TrackerConfig* configOverride = nullptr, const ViewsStore* viewsOverride = nullptr);

    /** Process queued streaming ticket batches on the UI thread (frame-budgeted). */
    void TickStreamingApply();

    /** Clears the live ticket-sync warning banner (e.g. before kicking off a background fetch). */
    void ClearLastTrackerTicketSyncWarning();

    /**
     * Runs FetchIssues under an internal mutex (safe with concurrent UI-triggered syncs).
     * Does not touch the SQLite cache; pair with ApplyIssueFetchPack on the UI thread.
     */
    TrackerIssueFetchPack FetchIssuesForActiveView(const TrackerConfig* configOverride = nullptr,
                                                   const ViewsStore* viewsOverride = nullptr);

    /** Merges fetch results into the local cache and updates connectivity banners. */
    void ApplyIssueFetchPack(TrackerIssueFetchPack pack);

    void RefreshLocalData();
    /** Reload ActiveTickets from cache and kick per-issue-type editmeta warmup (same tail as SyncWithBackend). */
    void RefreshLocalDataAndWarmIssueTypeMeta();

    void UpdateTicket(const CachedTicket& ticket);

    std::vector<CachedTicket> GetActiveTickets() const;
    /** Cheap read: shared_ptr to last published ticket list (thread-safe with MCP / workers). */
    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const;
    std::uint64_t GetActiveTicketsRevision() const { return ActiveTicketsRevision.load(); }
    /// De-inlined as of item 11 Phase 1C: the streaming-sync state lives on TicketSyncService.
    /// Defined in AppController.cpp where TicketSyncService.h is included; delegates to
    /// `ticketSync_->IsActive()`.
    bool IsStreamingSyncActive() const;

    /** Bumped when the field catalog changes (fetch, error clear, etc.); UI sort cache should invalidate. */
    std::uint64_t GetFieldCatalogRevision() const { return TrackerFieldCatalogRevision.load(); }

    bool RefreshFieldCatalog(const TrackerConfig& cfg);
    /** PR 6: refetch the catalog scoped to a specific project. The project key is plumbed to
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

    const std::vector<TrackerField>& GetAvailableFields() const { return AvailableFields; }
    const std::vector<TrackerComponent>& GetAvailableComponents() const { return AvailableComponents; }
    /// Last-fetched user catalog. May be empty before the first catalog fetch completes
    /// or when the active backend doesn't surface a users endpoint.
    const std::vector<TrackerUser>& GetAvailableUsers() const { return AvailableUsers; }
    const std::string& GetFieldCatalogError() const { return LastTrackerFieldCatalogError; }
    const std::string& GetFieldCatalogWarning() const { return LastTrackerFieldCatalogWarning; }
    /** Set when a live JQL refresh failed with a transport-style error; UI may show cached tickets. */
    const std::string& GetLastTicketSyncWarning() const { return LastTrackerTicketSyncWarning; }

    /**
     * One banner for field-catalog error/warning, ticket-list cache warning, and optional session note
     * (e.g. Views dashboard users-fetch warning). Prefer this over separate `GetFieldCatalogWarning` /
     * `GetLastTicketSyncWarning` lines in headers.
     */
    TrackerConnectivityBannerForUi
    GetTrackerConnectivityBannerForUi(const std::string* sessionCatalogNote = nullptr) const;

    /** Last outcome of periodic tracker probe (UI thread). */
    enum class TrackerConnectivityState {
        Unknown,
        AuthenticatedReachable,
        ReachableAuthOrConfigError,
        TransportDown,
        ServiceUnavailable,
    };
    /** Rate-limited background probe; updates connectivity state and recovery latch. */
    void TickTrackerConnectivityMonitor(const TrackerConfig& cfg);
    /** Latest reachability from background probe (or after a successful live backend request). */
    TrackerConnectivityState GetLastTrackerConnectivityState() const { return lastTrackerConnectivityState_; }
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
                         const std::string& error);
    void SetFieldCatalog(std::vector<TrackerField> fields, std::vector<TrackerComponent> components,
                         std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta, const std::string& error);
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
        return AvailableIssueTypeMeta;
    }

    /** Read-only accessor used by UI sites (e.g. `ResolveProjectForDraft`) to call
     *  `ITrackerConnectivity::ExtractProjectFromQuery` / `GetTrackerType`. May be null before
     *  `Initialize` has wired up the factory. Do not retain the pointer past the current frame. */
    // All reads of the `Backend` member go through std::atomic_load and all writes through
    // std::atomic_store (ADR 0012): the slot is reassigned live on a tracker swap, and a
    // shared_ptr *instance* is not itself thread-safe to copy/assign concurrently (C++14).
    const ITrackerBackend* GetTrackerBackend() const { return std::atomic_load(&Backend).get(); }
    // PR 4b: non-const accessor for callers that invoke mutating client methods (e.g.
    // ListProjects() which populates a per-client in-memory cache).
    ITrackerBackend* GetTrackerBackendMutable() { return std::atomic_load(&Backend).get(); }
    /** Strong (shared) handle to the active backend, for OFF-THREAD work that must
     *  outlive a live tracker swap. `Backend` is reassigned live on a tracker change
     *  (`SyncWithBackend`→`SetBackend`), which frees the old object; a worker that
     *  captured only a raw pointer would dangle. Capture this `shared_ptr` instead so
     *  the old backend stays alive until the worker drops it. Atomic-loaded so the
     *  snapshot itself can't race the swap. See ADR 0012. */
    std::shared_ptr<ITrackerBackend> BackendShared() const { return std::atomic_load(&Backend); }

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
     */
    std::future<IssueCreateResult> CreateIssueAsync(const IssueDraft& draft);

    /**
     * Persist `draft` to SQLite and return the queued row id. Useful when the
     * user wants to stage creates before going online, or when a create fails
     * due to connectivity errors. Replayed by `TickOfflineCreates`.
     */
    std::int64_t QueueCreateOffline(const IssueDraft& draft);

    /**
     * Replay any queued offline creates. No-op when the queue is empty or the
     * backend is unreachable. Intended to be polled from the main tick.
     */
    void TickOfflineCreates();

    /** Current depth of the offline create queue (SQLite row count). */
    size_t GetPendingCreateCount() const;
    /** Active offline create rows (`pending_creates`), oldest first. */
    std::vector<PendingCreate> GetPendingCreates() const;
    size_t GetDeadPendingCreateCount() const;
    std::vector<DeadPendingCreate> GetDeadPendingCreates() const;

    struct DeadLetterRestoreSummary {
        int Restored = 0;
        int Failed = 0;
    };
    /** Move selected dead-letter rows back to the active offline queue (attempts reset to 0). */
    DeadLetterRestoreSummary RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds);
    /** One-shot startup message from legacy max-attempt pending drop; empty if none. */
    std::string TakeLegacyPendingStartupBanner();

    struct DeadLetterDeleteSummary {
        int Deleted = 0;
        int Failed = 0;
    };
    /** Permanently remove dead-letter rows by `pending_creates_dead.dead_id`. */
    DeadLetterDeleteSummary DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds);

    struct PendingQueueDeleteSummary {
        int Deleted = 0;
        int Failed = 0;
    };
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
                                       const std::string& originalRichValue = std::string());

    /** Replay queued offline field edits (rate-limited; called from UI tick). */
    void TickOfflineFieldEdits();

    std::vector<PendingFieldEditRecord> GetPendingFieldEdits() const;
    std::vector<DeadPendingFieldEdit> GetDeadPendingFieldEdits() const;
    /// Replace the queued payload with a user-resolved version and clear the conflict flag.
    /// The edit will be retried on the next TickOfflineFieldEdits pass.
    void ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedMarkdown, const std::string& richKind);

    struct PendingFieldEditDeleteSummary {
        int Deleted = 0;
        int Failed = 0;
    };
    PendingFieldEditDeleteSummary DeletePendingFieldEdits(const std::vector<std::int64_t>& ids);

    struct DeadFieldEditDeleteSummary {
        int Deleted = 0;
        int Failed = 0;
    };
    DeadFieldEditDeleteSummary DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds);

    /**
     * Background-fetch issues by key (Jira search) and merge into the local cache.
     * Used so bulk-import update rows can show field diffs when keys are outside the current JQL.
     */
    void PrefetchIssueTicketsForKeys(const std::vector<std::string>& issueKeys, bool includeAlreadyActive = false);
    bool IsBulkImportPrefetchInFlight(const std::string& issueKey) const;

    const TrackerField* FindFieldById(const std::string& fieldId) const
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        override
#endif
        ;

    /** Component options valid for one Jira project key (e.g. "PROJ"), warmed async for cross-project
     *  grid views. Returns a by-value copy taken under availableFieldsMutex_; empty when the project
     *  has not been warmed yet (caller falls back to the global components catalog). */
    std::vector<TrackerFieldOption> GetComponentOptionsForProject(const std::string& projectKey) const;

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

    bool SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                         const std::vector<std::string>& rawValues, std::string& outError)
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        override
#endif
        ;
    bool SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues,
                                    const std::string& originalEstimateSnapshot,
                                    const std::string& remainingEstimateSnapshot,
                                    const std::string& issueTypeKeySnapshot, FieldEditResult& outResult);

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
    bool ApplyFieldEditResult(const std::string& issueId, const FieldEditResult& result, std::string& outError);
    /** Best-effort async warmup so edit controls can reflect per-issue permissions sooner. */
    void WarmIssueEditMetaAsync(const std::string& issueId);

    bool FetchIssueWatchers(const std::string& issueKey, std::vector<TrackerUser>& outWatchers,
                            std::string& outError) const;

    bool AddIssueWatcher(const std::string& issueKey, std::string& outError);

    bool FetchIssueVotes(const std::string& issueKey, std::vector<TrackerUser>& outVoters, std::string& outError,
                         int* outVoteCount = nullptr, bool* outHasVoted = nullptr,
                         bool* outVotersInResponse = nullptr) const;

    bool SearchUsersByQuery(const std::string& query, std::vector<TrackerUser>& outUsers, std::string& outError) const;

    bool AddIssueCommentPlain(const std::string& issueKey, const std::string& plainText, std::string& outError);

    bool SubmitWorklog(const std::string& issueId, const std::string& timeSpent, const std::string& timeRemaining,
                       const std::string& adjustEstimate, const std::string& workDescription,
                       const std::string& startedDate, std::string& outError);

    bool AddIssueCommentAnnotateContext(const std::string& issueKey, const std::string& p4User,
                                        const std::string& functionName, const std::string& filePath, int lineNumber,
                                        const std::string& changelist, const std::string& date, bool approximated,
                                        const std::string& codeSnippet, std::string& outError);

    bool FetchUserGroupNames(const std::string& accountId, std::vector<std::string>& outGroupNames,
                             std::string& outError) const;

  private:
    std::unique_ptr<LocalCacheManager> Cache;
    std::unique_ptr<ITrackerBackendFactory>
        backendFactory_; ///< Lazy-initialized in `Initialize` if not pre-set via `SetBackendFactory`.
    /// Shared (not unique) so off-thread workers can capture a strong handle that
    /// survives a live tracker swap freeing this slot. All reads go through
    /// `std::atomic_load`, all writes through `std::atomic_store`/`atomic_exchange`
    /// (a shared_ptr instance is not concurrently copy/assign-safe in C++14). See ADR 0012.
    std::shared_ptr<ITrackerBackend> Backend;
    /// Defer-free graveyard (ADR 0012): a live tracker swap retires the OLD backend here
    /// instead of freeing it, so raw subobject pointers (Reader/Mutations/Connectivity)
    /// captured by in-flight workers before the swap stay valid. Drained only in
    /// `~AppController` (after `JoinBackgroundTasks` joins all workers). Tracker switches are
    /// rare user actions, so this holds at most a handful of small backend objects per session.
    std::mutex retiredBackendsMutex_;
    std::vector<std::shared_ptr<ITrackerBackend>> retiredBackends_;

  public:
    /// Retire a swapped-out backend into the defer-free graveyard (see `retiredBackends_`).
    /// Called by `AppControllerDepsAdapter::SetBackend`. Thread-safe.
    void RetireBackend(std::shared_ptr<ITrackerBackend> old);

  private:
    /// Implements `IOfflineQueueDeps` + `ITicketSyncDeps`. Constructed eagerly in `Initialize`
    /// before `offlineQueue_` / `ticketSync_` so they can capture an interface reference at
    /// construction time. The adapter never outlives this AppController (owned by it), so the
    /// `AppController&` member it stores is trivially valid for the adapter's full lifetime.
    std::unique_ptr<AppControllerDepsAdapter> depsAdapter_;
    /// Owns the offline-create / offline-field-edit replay queues and their dead-letter management.
    /// Constructed lazily in `Initialize`. Public AppController methods (`QueueCreateOffline`,
    /// `GetPendingCreates`, etc.) are thin delegators that forward to this service. See
    /// BACKLOG_CODE_REVIEW.md §1.7 / §7 item 12.
    std::unique_ptr<OfflineQueueService> offlineQueue_;
    /// Owns the streaming-sync FSM (worker thread, batch queue, supersede/cancel transitions)
    /// and applies fetched batches to the cache. Constructed eagerly in `Initialize` alongside
    /// `offlineQueue_`. Public AppController methods (`ApplyIssueFetchPack`,
    /// `CancelAndJoinActiveStreamingSync`, etc.) are thin delegators that forward here. See
    /// BACKLOG_CODE_REVIEW.md §1.7 / §7 item 11.
    std::unique_ptr<TicketSyncService> ticketSync_;
    /// Owns the Lua sandbox + automation worker + Lua bindings. Phase 1A of the item 14
    /// extraction only routes log-sink methods through it; later phases migrate the Lua
    /// state and worker thread. Constructed eagerly in `Initialize` to keep the
    /// `Add*LogSink` calls from `OnEarlyInit` working.
    std::unique_ptr<LuaAutomationHost> luaHost_;
    /// Unified Command System registry — owns the catalog of named commands and dispatches them to
    /// CLI / MCP / Lua / Palette callers. Constructed eagerly in `Initialize` after the tracker
    /// backend so handlers can capture `*this` and safely call AppController methods.
    std::unique_ptr<smatchet::cmd::CommandRegistry> commandRegistry_;
    std::unique_ptr<smatchet::cmd::ScenarioRunner> scenarioRunner_;
    std::vector<CachedTicket> ActiveTickets;
    mutable std::shared_ptr<const std::vector<CachedTicket>> activeTicketsPublished_;
    std::atomic<std::uint64_t> ActiveTicketsRevision{0};
    std::atomic<std::uint64_t> TrackerFieldCatalogRevision{0};
    mutable std::mutex availableFieldsMutex_; ///< Guards AvailableFields writes (UI) vs. FindFieldById reads (workers).
    std::vector<TrackerField> AvailableFields;
    std::vector<TrackerComponent> AvailableComponents;
    std::vector<TrackerIssueTypeCreateMeta> AvailableIssueTypeMeta;
    /// Last-fetched user catalog (full payload from `/rest/api/3/users/search` for Jira).
    /// Surfaced to JQL autocomplete to suggest assignees / reporters by display name; the
    /// engine filters out non-human accounts (AccountType == "app" / "customer") at query
    /// time so the raw list stays pristine for other callers.
    std::vector<TrackerUser> AvailableUsers;
    std::string LastTrackerFieldCatalogError;
    std::string LastTrackerFieldCatalogWarning;
    std::string LastTrackerTicketSyncWarning;
    bool fieldCatalogEverLoaded_ = false;
    /** PR 6: project key for the most-recent in-flight catalog fetch — used by SetFieldCatalog
     *  to write the snapshot under the right per-project cache entry now that
     *  TrackerConfig::ProjectKey / PlaneProjectId are gone. Guarded by availableFieldsMutex_. */
    std::string currentCatalogProjectKey_;
    /** Per-project component option lists for cross-project grid views, keyed by Jira project key
     *  (e.g. "PROJ"). Warmed async by WarmIssueTypeEditMetaAtStartAsync; read by the components
     *  MultiSelect editor via GetComponentOptionsForProject. In-memory only (no disk persistence).
     *  Guarded by availableFieldsMutex_. */
    std::unordered_map<std::string, std::vector<TrackerFieldOption>> projectComponentOptions_;
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
    std::function<void(const std::string&)> OpenUrlHandler;
    std::function<void()> CloseEmbeddedUiHandler;
    std::function<void()> RequestAppQuitHandler;
    AttachmentViewerHandler AttachmentViewerHandlerCallback;
    AttachmentPreviewHandler AttachmentPreviewHandlerCallback;
    AttachmentCollectionHandler AttachmentCollectionHandlerCallback;
    OpenFilePathsHandler OpenFilePathsHandlerCallback;

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    // Recorder + replay value types live in `AppController_LuaTypes.h` so this header
    // stays under ~900 LOC. The `using` aliases below preserve the existing
    // `AppController::ImCmd` / `LuaFieldCacheEntry` / `LuaWindowEntry` / `PendingLuaWindowOp`
    // qualified names every call site uses (notably the anonymous-namespace `LuaDrawList`
    // class in AppController_LuaBindings.cpp). Members holding these types stay `private`
    // further down. See docs/plans/shipped/large-files-and-phase-2.md § A5 and
    // docs/plans/shipped/lua-recorded-cmd-list.md.
  public:
    using ImCmd = smatchet::lua::ImCmd;
    using LuaFieldCacheEntry = smatchet::lua::LuaFieldCacheEntry;
    using LuaWindowEntry = smatchet::lua::LuaWindowEntry;
    using PendingLuaWindowOp = smatchet::lua::PendingLuaWindowOp;

    /// UI-thread helper: applies a window register / unregister / invalidate op immediately
    /// when DrawLuaWindows is not iterating, otherwise enqueues onto pendingLuaWindowOps_
    /// for in-frame drain. Off-thread callers must hop the dispatcher BEFORE this.
    void ApplyOrQueueLuaWindowOp(PendingLuaWindowOp op);

  private:
    // Member-order invariant: `sol::state lua` MUST be declared BEFORE every container that
    // stores `sol::protected_function`. C++ destroys members in reverse declaration order, so
    // the containers below tear down first (their Lua-handle members touch a still-alive
    // state), then `lua` last. Inverting this order is a UAF — see plan §Shutdown ordering.
    // Threading invariant: `lua` is driven EXCLUSIVELY by the UI thread (DrawLuaWindows,
    // cell-providers, ExecuteLuaConsoleSnippet). Off-UI-thread Lua execution (MCP run_lua /
    // registered-tool handlers on httplib workers, the automation worker) MUST run on a fresh
    // per-call sol::state via PrepareFreshLuaState — never `lua`. Two threads through one
    // lua_State is UB. See docs/plans/shipped/mcp-lua-fresh-state-race.md.
    sol::state lua;
    std::unordered_map<std::string, sol::protected_function> fieldDisplayCachedProviders_;
    /** Lowercased Jira field display name (from catalog) -> handler. */
    std::unordered_map<std::string, sol::protected_function> fieldDisplayCachedProvidersByName_;
    /// Per-cell cmd-list cache. Key = `ticket.id + '\0' + fieldId`. UI-thread only.
    std::unordered_map<std::string, LuaFieldCacheEntry> luaFieldCache_;
    /// Bumped on provider (un)register. Init to 1 so cached entries with `providerGen=0`
    /// always miss on first compare.
    std::atomic<std::uint64_t> luaProviderGen_{1};
    /// Bumped by NotifyLuaTicketDataChanged; cells use per-entry comparison instead.
    std::atomic<std::uint64_t> luaWindowDataGen_{1};
    std::vector<LuaWindowEntry> luaWindows_;
    std::vector<PendingLuaWindowOp> pendingLuaWindowOps_;
    /// Snapshot of user-side providers displaced by a scenario register call. Keyed by
    /// fieldId; the value is the *prior* provider (may be empty if the field had none).
    /// `ScenarioUnregisterLuaCachedProvider` restores from this map so a scenario run never
    /// silently destroys a user-side provider for the session.
    std::unordered_map<std::string, sol::protected_function> scenarioPriorFieldProviders_;
    /// Set membership tracks fields whose prior was *empty* (no provider) — distinguishes
    /// "had nothing, restore to nothing" from "field absent in scenario map → leave alone".
    std::unordered_set<std::string> scenarioPriorEmptyFields_;
    /// True while inside DrawLuaWindows iteration. Callbacks fired during replay route
    /// register/unregister/invalidate ops into pendingLuaWindowOps_ instead of mutating
    /// luaWindows_ directly. Plain bool — UI-thread-only.
    bool inDrawLuaWindows_ = false;
    std::vector<McpToolDefinition> luaMcpTools_;
    mutable std::mutex luaMcpToolsMutex_;
    /// Guards `luaTicketActions_` and `luaGlobalActions_`. Both vectors are mutated from
    /// the worker thread when `AutomationWorkerLoop` re-executes setup scripts (e.g.
    /// `SmatchetHooks.lua`) which call `ui.register_ticket_action` / `ui.register_global_action`
    /// — these resolve `__smatchet_app_ui` on the worker `bgState` and call
    /// `LuaUiRegister{Ticket,Global}ActionBind` on the worker thread. Meanwhile UI thread reads
    /// them every frame via `Get{Ticket,Global}ActionNames()` (LuaConsolePlugin). Without this
    /// lock the std::vector erase/push_back reallocates concurrently with std::transform — UB,
    /// crash. Mirrors the `luaMcpToolsMutex_` pattern for the same shape on `luaMcpTools_`.
    mutable std::mutex luaActionsMutex_;
    std::vector<std::pair<std::string, std::string>> luaTicketActions_;
    std::vector<std::pair<std::string, std::string>> luaGlobalActions_;
    mutable std::mutex fieldIconMapsMutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fieldIconMapsByFieldId_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fieldIconMapsByDisplayName_;

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

    /// Memoised result of `ResolveFieldIconAssetPath` keyed on the raw path-or-url input.
    /// Resolution does 2-3 `fs::weakly_canonical` syscalls on identical inputs hot-path-per-frame
    /// from `LuaDrawList::Replay`. Both base directories (`luaScriptsDirectory_`,
    /// `ConfigManager::GetRuntimeAssetDirectory`) are set once at startup and never re-assigned,
    /// so the function is pure w.r.t. its input string for the process lifetime. UI-thread only.
    static constexpr std::size_t kFieldIconAssetPathCacheCap = 256;
    mutable std::unordered_map<std::string, std::string> fieldIconAssetPathCache_;

    struct IssueEditMetaCache {
        bool loaded = false;
        /** Field id -> backend allows an update operation (set/add/remove). */
        std::unordered_map<std::string, bool> fieldCanEdit;
    };

    mutable std::mutex editMetaMutex_;
    std::unordered_map<std::string, IssueEditMetaCache> issueEditMeta_;
    std::unordered_map<std::string, IssueEditMetaCache> issueTypeEditMeta_;
    std::unordered_set<std::string> issueEditMetaWarmupInFlight_;

    /**
     * @param issueTypeKeyOverride if non-null and non-empty, used instead of scanning `ActiveTickets`
     *        for issuetype (safe for background threads that captured the key on the UI thread).
     * @param configSnapshot if non-null, used instead of ConfigManager::Load() (e.g. snapshot from main thread
     *        or loaded before InitLua to avoid parsing smatchet_config.json after Lua init in release builds).
     */
    bool EnsureIssueEditMetaLoaded(const std::string& issueId, std::string* outError = nullptr,
                                   const std::string* issueTypeKeyOverride = nullptr,
                                   const TrackerConfig* configSnapshot = nullptr);
    bool RefreshIssueEditMeta(const std::string& issueId, std::string* outError = nullptr,
                              const std::string* issueTypeKeyOverride = nullptr);
    void InvalidateIssueEditMeta(const std::string& issueId);
    void PruneEditMetaCacheToActiveTickets();
    /** @param trackerCfgForWorker credentials/settings copy for background fetch (never ConfigManager::Load inside
     * worker). */
    void WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker);
    void EnsureCatalogHistoryField();
    bool TryBuildFieldEditPayloadForNetwork(const std::string& issueId, const TrackerField& field,
                                            const std::vector<std::string>& rawValues,
                                            const std::string& originalEstimateSnapshot,
                                            const std::string& remainingEstimateSnapshot,
                                            const std::string& issueTypeKeySnapshot, nlohmann::json& outFieldsPayload,
                                            std::unordered_map<std::string, std::string>& outDisplayValues,
                                            std::string& outError);
    std::string ResolveIssueTypeKeyForIssue(const std::string& issueId) const;

  public:
    /// Spawn `task` on a tracked background thread. Threads are joined either
    /// when the producer completes or in `JoinBackgroundTasks` before
    /// destruction — never detached. Post results back to the UI thread via
    /// `mainThreadDispatcher.PostToMainThread` inside `task`.
    /// Public so non-member callers (grid field-edit pipeline, etc.) can
    /// dispatch HTTP / SQLite work off the UI thread without re-implementing
    /// thread-bookkeeping.
    void LaunchBackgroundTask(std::function<void()> task);

  private:
    void JoinBackgroundTasks();
    /// Join + erase any background workers whose task has completed (their `done` flag is set,
    /// so the join returns immediately — no UI-thread stall). Called on each new launch so the
    /// worker vector stays bounded mid-session instead of accumulating dead-but-joinable
    /// std::thread objects until shutdown (memory-budget-and-lifetime-hardening § Phase 4).
    /// Caller MUST hold `backgroundWorkersMutex_`.
    void reapFinishedBackgroundWorkersLocked_();

    void DrainTrackerConnectivityProbeFuture();
    void ApplyTrackerConnectivityProbeResult(const std::chrono::steady_clock::time_point now,
                                             const TrackerReachabilityProbeResult& r);
    bool IsConnectivityDegradedForProbeInterval(TrackerConnectivityState nextProbeState) const;
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now);
    static TrackerConnectivityState MapReachabilityProbeKind(TrackerReachabilityProbeKind k);

    void requestDeferredLiveTrackerBackendSuccessNotify_() const;
    void applyLiveTrackerReachabilityAfterSuccessfulBackendRequest_();

    std::chrono::steady_clock::time_point nextTrackerConnectivityProbeAt_{};
    bool trackerConnectivityProbeInFlight_ = false;
    std::future<TrackerReachabilityProbeResult> trackerConnectivityProbeFuture_;
    TrackerConnectivityState lastTrackerConnectivityState_ = TrackerConnectivityState::Unknown;
    std::string lastTrackerConnectivityDiagnostic_;
    bool trackerConnectivityRecoveryPending_ = false;
    std::atomic<bool> fieldCatalogRefetchAfterLiveTicketSyncPending_{false};
    mutable std::atomic<bool> deferredLiveTrackerBackendSuccessNotify_{false};

    mutable std::mutex activeTicketsMutex_;
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
    static constexpr size_t kMcpActivityLogMax = 100;
    mutable std::mutex mcpActivityMutex_;
    std::deque<std::string> mcpActivityLog_;
    /** `steady_clock` epoch offset in nanoseconds; 0 means no client HTTP activity yet. */
    std::atomic<std::uint64_t> mcpLastClientHttpActivityNs_{0};
    std::atomic<std::uint64_t> mcpHttpTrafficEpoch_{0};
#endif

#if defined(SMATCHET_WITH_AI)
    // Smatchet Assistant — Phase B. Held in a unique_ptr so the header can forward-declare
    // `AiAssistantController` and avoid pulling in AiTypes.h + AiClientFactory transitively.
    // Lifetime contract: constructed at the end of `Initialize` (after ConfigManager::Load
    // has settled the Ai* config fields); destroyed at the *top* of `~AppController` BEFORE
    // mainThreadDispatcher.BeginShutdown() fires, so any in-flight worker callback can
    // still post to the dispatcher during its shutdown drain.
    std::unique_ptr<AiAssistantController> aiAssistant_;
#endif

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    struct AutomationJob {
        enum class Type { RunAutoScript, TicketAction, GlobalAction, RunFlatScript };
        Type type;
        std::string scriptPathOrActionName;
        std::vector<std::string> selectedIds;
        std::string targetIssueId;
    };
    mutable std::mutex automationJobMutex_;
    std::condition_variable automationJobCv_;
    std::deque<AutomationJob> automationJobs_;
    // Lifetime contract (see ~AppController and AutomationWorkerLoop):
    //   - The worker reads `this` indirectly via each iteration's `bgState["__smatchet_app"] = this`.
    //   - `bgState` is a stack-local per iteration; it never escapes the iteration's try-block.
    //   - `~AppController` flips `automationWorkerShuttingDown_` and `automationWorker_.join()` BEFORE
    //     any AppController member is destroyed. The join therefore provides both the
    //     happens-before barrier and the guarantee that no live `bgState` (and no `__smatchet_app`
    //     pointer reachable from worker code) survives into member destruction.
    std::thread automationWorker_;
    std::atomic<bool> automationWorkerShuttingDown_{false};
    void AutomationWorkerLoop();
    void RunAutomationJob(sol::state& state, sol::environment& env, const AutomationJob& job);
    std::vector<std::string> activeSetupScripts_;
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
};

#endif
