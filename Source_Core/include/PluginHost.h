#pragma once

#include "IPlugin.h"
#include <memory>
#include <vector>

class AppController;
struct JiraConfig;
#if defined(SMATCHET_WITH_MCP)
#include "McpServerStatus.h"
#endif

class PluginHost {
  public:
    void Register(std::unique_ptr<IPlugin> plugin);

    void OnEarlyInit(AppController& app);
    void OnStart(AppController& app);
    void OnDraw(AppController& app);
    void OnStop();

#if defined(SMATCHET_WITH_MCP)
    /** Add/remove MCP plugin when config changes (UI thread; after first OnStart). */
    void SyncMcpPluginWithConfig(AppController& app, const JiraConfig& cfg);
    McpServerStatus GetMcpServerStatus() const;
#endif

  private:
    std::vector<std::unique_ptr<IPlugin>> plugins_;
#if defined(SMATCHET_WITH_MCP)
    bool on_start_completed_ = false;
#endif
};
