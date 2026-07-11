#pragma once

// ConnectivityMonitorService — owns the tracker connectivity-probe finite-state machine extracted
// verbatim from AppController per the god-object decomposition plan (Phase 3), mirroring the
// OfflineQueueService / TicketSyncService / EditMetaCacheService / FieldEditPipelineService
// template: the service holds an IConnectivityDeps& (backed by GridContextDepsAdapter) and reaches
// AppController-side state only through that interface. AppController's public connectivity surface
// keeps the same shape but its bodies are thin delegators forwarding into this service.
// GLOBAL singleton: today connectivity is a single global FSM (one nextProbeAt_, one lastState_,
// one ticket-sync warning) written by N per-pane adapters that all share one AppController. The
// service is one AppController-owned instance constructed eagerly in Initialize; every per-pane
// adapter (the default depsAdapter_ plus the paneAdapters_ map) forwards its connectivity writes
// into this one instance (N writers, one service) via the three re-pointed ITicketSyncDeps setters.
// Lifetime contract: AppController owns the service via std::unique_ptr and outlives it. The single
// std::future the FSM launches (the reachability probe) is drained by DrainProbeFuture, which
// ~AppController invokes after shuttingDown_.store(true) and before JoinBackgroundTasks while the
// deps adapter is still live.
// THREADING: the FSM state is UI-thread-only by contract (driven from the per-frame
// TickTrackerConnectivityMonitor). No mutex — the std::future is the only cross-thread handle and
// it stays owned by the single UI-thread FSM. The two std::atomic<bool> latches are read by UI
// consumers on any frame. lastTicketSyncWarning_ has a second writer (TicketSyncService, via the
// adapter's re-pointed SetLastTrackerTicketSyncWarning) but both writers run on the UI tick, so the
// string keeps a single logical owner. The catalog-warning reads/clears go through the deps adapter
// UNLOCKED, preserving the current UI-thread-only single-kick-time-latch discipline.

#include <atomic>
#include <chrono>
#include <future>
#include <string>

#include "ITrackerConnectivity.h"    // TrackerReachabilityProbeResult / TrackerReachabilityProbeKind
#include "Sync/SyncTypes.h"          // TrackerConnectivityBannerForUi
#include "Types/ConnectivityTypes.h" // TrackerConnectivityState

class IConnectivityDeps;
struct TrackerConfig;

class ConnectivityMonitorService {
  public:
    explicit ConnectivityMonitorService(IConnectivityDeps& deps);

    /// Rate-limited background probe; updates connectivity state and recovery latch. Drives the
    /// FSM: collects a finished probe future, or launches a new probe once the next-probe-at
    /// deadline passes. UI thread, called once per frame.
    void TickTrackerConnectivityMonitor(const TrackerConfig& cfg);

    /// Latest reachability from background probe (or after a successful live backend request).
    TrackerConnectivityState GetLastState() const { return lastState_; }

    /// Set when a live JQL refresh failed with a transport-style error; UI may show cached
    /// tickets. Returns a const reference (callers bind a reference — do not return by value).
    const std::string& LastTicketSyncWarning() const { return lastTicketSyncWarning_; }

    /// Clears the live ticket-sync warning banner (e.g. before kicking off a background fetch).
    void ClearLastTicketSyncWarning() {
        lastTicketSyncWarning_.clear();
        lastTicketSyncWarningTransient_ = false;
    }

    /// One consolidated banner for field-catalog error/warning, ticket-list cache warning, and an
    /// optional session note (e.g. Views dashboard users-fetch warning). Pure given the catalog /
    /// warning inputs the deps interface surfaces.
    TrackerConnectivityBannerForUi GetBannerForUi(const std::string* sessionCatalogNote = nullptr) const;

    /// One-shot: true when reachability improved to authenticated-reachable (including from
    /// transport-down, service-unavailable, or auth/config errors, and cold-start when a catalog
    /// offline banner is still set). Clears ticket sync + field-catalog warnings and nudges offline
    /// replay timers. UI runs catalog refetch + SyncWithCurrentView on the same frame.
    bool ConsumeTrackerConnectivityRecovery();

    /// One-shot: true after a successful live SyncWithBackend issue fetch cleared a stale offline
    /// field-catalog banner. UI sets triggerCatalogRefetch (same as connectivity recovery).
    bool ConsumeFieldCatalogRefetchAfterLiveTicketSync();

    /// One-shot: applies a deferred live-tracker success notify if one is armed. True if applied.
    bool ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny();

    /// Arm the deferred live-tracker-backend success notify (a successful live backend request
    /// counts as a reachable-backend signal). const — the latch is a mutable atomic.
    void RequestDeferredLiveTrackerBackendSuccessNotify() const;

    /// Setters re-pointed from the adapter's ITicketSyncDeps overrides — TicketSyncService writes
    /// connectivity state through these (N per-pane adapters → this one global service).
    /// `transient` travels with the message from the fetch seam that classified it (N12 slice 1)
    /// so the degraded-probe-interval check never re-classifies the composed text.
    void SetLastTicketSyncWarning(const std::string& message, bool transient) {
        lastTicketSyncWarning_ = message;
        lastTicketSyncWarningTransient_ = transient && !message.empty();
    }
    void SetLastState(TrackerConnectivityState state) { lastState_ = state; }
    void SetNextProbeAt(std::chrono::steady_clock::time_point at) { nextProbeAt_ = at; }

    /// Push replay timers forward while the transport is down (also called internally by
    /// ApplyProbeResult). Routed through the deps adapter so the null-queue guard lives there.
    void PushOfflineReplayTimersDuringTransportOutage(std::chrono::steady_clock::time_point now);

    /// Block on the in-flight probe future during shutdown so no async worker outlives the service.
    /// Called from ~AppController after shuttingDown_.store(true), before JoinBackgroundTasks.
    void DrainProbeFuture();

  private:
    /// Pure mapping from probe kind to connectivity state — no default case so a new enumerator
    /// trips a compiler warning instead of a silent fallthrough.
    static TrackerConnectivityState MapReachabilityProbeKind(TrackerReachabilityProbeKind k);

    /// True when the next probe state (or the current ticket-sync / catalog warning) indicates a
    /// transport-style outage, which selects the aggressive probe interval.
    bool IsConnectivityDegradedForProbeInterval(TrackerConnectivityState nextProbeState) const;

    /// Apply a finished probe result: update state/diagnostic, arm the recovery latch on
    /// improvement, set the connectivity-loss warning on degradation, push replay timers, and
    /// schedule the next probe.
    void ApplyTrackerConnectivityProbeResult(std::chrono::steady_clock::time_point now,
                                             const TrackerReachabilityProbeResult& r);

    /// Promote the FSM to authenticated-reachable after a successful live backend request and clear
    /// a stale offline catalog banner (arming the refetch latch when it cleared one).
    void applyLiveTrackerReachabilityAfterSuccessfulBackendRequest_();

    IConnectivityDeps& deps_;

    std::chrono::steady_clock::time_point nextProbeAt_{};
    bool probeInFlight_ = false;
    std::future<TrackerReachabilityProbeResult> probeFuture_;
    TrackerConnectivityState lastState_ = TrackerConnectivityState::Unknown;
    std::string lastDiagnostic_;
    bool recoveryPending_ = false;
    std::atomic<bool> fieldCatalogRefetchAfterLiveTicketSyncPending_{false};
    mutable std::atomic<bool> deferredLiveTrackerBackendSuccessNotify_{false};
    // Single owner of the live ticket-sync warning string (Phase 3 R2): moving it here makes any
    // un-repointed writer fail to compile. Second logical writer is TicketSyncService via the
    // adapter's re-pointed SetLastTicketSyncWarning; both run on the UI tick.
    std::string lastTicketSyncWarning_;
    /// Whether lastTicketSyncWarning_ stems from a transport-shaped failure — set with the
    /// message (same single-writer UI-tick discipline as the string; see the class comment).
    bool lastTicketSyncWarningTransient_ = false;
};
