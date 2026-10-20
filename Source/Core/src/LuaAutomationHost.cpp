#include "LuaAutomationHost.h"

#include <utility>

void LuaAutomationHost::AddAutomationLogSink(smatchet::StringSinkList::Sink sink) {
    automationLogSinks_.Add(std::move(sink));
}

void LuaAutomationHost::ClearAutomationLogSinks() { automationLogSinks_.ClearAndWait(); }

bool LuaAutomationHost::InvokeLogSinks(const std::string& line) const { return automationLogSinks_.InvokeAll(line); }

std::shared_ptr<const smatchet::StringSinkList::Sinks> LuaAutomationHost::SnapshotLogSinks() const {
    return automationLogSinks_.Snapshot();
}
