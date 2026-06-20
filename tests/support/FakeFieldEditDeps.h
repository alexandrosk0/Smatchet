#ifndef SMATCHET_TESTS_FAKE_FIELD_EDIT_DEPS_H
#define SMATCHET_TESTS_FAKE_FIELD_EDIT_DEPS_H

// FakeFieldEditDeps — header-only in-memory implementation of `IFieldEditDeps` for the doctest rig.
// Backs the backend with a `FakeTrackerClient` held by shared_ptr (so BackendShared() hands back a
// latched strong handle, exactly as the production adapter's atomic_load does), exposes a settable
// HasCache predicate + an in-memory active-tickets snapshot, and records UpdateTicket /
// RefreshLocalData / the deferred-notify so tests can assert the optimistic-update + reachable-
// backend side effects.
//
// This fixture is the test-side counterpart of `GridContextDepsAdapter`. Any new method added to
// `IFieldEditDeps` MUST be implemented here too — the override list mirrors the production adapter.
//
// SQLite/ImGui/cpr-free by construction (ADR-0020 sync-cache purity): HasCache() is a PREDICATE
// (settable bool), never a raw Cache*, so no LocalCacheManager is pulled. It uses only the narrow
// interface header + FakeTrackerClient + CachedTicketTypes (plain structs).

#include "CachedTicketTypes.h"
#include "FakeTrackerClient.h"
#include "IFieldEditDeps.h"
#include "ITrackerBackend.h"

#include <memory>
#include <vector>

namespace smatchet_tests {

class FakeFieldEditDeps : public IFieldEditDeps {
  public:
    /// Latched backend handle. Held by shared_ptr so BackendShared() returns a strong copy
    /// (matches the adapter's atomic_load(&ctx_.Backend) contract). The concrete FakeTrackerClient
    /// is reachable via Fake() for mutation/editmeta scripting.
    std::shared_ptr<ITrackerBackend> BackendImpl{std::make_shared<FakeTrackerClient>()};

    /// Active-tickets snapshot returned by GetActiveTicketsSnapshot() — the pipeline scans it to
    /// find the edited ticket for the optimistic local update + audit before/after values.
    std::vector<CachedTicket> ActiveTicketsImpl;

    /// HasCache() predicate (settable). Default true — the common "cache initialized" case.
    bool HasCacheImpl = true;

    /// Recorded side effects.
    std::vector<CachedTicket> UpdatedTickets; ///< every UpdateTicket() arg, in order
    int RefreshLocalDataCalls = 0;            ///< RefreshLocalData() call count
    mutable int DeferredNotifyCalls = 0;      ///< RequestDeferredLiveTrackerBackendSuccessNotify() count

    /// Convenience accessor for the concrete fake backend (mutation/editmeta scripting).
    FakeTrackerClient* Fake() { return static_cast<FakeTrackerClient*>(BackendImpl.get()); }

    // --- IFieldEditDeps overrides -------------------------------------------------------------

    std::shared_ptr<ITrackerBackend> BackendShared() const override { return BackendImpl; }

    bool HasCache() const override { return HasCacheImpl; }

    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const override {
        return std::make_shared<const std::vector<CachedTicket>>(ActiveTicketsImpl);
    }

    void UpdateTicket(const CachedTicket& ticket) override { UpdatedTickets.push_back(ticket); }

    void RefreshLocalData() override { ++RefreshLocalDataCalls; }

    void RequestDeferredLiveTrackerBackendSuccessNotify() const override { ++DeferredNotifyCalls; }
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_FIELD_EDIT_DEPS_H
