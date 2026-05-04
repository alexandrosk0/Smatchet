#pragma once

#include "IPlugin.h"
#include "McpServerStatus.h"
#include <memory>
#include <string>

class McpPlugin : public IPlugin {
  public:
    explicit McpPlugin(int port = 8080);
    ~McpPlugin() override;

    const char* Id() const override { return "mcp"; }
    void OnStart(AppController& app) override;
    void OnStop() override;

    McpServerStatus GetStatus() const;
    /** Compare bound auth token to current config (for restart-on-change). */
    bool AuthTokenMatches(const std::string& cfgToken) const;

  private:
    int port_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
