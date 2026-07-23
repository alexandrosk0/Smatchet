#ifndef SMATCHET_APPCONTROLLER_IMPL_H
#define SMATCHET_APPCONTROLLER_IMPL_H

// Internal (src-only) definition of AppController::Impl — hardening #19, steps 19b + 19c.
// The COLD, sol2-/subsystem-heavy member STORAGE lives here, off the public
// AppController.h, so the ~100 header includers stop pulling this state (and, after
// 19c, sol2 itself) into their compile. This header is included ONLY by the
// AppController*.cpp translation units; it is never part of the public include graph.
// Step 19c: AppController.h no longer includes <sol/sol.hpp> and no longer derives from
// ILuaBindingHost. The interface implementation + every sol-typed binding method moved
// HERE onto Impl, which now `public ILuaBindingHost`. Impl holds `AppController& app_`
// (set in the ctor; never dangles — Impl is unique_ptr-owned by AppController) and
// forwards to the sol-free AppController Lua API for the bodies that stay on AppController.

#include "AppController.h"

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
#include "ILuaBindingHost.h"        // sol2 + the ILuaBindingHost interface + smatchet::lua::McpToolDefinition
#include "AppController_LuaTypes.h" // smatchet::lua::ImCmd / ... — uses sol::, so MUST follow ILuaBindingHost.h
#endif

#if defined(SMATCHET_WITH_AI)
// Fan-in Phase 4: AppController.h now forward-declares AiAssistantController, so the full type for
// the `aiAssistant_` (unique_ptr<AiAssistantController>) member declared below — and its dtor,
// instantiated in the AppController*.cpp TUs that own Impl — must be included directly here.
#include "AiAssistantController.h"
#endif

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
struct AppController::Impl : public ILuaBindingHost {
#else
struct AppController::Impl {
#endif
    /// Owning AppController. Set in the ctor (threaded through from AppController::AppController).
    /// A reference, never a pointer: Impl is a `unique_ptr<Impl>` member of AppController, so the
    /// owner strictly outlives the Impl and the reference can never dangle. The sol-typed binding
    /// methods below forward AppController-resident state through `app_`.
    AppController& app_;

    explicit Impl(AppController& app) : app_(app) {}

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    // Value-type aliases relocated from AppController.h (19c). Backed by AppController_LuaTypes.h.
    using ImCmd = smatchet::lua::ImCmd;
    using LuaFieldCacheEntry = smatchet::lua::LuaFieldCacheEntry;
    using LuaWindowEntry = smatchet::lua::LuaWindowEntry;
    using PendingLuaWindowOp = smatchet::lua::PendingLuaWindowOp;
#endif

    // ---- Member-order / destruction invariant (preserved verbatim from the former
    // AppController member block) ----
    // `sol::state lua` MUST be declared BEFORE every container that stores a
    // `sol::protected_function`. C++ destroys members in reverse declaration order, so the
    // protected_function containers below tear down FIRST (their Lua-handle members touch a
    // still-alive state), then `lua` destructs LAST. Inverting this order is a UAF — see
    // docs/plans §Shutdown ordering. Threading invariant: `lua` is driven EXCLUSIVELY by the
    // UI thread; off-UI-thread Lua runs on a fresh per-call sol::state (PrepareFreshLuaState).
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    sol::state lua;
    std::unordered_map<std::string, sol::protected_function> fieldDisplayCachedProviders_;
    /** Lowercased Jira field display name (from catalog) -> handler. */
    std::unordered_map<std::string, sol::protected_function> fieldDisplayCachedProvidersByName_;
    /// Per-cell cmd-list cache. Key = `ticket.id + '\0' + fieldId`. UI-thread only.
    std::unordered_map<std::string, LuaFieldCacheEntry> luaFieldCache_;
    std::vector<LuaWindowEntry> luaWindows_;
    std::vector<PendingLuaWindowOp> pendingLuaWindowOps_;
    /// Snapshot of user-side providers displaced by a scenario register call (keyed by
    /// fieldId; value is the *prior* provider, may be empty if the field had none).
    std::unordered_map<std::string, sol::protected_function> scenarioPriorFieldProviders_;
    /// Fields whose prior provider was *empty* — distinguishes "restore to nothing" from
    /// "field absent in scenario map → leave alone".
    std::unordered_set<std::string> scenarioPriorEmptyFields_;
    std::vector<smatchet::lua::McpToolDefinition> luaMcpTools_;
    mutable std::mutex luaMcpToolsMutex_;
    /// Guards `luaTicketActions_` and `luaGlobalActions_` (mutated from the worker thread on
    /// setup-script replay, read every frame on the UI thread).
    mutable std::mutex luaActionsMutex_;
    std::vector<std::pair<std::string, std::string>> luaTicketActions_;
    std::vector<std::pair<std::string, std::string>> luaGlobalActions_;
    mutable std::mutex fieldIconMapsMutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fieldIconMapsByFieldId_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fieldIconMapsByDisplayName_;

    // ---- `ai.prompt` Lua-glue rate limiter (security audit H5 / E6) ----
    // The instruction-count `lua_sethook` does NOT cover the outbound LLM HTTP
    // call `ai.prompt` kicks off, so paste-and-run Lua could loop it to burn
    // the API quota or stream ticket data off-host. This per-instance gate
    // rejects re-entrant or <5 s-spaced calls (decision in
    // AiLuaPromptRateLimit.h) and fires a one-time-per-session consent toast
    // naming the provider host. UI-thread-driven (`ai.*` is UI-thread only —
    // see the Phase E note in AppController_LuaBindings.cpp) but mutex-guarded
    // anyway so a stray worker-thread call can't tear the state.
    mutable std::mutex aiPromptGateMutex_;
    std::chrono::steady_clock::time_point aiPromptLastCallAt_{};
    bool aiPromptEverCalled_ = false;
    bool aiPromptInFlight_ = false;
    bool aiPromptConsentShown_ = false;

    /// Gate the next `ai.prompt` turn. Returns `VoidOk()` (and marks the turn
    /// in-flight) when the call is allowed; returns `VoidResult::Err(reason)`
    /// when rejected (re-entrant or spaced too close). Fires the one-time
    /// consent toast on the first allowed call this session. Must be paired
    /// with `EndLuaAiPromptTurn()` once the submit has been issued.
    VoidResult TryBeginLuaAiPromptTurn();
    /// Clear the in-flight flag once the `ai.prompt` submit has been issued
    /// (Submit() hands off to the worker thread, so the C++ glue's synchronous
    /// work is done — the 5 s spacing rule then guards the next call).
    void EndLuaAiPromptTurn();

    // ---- ILuaBindingHost interface (sol-typed glue resolves Impl* through __smatchet_app /
    // __smatchet_app_ui). Sol-typed bodies live here; sol-free interface methods forward to
    // the AppController implementation via app_. (19c) ----
    void LuaLogInfoBind(const std::string& msg) override;
    std::tuple<sol::object, std::string> LuaGetTicketBind(sol::state_view sv, const std::string& issueId) override;
    std::vector<CachedTicket> LuaGetActiveTicketsBind() override;
    std::tuple<sol::object, std::string> LuaDecodeJsonBind(sol::state_view sv, const std::string& s) override;
    const TrackerField* FindFieldById(const std::string& fieldId) const override;
    VoidResult SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                               const std::vector<std::string>& rawValues) override;
    std::tuple<sol::object, std::string> LuaCreateIssueBind(sol::state_view sv, sol::table spec) override;
    void LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) override;
    std::vector<smatchet::lua::McpToolDefinition> GetLuaMcpTools() const override;
    smatchet::cmd::CommandRegistry& LuaCommands() override;
    IAppScenarioHost* ScenarioHostForCommandContext() override;
    IAppThreading* ThreadingForCommandContext() override;

    /// Parse a Lua `mcp.register_tool` definition table into the name / description /
    /// parametersSchema fields of `out` (does NOT set `callback`).
    static void ParseMcpToolDef(const sol::table& toolDef, smatchet::lua::McpToolDefinition& out);

    // ---- UI binds (resolved via __smatchet_app_ui = Impl*). sol-typed; moved off AppController.h. ----
    void LuaRegisterFieldDisplayCachedBind(const std::string& fieldId, sol::function fn);
    void LuaUnregisterFieldDisplayCachedBind(const std::string& fieldId);
    void LuaRegisterFieldDisplayCachedByNameBind(const std::string& displayName, sol::function fn);
    void LuaUnregisterFieldDisplayCachedByNameBind(const std::string& displayName);
    void LuaRegisterFieldIconMapBind(const std::string& fieldKey, sol::table map, sol::optional<bool> byName);
    void LuaUnregisterFieldIconMapBind(const std::string& fieldKey, sol::optional<bool> byName);
    void LuaImGuiTextBind(const std::string& s);
    void LuaImGuiTextUnformattedBind(const std::string& s);
    bool LuaImGuiImageBind(const std::string& path, float w, float h);
    void LuaUiRegisterWindowBind(const std::string& name, sol::function drawFn);
    void LuaUiUnregisterWindowBind(const std::string& name);
    void LuaUiRegisterTicketActionBind(const std::string& name, const std::string& callbackFuncName);
    void LuaUiRegisterGlobalActionBind(const std::string& name, const std::string& callbackFuncName);
    void LuaUiInvalidateFieldCacheBind(sol::optional<std::string> ticketId, sol::optional<std::string> fieldId);

    /// TryRenderCachedLuaField helper — resolve the cell provider by field id, else by name.
    sol::protected_function* ResolveLuaFieldProvider(const std::string& fieldId, const TrackerField* fieldMeta);

    // ---- Lua state lifecycle (sol-typed; moved off AppController.h). ----
    void InitLuaCore(sol::state& state);
    void InitLuaUi(sol::state& state);
    void PrepareFreshLuaState(sol::state& state);
    void ReplayActiveSetupScripts(sol::state& state, sol::environment& sandbox);

    // ---- Automation worker (sol-typed; moved off AppController.h). ----
    void AutomationWorkerLoop();
    void RunAutomationJob(sol::state& state, sol::environment& env, const AppController::AutomationJob& job);
    using AutomationErrorSink = std::function<void(const char*, const std::string&)>;
    void RunAutomationAutoScript(sol::state& state, const AppController::AutomationJob& job,
                                 const AutomationErrorSink& logErr);
    void RunAutomationFlatScript(sol::state& state, const AppController::AutomationJob& job,
                                 const AutomationErrorSink& logErr);
    void RunAutomationActionCall(sol::environment& env, const AppController::AutomationJob& job, bool passTargetId,
                                 const AutomationErrorSink& logErr);
#endif

    /// Owns the Lua sandbox + automation worker + Lua bindings. Constructed eagerly in
    /// `Initialize` to keep the `Add*LogSink` calls from `OnEarlyInit` working.
    /// Destruction-order note (review #1016): declared AFTER the `lua`/provider block, so it
    /// destructs BEFORE `lua` — benign today because LuaAutomationHost holds only a log-sink
    /// callback list (no `sol` handles, no `Impl::lua` reference). If item-14 later migrates
    /// real sol-referencing state into LuaAutomationHost, move this back ABOVE `lua`.
    std::unique_ptr<LuaAutomationHost> luaHost_;

#if defined(SMATCHET_WITH_AI)
    // Smatchet Assistant — Phase B. unique_ptr so the (former) header could forward-declare
    // AiAssistantController. Lifetime: constructed at the end of `Initialize`; destroyed at the
    // *top* of `~AppController` (impl_->aiAssistant_.reset()) BEFORE BeginShutdown().
    std::unique_ptr<AiAssistantController> aiAssistant_;
#endif

#if defined(SMATCHET_WITH_MCP)
    static constexpr size_t kMcpActivityLogMax = 100;
    mutable std::mutex mcpActivityMutex_;
    std::deque<std::string> mcpActivityLog_;
#endif

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    // Automation queue + worker. Lifetime contract (see ~AppController + AutomationWorkerLoop):
    // `~AppController` flips automationWorkerShuttingDown_ and joins automationWorker_ BEFORE any
    // member (impl_ included) is destroyed — impl_ is still alive throughout the dtor *body*, so
    // the worker join through `impl_->automationWorker_` is safe; the join is the happens-before
    // barrier guaranteeing no live worker touches freed Impl state during destruction.
    mutable std::mutex automationJobMutex_;
    std::condition_variable automationJobCv_;
    std::deque<AppController::AutomationJob> automationJobs_;
    std::thread automationWorker_;
    std::atomic<bool> automationWorkerShuttingDown_{false};
    // Set true by AutomationWorkerLoop the instant it breaks its loop, BEFORE the
    // thread function returns. The dtor polls this to bound its shutdown wait: a
    // worker stuck inside blocking C++ glue (a synchronous tracker HTTP PUT — see
    // LuaAutomationHookPolicyPure.h) cannot be released by the count-hook and would
    // make automationWorker_.join() hang until the HTTP timeout. The dtor waits on
    // this flag up to a deadline and emits a loud WARN naming the hang if it
    // overruns, then still joins (the only no-detach-compliant terminal — abandoning
    // the thread is banned, so the warned bounded wait is the safe observable cap).
    std::atomic<bool> automationWorkerExited_{false};
    std::vector<std::string> activeSetupScripts_;
#endif
};

#endif // SMATCHET_APPCONTROLLER_IMPL_H
