#ifndef SMATCHET_APPCONTROLLER_IMPL_H
#define SMATCHET_APPCONTROLLER_IMPL_H

// Internal (src-only) definition of AppController::Impl — hardening #19, step 19b.
//
// The COLD, sol2-/subsystem-heavy member STORAGE lives here, off the public
// AppController.h, so the ~100 header includers stop pulling this state (and, after
// 19c, sol2 itself) into their compile. This header is included ONLY by the
// AppController*.cpp translation units; it is never part of the public include graph.
//
// In step 19b the public AppController.h STILL includes <sol/sol.hpp> (the sol-typed
// *method* signatures + nested types remain on the class); only the member fields move.
// Step 19c relocates those methods + nested types and finally drops the sol2 include.

#include "AppController.h"

struct AppController::Impl {
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
    std::unordered_map<std::string, AppController::LuaFieldCacheEntry> luaFieldCache_;
    std::vector<AppController::LuaWindowEntry> luaWindows_;
    std::vector<AppController::PendingLuaWindowOp> pendingLuaWindowOps_;
    /// Snapshot of user-side providers displaced by a scenario register call (keyed by
    /// fieldId; value is the *prior* provider, may be empty if the field had none).
    std::unordered_map<std::string, sol::protected_function> scenarioPriorFieldProviders_;
    /// Fields whose prior provider was *empty* — distinguishes "restore to nothing" from
    /// "field absent in scenario map → leave alone".
    std::unordered_set<std::string> scenarioPriorEmptyFields_;
    std::vector<AppController::McpToolDefinition> luaMcpTools_;
    mutable std::mutex luaMcpToolsMutex_;
    /// Guards `luaTicketActions_` and `luaGlobalActions_` (mutated from the worker thread on
    /// setup-script replay, read every frame on the UI thread).
    mutable std::mutex luaActionsMutex_;
    std::vector<std::pair<std::string, std::string>> luaTicketActions_;
    std::vector<std::pair<std::string, std::string>> luaGlobalActions_;
    mutable std::mutex fieldIconMapsMutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fieldIconMapsByFieldId_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> fieldIconMapsByDisplayName_;
#endif

    /// Owns the Lua sandbox + automation worker + Lua bindings. Constructed eagerly in
    /// `Initialize` to keep the `Add*LogSink` calls from `OnEarlyInit` working.
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
    std::vector<std::string> activeSetupScripts_;
#endif
};

#endif // SMATCHET_APPCONTROLLER_IMPL_H
