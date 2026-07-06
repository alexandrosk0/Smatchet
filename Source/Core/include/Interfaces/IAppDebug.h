#ifndef SMATCHET_INTERFACES_IAPP_DEBUG_H
#define SMATCHET_INTERFACES_IAPP_DEBUG_H

// Narrow facet for the debug.* diagnostics surface (Lua-eval / automation-sink capture,
// MCP activity introspection) — AppController fan-in Phase 5
// (docs/plans/appcontroller-fan-in-phase5-facets.md). AppController implements it; the
// debug.* command TU depends on this instead of the full AppController.h.
//
// The debug.* dock/window commands additionally marshal to the UI thread via
// RunOnUiThreadAsCommandResult(IMainThreadPoster&, ...); those take a separate
// IMainThreadPoster& (see BuiltinCommands_Internal.h), so this facet stays purely the
// domain surface.
//
// Rank-0 leaf (Interfaces/): only std / <chrono> types cross the boundary.
// ConsumeScriptingWindowRequest is called once per frame from the LuaConsole plugin's
// OnDraw, but through a concrete AppController& and it is already an out-of-line atomic
// exchange (never inlined), so virtualizing it costs at most one vtable lookup per frame
// — not the inlined-hot-getter case the Pillar-1 exclusion protects.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

class IAppDebug {
  public:
    virtual ~IAppDebug() = default;

    // Automation log/error sinks + scripting-window request (present with or without the
    // Lua build; no-op / empty when SMATCHET_WITH_LUA_AUTOMATION is off).
    virtual void AddAutomationLogSink(std::function<void(const std::string&)> sink) = 0;
    virtual void AddAutomationErrorSink(std::function<void(const std::string&)> sink) = 0;
    virtual bool ConsumeScriptingWindowRequest() = 0;
    virtual bool ExecuteLuaConsoleSnippet(const std::string& code, std::string& outError,
                                          std::string& outResultSummary) = 0;

#if defined(SMATCHET_WITH_MCP)
    // MCP activity introspection — guarded identically to the AppController declarations so
    // the facet's abstract surface matches the concrete class in every build configuration.
    virtual std::vector<std::string> CopyMcpActivityLog() const = 0;
    virtual bool TryGetMcpLastClientHttpActivity(std::chrono::steady_clock::time_point* out) const = 0;
#endif
};

#endif // SMATCHET_INTERFACES_IAPP_DEBUG_H
