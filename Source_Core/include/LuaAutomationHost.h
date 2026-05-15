#pragma once

// LuaAutomationHost — will own the Lua sandbox (sol2 state, all Lua bindings, automation
// job queue + worker thread, field-display/icon-map registries) extracted from
// `AppController_LuaBindings.cpp` per BACKLOG_CODE_REVIEW.md §1.7 / §7 item 14 and §5.2.
//
// Phase 1A (this PR): introduces the class skeleton, owned by AppController as a `unique_ptr`
// member, friend-accesses AppController-private state during the transition, and migrates
// the two simplest log-sink methods + the `AutomationLogSinks` state member.
//
// Future phases migrate the rest cluster-by-cluster:
//   1B — `InitLua` / `InitLuaCore` (sol2 state setup + sandbox).
//   1C — Lua bindings (`LuaLog*Bind`, `LuaGetTicket*Bind`, `LuaRegister*Bind`, etc. — ~18
//        methods totaling ~1100 LOC).
//   1D — Automation worker thread (`AutomationWorkerLoop`, `RunAutomationJob`,
//        `automationJobMutex_`, `automationJobs_`, `automationWorker_`,
//        `automationWorkerShuttingDown_`).
//   2  — Replace `friend class LuaAutomationHost;` with a `TrackerActions` interface so the
//        host stops needing AppController-private access (§1.7 design proposal #4).

#include <functional>
#include <string>
#include <vector>

class AppController;

/// Lifetime contract mirrors `OfflineQueueService` / `TicketSyncService`: AppController owns
/// the host via `std::unique_ptr` and outlives it. The constructor stores `AppController&`
/// for friend-access to the still-shared state. Constructed lazily on first need (currently
/// the log-sink methods, eventually `InitLua`).
class LuaAutomationHost {
  public:
    explicit LuaAutomationHost(AppController& app);

    // --- Phase 1A: log sinks --------------------------------------------------------------
    /// Register a sink that receives every line written to the Lua log ring. Called from
    /// plugins in `OnEarlyInit` only (before `InitLua` completes).
    void AddAutomationLogSink(std::function<void(const std::string&)> sink);

    /// Drop all sinks. Called before destroying plugins to avoid dangling `[this]` captures.
    void ClearAutomationLogSinks();

    /// Snapshot copy of the sink list for the Lua log writer to iterate without holding the
    /// host alive (the writer is called from background threads). Empty when no sinks are
    /// registered.
    std::vector<std::function<void(const std::string&)>> SnapshotLogSinks() const;

  private:
    AppController& app_;

    /// Plugin-registered sinks for the Lua log stream. UI-thread-only writes via `Add*` /
    /// `Clear*`; the Lua worker reads via `SnapshotLogSinks` so it iterates a stable copy.
    std::vector<std::function<void(const std::string&)>> automationLogSinks_;
};
