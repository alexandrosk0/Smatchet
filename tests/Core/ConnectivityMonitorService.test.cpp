// ConnectivityMonitorService bucket-A tests — exercise the tracker connectivity-probe FSM extracted
// from AppController (god-object decomposition Phase 3) through the IConnectivityDeps interface
// bundle. Tests drive the service against a FakeConnectivityDeps fixture (header-only:
// shared_ptr<FakeTrackerClient> focused backend + plain-member catalog warning/error + recorded
// replay-timer / clear / bump side effects) so no AppController, ImGui, cpr, HTTP, or SQLite surface
// is touched.
//
// First-ever coverage of the banner formatter (GetBannerForUi): error-priority, session-note,
// empty, both-warnings, snapshot-catalog headline, suffix-dedup, and truncation. Plus a
// probe-down -> recovery sequence and a two-writer banner reconciliation (the FSM's probe-loss
// warning + the sync-warning writer share the one ticket-sync string).
//
// The FSM launches its reachability probe via std::async and collects it on a LATER tick, so the
// sequence tests poll TickTrackerConnectivityMonitor until the state settles (bounded). The probe
// future is joined by the service before assertions read state — no detached threads.
//
// Per-case isolation: every TEST_CASE constructs its own FakeConnectivityDeps + fresh
// ConnectivityMonitorService. No statics, no shared world state, no order dependencies.

#include "../support/FakeConnectivityDeps.h"
#include "../support/FakeTrackerClient.h"

#include "ConnectivityMonitorService.h"
#include "Config/ConfigManager.h"
#include "ITrackerConnectivity.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

using smatchet_tests::FakeConnectivityDeps;
using smatchet_tests::FakeTrackerClient;

namespace {

// Tick the FSM until GetLastState() leaves Unknown (the probe future resolved + was applied) or a
// bounded number of iterations elapse. The fake's ProbeReachability returns immediately, so the
// std::async future is ready within microseconds; a short sleep between ticks lets it settle.
bool PumpUntilStateSettled(ConnectivityMonitorService& svc, const TrackerConfig& cfg, TrackerConnectivityState until) {
    for (int i = 0; i < 200; ++i) {
        // Clear the probe rate-limit gate each iteration so a fresh probe fires every tick — after a
        // healthy probe the FSM schedules the next one ~90 s out (intended production rate-limiting),
        // which a wall-clock-bounded unit pump can't reach. SetNextProbeAt is the test seam for it.
        svc.SetNextProbeAt(std::chrono::steady_clock::now());
        svc.TickTrackerConnectivityMonitor(cfg);
        if (svc.GetLastState() == until) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// GetBannerForUi — the banner formatter matrix (pure given catalog/warning inputs).
// ---------------------------------------------------------------------------

TEST_CASE("ConnectivityMonitorService::GetBannerForUi is None/empty with no warnings") {
    FakeConnectivityDeps deps;
    ConnectivityMonitorService svc(deps);
    const auto banner = svc.GetBannerForUi(nullptr);
    CHECK(banner.Kind == TrackerConnectivityBannerForUi::Level::None);
    CHECK(banner.Message.empty());
}

TEST_CASE("ConnectivityMonitorService::GetBannerForUi surfaces catalog error at Error level (priority)") {
    FakeConnectivityDeps deps;
    deps.CatalogErrorImpl = "Field catalog fetch failed: HTTP 500";
    deps.CatalogWarningImpl = "Offline: using cached tracker field catalog. Last fetch failed: x";
    ConnectivityMonitorService svc(deps);
    const auto banner = svc.GetBannerForUi(nullptr);
    CHECK(banner.Kind == TrackerConnectivityBannerForUi::Level::Error);
    CHECK(banner.Message.find("Field catalog fetch failed") != std::string::npos);
}

TEST_CASE("ConnectivityMonitorService::GetBannerForUi shows session note alone as Warning") {
    FakeConnectivityDeps deps;
    ConnectivityMonitorService svc(deps);
    const std::string note = "Users list could not be refreshed";
    const auto banner = svc.GetBannerForUi(&note);
    CHECK(banner.Kind == TrackerConnectivityBannerForUi::Level::Warning);
    CHECK(banner.Message.find("Users list") != std::string::npos);
}

TEST_CASE("ConnectivityMonitorService::GetBannerForUi snapshot-catalog headline (Warning)") {
    FakeConnectivityDeps deps;
    deps.CatalogWarningImpl =
        "Working offline: Jira field catalog loaded from local snapshot until a live refresh succeeds.";
    ConnectivityMonitorService svc(deps);
    const auto banner = svc.GetBannerForUi(nullptr);
    CHECK(banner.Kind == TrackerConnectivityBannerForUi::Level::Warning);
    CHECK(banner.Message.find("local snapshot") != std::string::npos);
}

TEST_CASE("ConnectivityMonitorService::GetBannerForUi both warnings -> combined headline") {
    FakeConnectivityDeps deps;
    deps.CatalogWarningImpl = "Offline: using cached tracker field catalog. Last fetch failed: boom";
    ConnectivityMonitorService svc(deps);
    svc.SetLastTicketSyncWarning("Showing cached issues — lost connection to tracker: boom", /*transient=*/true);
    const auto banner = svc.GetBannerForUi(nullptr);
    CHECK(banner.Kind == TrackerConnectivityBannerForUi::Level::Warning);
    CHECK(banner.Message.find("issue list and field catalog") != std::string::npos);
    // Suffix-dedup: identical catalog + ticket suffix ("boom") collapses to one detail copy.
    const auto first = banner.Message.find("boom");
    REQUIRE(first != std::string::npos);
    CHECK(banner.Message.find("boom", first + 1) == std::string::npos);
}

TEST_CASE("ConnectivityMonitorService::GetBannerForUi truncates an over-long catalog error") {
    FakeConnectivityDeps deps;
    deps.CatalogErrorImpl = std::string(400, 'x');
    ConnectivityMonitorService svc(deps);
    const auto banner = svc.GetBannerForUi(nullptr);
    CHECK(banner.Kind == TrackerConnectivityBannerForUi::Level::Error);
    CHECK(banner.Message.size() <= 240);
    CHECK(banner.Message.find("...") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Probe -> state mapping (exercises MapReachabilityProbeKind indirectly).
// ---------------------------------------------------------------------------

TEST_CASE("ConnectivityMonitorService probe maps AuthenticatedReachable to GetLastState") {
    FakeConnectivityDeps deps;
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    ConnectivityMonitorService svc(deps);
    TrackerConfig cfg;
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::AuthenticatedReachable));
    CHECK(svc.GetLastState() == TrackerConnectivityState::AuthenticatedReachable);
}

TEST_CASE("ConnectivityMonitorService probe-down sets warning + pushes replay timers, recovery clears + restarts") {
    FakeConnectivityDeps deps;
    // First reach authenticated so the next transport-down crosses the loss boundary that sets the
    // ticket-sync warning.
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    ConnectivityMonitorService svc(deps);
    TrackerConfig cfg;
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::AuthenticatedReachable));

    // Flip the backend to report transport-down; pump until the FSM applies it.
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::TransportDown, "connection refused");
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::TransportDown));
    CHECK_FALSE(svc.LastTicketSyncWarning().empty());
    CHECK(svc.LastTicketSyncWarning().find("lost connection to tracker") != std::string::npos);
    CHECK_FALSE(deps.PushReplayCalls.empty());

    // Now recover: a fresh authenticated probe arms the recovery latch.
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::AuthenticatedReachable));

    const int restartsBefore = static_cast<int>(deps.RestartReplayCalls.size());
    CHECK(svc.ConsumeTrackerConnectivityRecovery());
    CHECK(svc.LastTicketSyncWarning().empty());
    CHECK(deps.ClearWarningCalls >= 1);
    CHECK(static_cast<int>(deps.RestartReplayCalls.size()) == restartsBefore + 1);
    // One-shot: a second consume returns false.
    CHECK_FALSE(svc.ConsumeTrackerConnectivityRecovery());
}

// ---------------------------------------------------------------------------
// Two-writer banner reconciliation: the sync-warning writer + the FSM probe-loss writer both feed
// the one ticket-sync string; the banner reflects whichever is current.
// ---------------------------------------------------------------------------

TEST_CASE("ConnectivityMonitorService two-writer reconciliation: sync warning then probe loss") {
    FakeConnectivityDeps deps;
    ConnectivityMonitorService svc(deps);

    // Writer 1 (TicketSyncService analogue, via the re-pointed setter): a sync caveat.
    svc.SetLastTicketSyncWarning("Showing cached issues — live refresh did not complete: timeout",
                                 /*transient=*/true);
    {
        const auto banner = svc.GetBannerForUi(nullptr);
        CHECK(banner.Kind == TrackerConnectivityBannerForUi::Level::Warning);
        CHECK(banner.Message.find("issue list is from cache") != std::string::npos);
    }

    // Writer 2 (the FSM probe-loss): drive authenticated -> transport-down so the FSM overwrites
    // the ticket-sync string with its connectivity-loss text.
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    TrackerConfig cfg;
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::AuthenticatedReachable));
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::TransportDown, "refused");
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::TransportDown));
    CHECK(svc.LastTicketSyncWarning().find("lost connection to tracker") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Deferred live-success notify one-shot.
// ---------------------------------------------------------------------------

TEST_CASE("ConnectivityMonitorService deferred live-success notify is one-shot + clears catalog warning") {
    FakeConnectivityDeps deps;
    deps.CatalogWarningImpl = "Offline: using cached tracker field catalog. Last fetch failed: x";
    ConnectivityMonitorService svc(deps);

    svc.RequestDeferredLiveTrackerBackendSuccessNotify();
    CHECK(svc.ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny());
    // Cleared the stale catalog banner + armed the refetch latch.
    CHECK(deps.ClearWarningCalls >= 1);
    CHECK(deps.BumpRevisionCalls >= 1);
    CHECK(svc.ConsumeFieldCatalogRefetchAfterLiveTicketSync());
    // One-shot on both latches.
    CHECK_FALSE(svc.ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny());
    CHECK_FALSE(svc.ConsumeFieldCatalogRefetchAfterLiveTicketSync());
}
