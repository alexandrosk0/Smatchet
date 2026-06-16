#ifndef SMATCHET_TYPES_CONNECTIVITY_TYPES_H
#define SMATCHET_TYPES_CONNECTIVITY_TYPES_H

// Types/ConnectivityTypes.h — TrackerConnectivityState relocated out of AppController.h
// (fan-in Phase 3, docs/plans/appcontroller-fan-in.md). Was nested as
// AppController::TrackerConnectivityState; moved to the global namespace here (rank-0 leaf)
// and re-exported via `using TrackerConnectivityState = ::TrackerConnectivityState;` inside
// AppController, so the ~115 includers that name AppController::TrackerConnectivityState (incl.
// the per-frame inline getter GetLastTrackerConnectivityState()) see an identical type with
// zero source change. NOTE: deliberately distinct from ITicketSyncDeps.h's mirror enum
// `ConnectivityState` — that Sync-layer decoupling is intentional (ADR-blessed); do not merge them.

enum class TrackerConnectivityState {
    Unknown,
    AuthenticatedReachable,
    ReachableAuthOrConfigError,
    TransportDown,
    ServiceUnavailable,
};

#endif // SMATCHET_TYPES_CONNECTIVITY_TYPES_H
