#ifndef SMATCHET_TESTS_FAKE_CONNECTIVITY_DEPS_H
#define SMATCHET_TESTS_FAKE_CONNECTIVITY_DEPS_H

// FakeConnectivityDeps — header-only in-memory implementation of IConnectivityDeps for the doctest
// rig. Backs the FOCUSED backend with a FakeTrackerClient held by shared_ptr (so
// FocusedBackendShared() hands back a latched strong handle, matching the production adapter's
// atomic_load), exposes plain CatalogError/Warning strings the connectivity FSM reads/clears, and
// records the replay-timer pushes + warning-clear / revision-bump calls so tests assert the FSM's
// side effects. SQLite/ImGui/cpr-free by construction (ADR-0020 sync-cache purity): it pulls only
// the narrow interface header + FakeTrackerClient.
// This fixture is the test-side counterpart of GridContextDepsAdapter's IConnectivityDeps surface.
// Any new method added to IConnectivityDeps MUST be implemented here too — the override list mirrors
// the production adapter.

#include "FakeTrackerClient.h"
#include "IConnectivityDeps.h"
#include "ITrackerBackend.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace smatchet_tests {

class FakeConnectivityDeps : public IConnectivityDeps {
  public:
    /// Focused-context backend handle. Held by shared_ptr so FocusedBackendShared() returns a strong
    /// copy the probe worker can capture (matches the adapter's atomic_load(&focusedContext().Backend)
    /// contract). Set to nullptr to model "no backend" so the FSM bails before launching a probe.
    std::shared_ptr<ITrackerBackend> BackendImpl{std::make_shared<FakeTrackerClient>()};

    bool ShuttingDownImpl = false;

    /// Live-focus catalog error/warning the FSM reads (banner formatter + degraded-interval) and
    /// clears (recovery / live-success path). Plain members tests set/inspect directly.
    std::string CatalogErrorImpl;
    bool CatalogErrorTransientImpl = false;
    std::string CatalogWarningImpl;

    /// Recorded side effects.
    int ClearWarningCalls = 0;
    int BumpRevisionCalls = 0;
    std::vector<std::chrono::steady_clock::time_point> PushReplayCalls;
    std::vector<std::chrono::steady_clock::time_point> RestartReplayCalls;

    /// Convenience accessor for the concrete fake backend (reachability scripting).
    FakeTrackerClient* Fake() { return static_cast<FakeTrackerClient*>(BackendImpl.get()); }

    // --- IConnectivityDeps overrides ----------------------------------------------------------

    std::shared_ptr<ITrackerBackend> FocusedBackendShared() const override { return BackendImpl; }

    bool IsShuttingDown() const override { return ShuttingDownImpl; }

    const std::string& FocusedFieldCatalogError() const override { return CatalogErrorImpl; }
    bool FocusedFieldCatalogErrorTransient() const override { return CatalogErrorTransientImpl; }
    const std::string& FocusedFieldCatalogWarning() const override { return CatalogWarningImpl; }

    void ClearFocusedFieldCatalogWarning() override {
        ++ClearWarningCalls;
        CatalogWarningImpl.clear();
    }
    void BumpFocusedFieldCatalogRevision() override { ++BumpRevisionCalls; }

    void PushReplayTimers(std::chrono::steady_clock::time_point pushTo) override { PushReplayCalls.push_back(pushTo); }
    void RestartReplayTimers(std::chrono::steady_clock::time_point now) override { RestartReplayCalls.push_back(now); }
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_CONNECTIVITY_DEPS_H
