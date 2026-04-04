#pragma once

#include "IPlugin.h"
#include <memory>
#include <vector>

class AppController;

class PluginHost {
public:
    void Register(std::unique_ptr<IPlugin> plugin);

    void OnEarlyInit(AppController& app);
    void OnStart(AppController& app);
    void OnDraw(AppController& app);
    void OnStop();

private:
    std::vector<std::unique_ptr<IPlugin>> plugins_;
};
