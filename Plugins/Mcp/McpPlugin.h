#pragma once

#include "IPlugin.h"
#include <memory>

class McpPlugin : public IPlugin {
  public:
    explicit McpPlugin(int port = 8080);
    ~McpPlugin() override;

    const char* Id() const override { return "mcp"; }
    void OnStart(AppController& app) override;
    void OnStop() override;

  private:
    int port_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
