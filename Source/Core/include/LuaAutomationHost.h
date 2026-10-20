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

#include "StringSinkList.h"

#include <memory>
#include <string>

class LuaAutomationHost {
  public:
    LuaAutomationHost() = default;

    /// Register a sink that receives every line written to the Lua log ring.
    /// Called from plugins in `OnEarlyInit` (before `InitLua` completes) and from commands.
    void AddAutomationLogSink(smatchet::StringSinkList::Sink sink);

    /// Drop all sinks and wait for any still running on another thread. Called before destroying
    /// plugins, so a `[this]` capture is never called after its plugin is gone.
    void ClearAutomationLogSinks();

    /// Send `line` to every sink, on whatever thread the Lua log writer runs. False when there is no
    /// sink (the caller then writes the line to the app log instead).
    bool InvokeLogSinks(const std::string& line) const;

    /// The current sinks (never null), for inspection. Callers that run them use InvokeLogSinks, which
    /// ClearAutomationLogSinks waits for.
    std::shared_ptr<const smatchet::StringSinkList::Sinks> SnapshotLogSinks() const;

  private:
    /// Plugin-registered sinks for the Lua log stream. Written on the UI thread, read on the
    /// automation worker and the UI thread; StringSinkList makes that safe.
    smatchet::StringSinkList automationLogSinks_;
};
