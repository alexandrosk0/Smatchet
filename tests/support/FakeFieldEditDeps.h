#ifndef SMATCHET_TESTS_FAKE_FIELD_EDIT_DEPS_H
#define SMATCHET_TESTS_FAKE_FIELD_EDIT_DEPS_H

// FakeFieldEditDeps — header-only in-memory implementation of `IFieldEditDeps` for the doctest rig.
// Models ONE focused pane ("main", backend key "Jira", generation 1) whose backend is a
// `FakeTrackerClient` held by shared_ptr (LatchFocusedPaneTarget() hands back a latched strong handle,
// exactly as the production adapter's atomic_load does), a settable HasCache predicate, and that pane's
// in-memory tickets. A target naming any other pane or generation reads as a pane that was closed or
// switched tracker (null snapshot, no update), so tests can pin the #2260 pane binding. Records
// UpdateTicketFor / the deferred-notify / every enqueue so tests can assert the optimistic-update,
// reachable-backend and offline-queue side effects.
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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace smatchet_tests {

class FakeFieldEditDeps : public IFieldEditDeps {
  public:
    /// The focused pane's backend. Held by shared_ptr so the latched target keeps a strong copy
    /// (matches the adapter's atomic_load(&ctx.Backend) contract). The concrete FakeTrackerClient is
    /// reachable via Fake() for mutation/editmeta scripting.
    std::shared_ptr<ITrackerBackend> BackendImpl{std::make_shared<FakeTrackerClient>()};
    std::string PaneIdImpl = "main";
    std::string BackendKeyImpl = "Jira";
    std::uint64_t GenerationImpl = 1;

    /// The focused pane's tickets — the pipeline finds the edited ticket here for the optimistic update.
    std::vector<CachedTicket> ActiveTicketsImpl;

    /// HasCache() predicate (settable). Default true — the common "cache initialized" case.
    bool HasCacheImpl = true;

    /// The last probe a latched target carries (settable). Default authenticated reachable.
    TrackerConnectivityState ConnectivityImpl = TrackerConnectivityState::AuthenticatedReachable;

    /// Recorded side effects.
    std::vector<CachedTicket> UpdatedTickets; ///< every UpdateTicketFor() ticket applied to the pane, in order
    mutable int DeferredNotifyCalls = 0;      ///< RequestDeferredLiveTrackerBackendSuccessNotify() count

    /// One EnqueueOfflineFieldEdit() call, as persisted.
    struct EnqueuedEdit {
        std::string BackendKey;
        std::string IssueKey;
        std::string FieldId;
        std::string FieldsPayloadJson;
        std::string OriginalRichValue;
        std::string OriginalValue;
        bool HasOriginalValue = false;
    };
    std::vector<EnqueuedEdit> Enqueued; ///< every successful EnqueueOfflineFieldEdit(), in order
    /// Non-empty → EnqueueOfflineFieldEdit() fails with this error and returns 0 (nothing recorded).
    std::string EnqueueFailImpl;

    /// Convenience accessor for the concrete fake backend (mutation/editmeta scripting).
    FakeTrackerClient* Fake() { return static_cast<FakeTrackerClient*>(BackendImpl.get()); }

    /// A target for the focused pane, latched now (what LatchFocusedPaneTarget() returns).
    PendingActionTarget FocusedTarget() const {
        PendingActionTarget target;
        target.Backend = BackendImpl;
        target.BackendKey = BackendKeyImpl;
        target.PaneId = PaneIdImpl;
        target.BackendGeneration = GenerationImpl;
        target.Connectivity = ConnectivityImpl;
        return target;
    }

    // --- IFieldEditDeps overrides -------------------------------------------------------------

    bool HasCache() const override { return HasCacheImpl; }

    PendingActionTarget LatchFocusedPaneTarget() const override { return FocusedTarget(); }

    std::shared_ptr<const std::vector<CachedTicket>>
    TicketsSnapshotFor(const PendingActionTarget& target) const override {
        if (!IsLivePane(target)) {
            return nullptr;
        }
        return std::make_shared<const std::vector<CachedTicket>>(ActiveTicketsImpl);
    }

    void UpdateTicketFor(const PendingActionTarget& target, const CachedTicket& ticket) override {
        if (IsLivePane(target)) {
            UpdatedTickets.push_back(ticket);
        }
    }

    void RequestDeferredLiveTrackerBackendSuccessNotify() const override { ++DeferredNotifyCalls; }

    std::int64_t EnqueueOfflineFieldEdit(const std::string& backendKey, const std::string& issueKey,
                                         const std::string& fieldId, const std::string& fieldsPayloadJson,
                                         const std::string& originalRichValue, const std::string& originalValue,
                                         bool hasOriginalValue, std::string& outError) override {
        outError.clear();
        if (!EnqueueFailImpl.empty()) {
            outError = EnqueueFailImpl;
            return 0;
        }
        EnqueuedEdit e;
        e.BackendKey = backendKey;
        e.IssueKey = issueKey;
        e.FieldId = fieldId;
        e.FieldsPayloadJson = fieldsPayloadJson;
        e.OriginalRichValue = originalRichValue;
        e.OriginalValue = originalValue;
        e.HasOriginalValue = hasOriginalValue;
        Enqueued.push_back(e);
        return static_cast<std::int64_t>(Enqueued.size());
    }

  private:
    bool IsLivePane(const PendingActionTarget& target) const {
        return target.PaneId == PaneIdImpl && target.BackendGeneration == GenerationImpl;
    }
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_FIELD_EDIT_DEPS_H
