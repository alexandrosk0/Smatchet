#include "UiThreadAffinity.h"

#include "Logger.h"

#include <atomic>
#include <thread>

namespace {
// Publish-once: written exactly once by SetUiThread() (before any worker spawns), then never
// mutated. Guarded by g_registered with release/acquire so worker-thread reads are race-free.
std::thread::id g_uiThreadId;
std::atomic<bool> g_registered{false};
} // namespace

namespace UiThreadAffinity {

void SetUiThread() {
    g_uiThreadId = std::this_thread::get_id();
    g_registered.store(true, std::memory_order_release);
}

bool IsOnUiThread() {
    if (!g_registered.load(std::memory_order_acquire)) {
        // Not registered yet (process startup) — fail open so early/once config reads on the
        // soon-to-be UI thread aren't mis-flagged before SetUiThread() runs.
        return false;
    }
    return std::this_thread::get_id() == g_uiThreadId;
}

bool WarnIfOnUiThread(const char* context) {
    if (!IsOnUiThread()) {
        return false;
    }
    LOG_ERROR("Pillar-2 violation: blocking I/O '%s' invoked on the UI thread — must run "
              "off-thread (LaunchBackgroundTask / deferred save). See close-gate-gaps Slice 1.",
              context != nullptr ? context : "(unnamed)");
    return true;
}

} // namespace UiThreadAffinity
