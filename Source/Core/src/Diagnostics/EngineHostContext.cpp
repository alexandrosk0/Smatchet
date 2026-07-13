// EngineHostContext — see EngineHostContext.h. Mutex-guarded snapshot string,
// intentionally tiny so the host's ~1 Hz push and the popup's on-open read never
// contend with the frame loop (nothing here is touched per frame).

#include "Diagnostics/EngineHostContext.h"

#include "Logger.h"

#include <mutex>

namespace smatchet {
namespace hostctx {

namespace {

// Inbound cap: a full engine snapshot (400 log lines + actor list) measures in
// the tens of KB; anything past this is a host-side bug, not real context.
const std::size_t kMaxSnapshotBytes = 128u * 1024u;

std::mutex& StateMutex() {
    static std::mutex m;
    return m;
}

std::string& StateJson() {
    static std::string s;
    return s;
}

std::uint64_t& StateRevision() {
    static std::uint64_t r = 0;
    return r;
}

} // namespace

void SetSnapshotJson(const std::string& json) {
    if (json.size() > kMaxSnapshotBytes) {
        LOG_WARN("EngineHostContext: rejecting oversized snapshot (%zu bytes, cap %zu)", json.size(),
                 kMaxSnapshotBytes);
        return;
    }
    std::lock_guard<std::mutex> lock(StateMutex());
    if (StateJson() == json) {
        return; // unchanged push (the host repeats at ~1 Hz) — keep the revision stable
    }
    StateJson() = json;
    ++StateRevision();
}

std::string GetSnapshotJson() {
    std::lock_guard<std::mutex> lock(StateMutex());
    return StateJson();
}

std::uint64_t Revision() {
    std::lock_guard<std::mutex> lock(StateMutex());
    return StateRevision();
}

} // namespace hostctx
} // namespace smatchet
