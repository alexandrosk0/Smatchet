#include "LuaAutomationHost.h"

#include <utility>

void LuaAutomationHost::AddAutomationLogSink(smatchet::StringSinkList::Sink sink) {
    automationLogSinks_.Add(std::move(sink));
}

void LuaAutomationHost::ClearAutomationLogSinks() { automationLogSinks_.Clear(); }

std::shared_ptr<const smatchet::StringSinkList::Sinks> LuaAutomationHost::SnapshotLogSinks() const {
    return automationLogSinks_.Snapshot();
}
