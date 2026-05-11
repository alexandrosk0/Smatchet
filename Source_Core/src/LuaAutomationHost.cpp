#include "LuaAutomationHost.h"

#include "AppController.h"

#include <utility>

LuaAutomationHost::LuaAutomationHost(AppController& app) : app_(app) {}

void LuaAutomationHost::AddAutomationLogSink(std::function<void(const std::string&)> sink) {
    if (sink) {
        automationLogSinks_.push_back(std::move(sink));
    }
}

void LuaAutomationHost::ClearAutomationLogSinks() {
    automationLogSinks_.clear();
}

std::vector<std::function<void(const std::string&)>> LuaAutomationHost::SnapshotLogSinks() const {
    return automationLogSinks_;
}
