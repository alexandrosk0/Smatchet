#include "PluginHost.h"
#include "AppController.h"
#include "Logger.h"

#include <exception>

void PluginHost::Register(std::unique_ptr<IPlugin> plugin) {
    if (plugin) {
        plugins_.push_back(std::move(plugin));
    }
}

void PluginHost::OnEarlyInit(AppController& app) {
    for (size_t i = 0; i < plugins_.size(); ++i) {
        auto& p = plugins_[i];
        try {
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
        auto& p = plugins_[i];
        try {
            p->OnStart(app);
        } catch (const std::exception& ex) {
            LOG_ERROR("PluginHost::OnStart plugin[%zu] exception: %s", i, ex.what());
        } catch (...) {
            LOG_ERROR("PluginHost::OnStart plugin[%zu] unknown exception", i);
        }
    }
}

void PluginHost::OnDraw(AppController& app) {
    for (size_t i = 0; i < plugins_.size(); ++i) {
        auto& p = plugins_[i];
        try {
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
        auto& p = plugins_[i];
        try {
            p->OnStop();
        } catch (const std::exception& ex) {
            LOG_ERROR("PluginHost::OnStop plugin[%zu] exception: %s", i, ex.what());
        } catch (...) {
            LOG_ERROR("PluginHost::OnStop plugin[%zu] unknown exception", i);
        }
    }
}
