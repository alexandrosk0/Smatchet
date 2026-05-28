#pragma once

#include "IPlugin.h"
#include "SmatchetDefaults.h"
#include "McpServerStatus.h"
#include <memory>
#include <string>

class McpPlugin : public IPlugin {
  public:
    explicit McpPlugin(int port = SmatchetDefaults::Mcp::kDefaultPort);
    ~McpPlugin() override;

    const char* Id() const override { return "mcp"; }
    void OnStart(AppController& app) override;
    // cppcheck-suppress virtualCallInConstructor // invoked from ~McpPlugin for non-dynamic teardown
    void OnStop() override;

    bool NeedsRestart(const TrackerConfig& cfg) const override;
    bool TryGetMcpStatusSnapshot(McpServerStatus& out) const override {
        out = GetStatus();
        return true;
    }

    McpServerStatus GetStatus() const;
    /** Compare bound auth token to current config (for restart-on-change). */
    bool AuthTokenMatches(const std::string& cfgToken) const;
    /** Compare run_lua exposure toggle to current config (for restart-on-change). */
    bool LuaExecutionEnabledMatches(bool enabled) const;

  private:
    int port_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};






