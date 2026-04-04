#include "PluginHost.h"
#include "AppController.h"

void PluginHost::Register(std::unique_ptr<IPlugin> plugin) {
    if (plugin) {
        plugins_.push_back(std::move(plugin));
    }
}

void PluginHost::OnEarlyInit(AppController& app) {
    for (auto& p : plugins_) {
        p->OnEarlyInit(app);
    }
}

void PluginHost::OnStart(AppController& app) {
    for (auto& p : plugins_) {
        p->OnStart(app);
    }
}

void PluginHost::OnDraw(AppController& app) {
    for (auto& p : plugins_) {
        p->OnDraw(app);
    }
}

void PluginHost::OnStop() {
    for (auto& p : plugins_) {
        p->OnStop();
    }
}
