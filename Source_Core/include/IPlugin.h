#pragma once

#if defined(SMATCHET_WITH_MCP)
#include "McpServerStatus.h"
#endif

class AppController;
struct TrackerConfig;

/** Single plugin contract; same vtable shape for future DLL loading. */
class IPlugin {
  public:
    virtual ~IPlugin() = default;

    virtual const char* Id() const = 0;

    /** Before AppController::Initialize (e.g. register Lua log sinks). */
    virtual void OnEarlyInit(AppController&) {}

    /** After Initialize; start threads, etc. */
    virtual void OnStart(AppController&) {}

    /** Same frame as main UI (ImGui). */
    virtual void OnDraw(AppController&) {}

    /** Clean shutdown before ImGui teardown. */
    virtual void OnStop() {}

    /** Return true when the plugin must be stopped and restarted to reflect cfg changes.
     *  Called by PluginHost::SyncMcpPluginWithConfig (and any future config-sync paths).
     *  Default: never restart (stateless / config-independent plugins). */
    virtual bool NeedsRestart(const TrackerConfig&) const { return false; }

#if defined(SMATCHET_WITH_MCP)
    /** Opaque status accessor for the MCP plugin so the host can surface it via PluginHost
     *  without a dynamic_cast<McpPlugin*>. Default returns false (plugin is not an MCP server);
     *  McpPlugin overrides it to fill `out` and return true. Same pattern as MatchesConfig
     *  added earlier — keeps the plugin host RTTI-free. */
    virtual bool TryGetMcpStatusSnapshot(McpServerStatus& /*out*/) const { return false; }
#endif
};






