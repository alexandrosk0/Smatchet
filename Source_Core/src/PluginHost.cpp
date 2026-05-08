#include "PluginHost.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"

#if defined(SMATCHET_WITH_MCP)
#include "McpPlugin.h"
#include <cstring>
#include <string>
#endif

#include <exception>

void PluginHost::Register(std::unique_ptr<IPlugin> plugin) {
    if (plugin) {
        plugins_.push_back(std::move(plugin));
    }
}

void PluginHost::OnEarlyInit(AppController& app) {
    for (size_t i = 0; i < plugins_.size(); ++i) {
        try {
            auto& p = plugins_[i];
            p->OnEarlyInit(app);
        } catch (const std::exception& ex) {
            LOG_ERROR("PluginHost::OnEarlyInit plugin[%zu] exception: %s", i, ex.what());
        } catch (...) {
            LOG_ERROR("PluginHost::OnEarlyInit plugin[%zu] unknown exception", i);
        }
    }
}

void PluginHost::OnStart(AppController& app) {
    for (size_t i = 0; i < plugins_.size(); ++i) {
        try {
            auto& p = plugins_[i];
            p->OnStart(app);
        } catch (const std::exception& ex) {
            LOG_ERROR("PluginHost::OnStart plugin[%zu] exception: %s", i, ex.what());
        } catch (...) {
            LOG_ERROR("PluginHost::OnStart plugin[%zu] unknown exception", i);
        }
    }
#if defined(SMATCHET_WITH_MCP)
    on_start_completed_ = true;
#endif
}

void PluginHost::OnDraw(AppController& app) {
    for (size_t i = 0; i < plugins_.size(); ++i) {
        try {
            auto& p = plugins_[i];
            p->OnDraw(app);
        } catch (const std::exception& ex) {
            LOG_ERROR("PluginHost::OnDraw plugin[%zu] exception: %s", i, ex.what());
        } catch (...) {
            LOG_ERROR("PluginHost::OnDraw plugin[%zu] unknown exception", i);
        }
    }
}

void PluginHost::OnStop() {
    for (size_t i = 0; i < plugins_.size(); ++i) {
        try {
            auto& p = plugins_[i];
            p->OnStop();
        } catch (const std::exception& ex) {
            LOG_ERROR("PluginHost::OnStop plugin[%zu] exception: %s", i, ex.what());
        } catch (...) {
            LOG_ERROR("PluginHost::OnStop plugin[%zu] unknown exception", i);
        }
    }
#if defined(SMATCHET_WITH_MCP)
    on_start_completed_ = false;
#endif
}

#if defined(SMATCHET_WITH_MCP)
McpServerStatus PluginHost::GetMcpServerStatus() const {
    for (size_t i = 0; i < plugins_.size(); ++i) {
        if (!plugins_[i] || std::strcmp(plugins_[i]->Id(), "mcp") != 0) {
            continue;
        }
        if (const auto* mcp = dynamic_cast<const McpPlugin*>(plugins_[i].get())) {
            return mcp->GetStatus();
        }
    }
    return McpServerStatus();
}

void PluginHost::SyncMcpPluginWithConfig(AppController& app, const TrackerConfig& cfg) {
    size_t mcpIndex = plugins_.size();
    for (size_t i = 0; i < plugins_.size(); ++i) {
        if (plugins_[i] && std::strcmp(plugins_[i]->Id(), "mcp") == 0) {
            mcpIndex = i;
            break;
        }
    }
    bool hasMcp = mcpIndex < plugins_.size();
    const int port = (cfg.McpPort >= 1 && cfg.McpPort <= 65535) ? cfg.McpPort : SmatchetDefaults::Mcp::kDefaultPort;
    const std::string expectedBind =
        cfg.McpAllowRemote ? SmatchetDefaults::Mcp::kBindAny : SmatchetDefaults::Mcp::kBindLocalhost;

    if (cfg.McpEnabled) {
        bool needFresh = !hasMcp;
        if (hasMcp) {
            if (auto* mcp = dynamic_cast<McpPlugin*>(plugins_[mcpIndex].get())) {
                const McpServerStatus st = mcp->GetStatus();
                if (st.ListenPort != port || st.BindHost != expectedBind || !mcp->AuthTokenMatches(cfg.McpAuthToken) ||
                    !mcp->LuaExecutionEnabledMatches(cfg.McpAllowLuaExecution)) {
                    needFresh = true;
                }
            } else {
                needFresh = true;
            }
        }

        if (needFresh && hasMcp) {
            try {
                plugins_[mcpIndex]->OnStop();
            } catch (const std::exception& ex) {
                LOG_ERROR("PluginHost::SyncMcpPluginWithConfig OnStop exception: %s", ex.what());
            } catch (...) {
                LOG_ERROR("PluginHost::SyncMcpPluginWithConfig OnStop unknown exception");
            }
            plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(mcpIndex));
            hasMcp = false;
            app.AppendMcpActivity("MCP: plugin stopped (restart for config change).");
        }

        if (needFresh) {
            plugins_.push_back(std::make_unique<McpPlugin>(port));
            if (on_start_completed_) {
                auto& p = plugins_.back();
                try {
                    p->OnStart(app);
                } catch (const std::exception& ex) {
                    LOG_ERROR("PluginHost::SyncMcpPluginWithConfig OnStart exception: %s", ex.what());
                } catch (...) {
                    LOG_ERROR("PluginHost::SyncMcpPluginWithConfig OnStart unknown exception");
                }
            }
            LOG_INFO("PluginHost: MCP server started (port %d)", port);
            app.AppendMcpActivity(std::string("MCP: plugin started (port ") + std::to_string(port) + ").");
        }
        return;
    }

    if (hasMcp) {
        try {
            plugins_[mcpIndex]->OnStop();
        } catch (const std::exception& ex) {
            LOG_ERROR("PluginHost::SyncMcpPluginWithConfig OnStop exception: %s", ex.what());
        } catch (...) {
            LOG_ERROR("PluginHost::SyncMcpPluginWithConfig OnStop unknown exception");
        }
        plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(mcpIndex));
        LOG_INFO("PluginHost: MCP server stopped");
        app.AppendMcpActivity("MCP: plugin stopped (disabled in config).");
    }
}
#endif






