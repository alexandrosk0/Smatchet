#pragma once

// LuaAutomationHost — fan-out hub for plugin-registered Lua-log sinks.
// Scope: this class owns the plugin-registered log-sink list
// for the Lua automation surface and nothing else. Ownership of sol2 state,
// every Lua binding, and the automation worker thread sits on AppController
// (expressed through the `ILuaBindingHost` interface that AppController
// implements; see `Source/Core/include/ILuaBindingHost.h`). The TU
// lift fixed the "tests cannot link binding code without ImGui" pain point
// without migrating ownership across the AppController boundary, so this
// class stays a thin coordinator rather than the heavyweight owner the
// original Phase 1B/1C/1D plan envisioned.
// Lifetime: AppController owns the host via `std::unique_ptr` and outlives
// it. The host is constructed lazily during `AppController::Initialize` so
// that plugin `OnEarlyInit` calls to `AddAutomationLogSink` (which may run
// before `Initialize`) can be buffered into `pendingLogSinks_` on
// AppController and drained on first construction.

#include <functional>
#include <string>
#include <vector>

class LuaAutomationHost {
  public:
    LuaAutomationHost() = default;

    /// Register a sink that receives every line written to the Lua log ring.
    /// Called from plugins in `OnEarlyInit` only (before `InitLua` completes).
    void AddAutomationLogSink(std::function<void(const std::string&)> sink);

    /// Drop all sinks. Called before destroying plugins to avoid dangling `[this]` captures.
    void ClearAutomationLogSinks();

    /// Snapshot copy of the sink list for the Lua log writer to iterate without holding the
    /// host alive (the writer is called from background threads). Empty when no sinks are
    /// registered.
    std::vector<std::function<void(const std::string&)>> SnapshotLogSinks() const;

  private:
    /// Plugin-registered sinks for the Lua log stream. UI-thread-only writes via `Add*` /
    /// `Clear*`; the Lua worker reads via `SnapshotLogSinks` so it iterates a stable copy.
    std::vector<std::function<void(const std::string&)>> automationLogSinks_;
};
