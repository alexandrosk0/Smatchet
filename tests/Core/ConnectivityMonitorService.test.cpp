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
// ConnectivityMonitorService. The one piece of shared world state is the process-global activity
// hub (AppActivity.h): cases that report into it hold an ActivityHubReset guard, so every other
// case sees the never-reported (never idle) default. No order dependencies.

#include "../support/FakeConnectivityDeps.h"
#include "../support/FakeTrackerClient.h"

#include "AppActivity.h"
#include "ConnectivityMonitorService.h"
#include "ConnectivityProbeSchedule.h"
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

TEST_CASE("ConnectivityMonitorService::GetBannerForUi startup snapshot wording from AppController is recognised") {
    FakeConnectivityDeps deps;
    deps.CatalogWarningImpl =
        "Working offline: tracker field catalog loaded from local snapshot until a live refresh succeeds.";
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

// ---------------------------------------------------------------------------
// Background-traffic idle gating (docs/plans/background-traffic-idle-gating.md): the activity hub,
// the pure probe schedule, and the FSM wiring (outage backoff, live-success push, idle pause).
// ---------------------------------------------------------------------------

namespace {

// The activity hub is process-global; every case that reports into it resets it on exit so the
// never-reported default (never idle) holds for every other case.
struct ActivityHubReset {
    ActivityHubReset() { smatchet::activity::ResetForTest(); }
    ~ActivityHubReset() { smatchet::activity::ResetForTest(); }
};

// Force one probe round trip: clear the rate-limit gate, launch, then tick until the result is
// applied (applying always moves nextProbeAt_ past the cleared deadline). Returns the scheduled
// delay to the next probe.
std::chrono::steady_clock::duration RunOneProbe(ConnectivityMonitorService& svc, const TrackerConfig& cfg) {
    const std::chrono::steady_clock::time_point cleared = std::chrono::steady_clock::now();
    svc.SetNextProbeAt(cleared);
    svc.TickTrackerConnectivityMonitor(cfg);
    for (int i = 0; i < 200 && svc.GetNextProbeAt() == cleared; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        svc.TickTrackerConnectivityMonitor(cfg);
    }
    return svc.GetNextProbeAt() - std::chrono::steady_clock::now();
}

bool DelayNear(std::chrono::steady_clock::duration delay, std::chrono::seconds expected) {
    return delay <= expected && delay > expected - std::chrono::seconds(5);
}

} // namespace

TEST_CASE("AppActivity::IsBackgroundIdleAt truth table") {
    using smatchet::activity::IsBackgroundIdleAt;
    using smatchet::activity::kBackgroundIdleAfter;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point stale = now - kBackgroundIdleAfter - std::chrono::seconds(1);
    const std::chrono::steady_clock::time_point recent = now - std::chrono::seconds(10);
    CHECK_FALSE(IsBackgroundIdleAt(/*reported=*/false, /*focused=*/false, stale, now)); // no host signal
    CHECK_FALSE(IsBackgroundIdleAt(true, /*focused=*/true, stale, now));                // focused
    CHECK_FALSE(IsBackgroundIdleAt(true, false, recent, now));                          // recently active
    CHECK(IsBackgroundIdleAt(true, false, stale, now));                                 // unfocused + stale
}

TEST_CASE("AppActivity hub: input while unfocused keeps the app active; silence makes it idle") {
    ActivityHubReset reset;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(smatchet::activity::IsBackgroundIdle(t0 + std::chrono::hours(1))); // never reported

    smatchet::activity::NoteFrame(/*focused=*/true, /*hadInput=*/false, t0);
    smatchet::activity::NoteFrame(/*focused=*/false, /*hadInput=*/false, t0 + std::chrono::seconds(1));
    CHECK_FALSE(smatchet::activity::IsBackgroundIdle(t0 + std::chrono::minutes(4)));
    CHECK(smatchet::activity::IsBackgroundIdle(t0 + std::chrono::minutes(6)));

    // Hovering or scrolling an unfocused window is activity.
    smatchet::activity::NoteFrame(false, /*hadInput=*/true, t0 + std::chrono::minutes(6));
    CHECK_FALSE(smatchet::activity::IsBackgroundIdle(t0 + std::chrono::minutes(7)));

    // Regaining focus ends idle at once.
    smatchet::activity::NoteFrame(true, false, t0 + std::chrono::minutes(20));
    CHECK_FALSE(smatchet::activity::IsBackgroundIdle(t0 + std::chrono::minutes(20)));
}

TEST_CASE("ConnectivityProbeSchedule::DegradedProbeInterval doubles to the cap") {
    CHECK(smatchet::DegradedProbeInterval(0) == std::chrono::seconds(20));
    CHECK(smatchet::DegradedProbeInterval(1) == std::chrono::seconds(20));
    CHECK(smatchet::DegradedProbeInterval(2) == std::chrono::seconds(40));
    CHECK(smatchet::DegradedProbeInterval(3) == std::chrono::seconds(80));
    CHECK(smatchet::DegradedProbeInterval(4) == std::chrono::seconds(120));
    CHECK(smatchet::DegradedProbeInterval(50) == std::chrono::seconds(120));
}

TEST_CASE("ConnectivityProbeSchedule::NextProbeAfterLiveSuccess pushes only a healthy FSM with no probe in flight") {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point soon = now + std::chrono::seconds(5);
    const std::chrono::steady_clock::time_point pushed = now + smatchet::kProbeRelaxedInterval;
    CHECK(smatchet::NextProbeAfterLiveSuccess(soon, now, /*wasHealthy=*/true, /*probeInFlight=*/false) == pushed);
    CHECK(smatchet::NextProbeAfterLiveSuccess(soon, now, /*wasHealthy=*/false, false) == soon);
    CHECK(smatchet::NextProbeAfterLiveSuccess(soon, now, true, /*probeInFlight=*/true) == soon);
    // Never pulls a later deadline earlier.
    const std::chrono::steady_clock::time_point later = now + std::chrono::minutes(10);
    CHECK(smatchet::NextProbeAfterLiveSuccess(later, now, true, false) == later);
}

TEST_CASE("ConnectivityMonitorService backs off a degraded probe streak and resets on recovery") {
    FakeConnectivityDeps deps;
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::TransportDown, "refused");
    ConnectivityMonitorService svc(deps);
    TrackerConfig cfg;

    CHECK(DelayNear(RunOneProbe(svc, cfg), std::chrono::seconds(20)));
    CHECK(DelayNear(RunOneProbe(svc, cfg), std::chrono::seconds(40)));
    CHECK(DelayNear(RunOneProbe(svc, cfg), std::chrono::seconds(80)));
    CHECK(DelayNear(RunOneProbe(svc, cfg), std::chrono::seconds(120)));
    CHECK(DelayNear(RunOneProbe(svc, cfg), std::chrono::seconds(120)));

    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    CHECK(DelayNear(RunOneProbe(svc, cfg), std::chrono::seconds(90)));
    CHECK(svc.GetLastState() == TrackerConnectivityState::AuthenticatedReachable);

    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::TransportDown, "refused");
    CHECK(DelayNear(RunOneProbe(svc, cfg), std::chrono::seconds(20))); // streak restarted
}

TEST_CASE("ConnectivityMonitorService live success while healthy pushes the next probe out") {
    FakeConnectivityDeps deps;
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    ConnectivityMonitorService svc(deps);
    TrackerConfig cfg;
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::AuthenticatedReachable));

    svc.SetNextProbeAt(std::chrono::steady_clock::now());
    svc.RequestDeferredLiveTrackerBackendSuccessNotify();
    REQUIRE(svc.ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny());
    CHECK(DelayNear(svc.GetNextProbeAt() - std::chrono::steady_clock::now(), std::chrono::seconds(90)));
}

TEST_CASE("ConnectivityMonitorService live success after an outage leaves the recovery probe due") {
    FakeConnectivityDeps deps;
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::TransportDown, "refused");
    ConnectivityMonitorService svc(deps);
    TrackerConfig cfg;
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::TransportDown));

    const std::chrono::steady_clock::time_point due = std::chrono::steady_clock::now();
    svc.SetNextProbeAt(due);
    svc.RequestDeferredLiveTrackerBackendSuccessNotify();
    REQUIRE(svc.ConsumeDeferredLiveTrackerBackendSuccessNotifyIfAny());
    CHECK(svc.GetNextProbeAt() == due);
}

TEST_CASE("ConnectivityMonitorService launches no probe while background-idle, probes on return") {
    ActivityHubReset reset;
    FakeConnectivityDeps deps;
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    ConnectivityMonitorService svc(deps);
    TrackerConfig cfg;

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    smatchet::activity::NoteFrame(/*focused=*/false, /*hadInput=*/false,
                                  now - smatchet::activity::kBackgroundIdleAfter - std::chrono::seconds(1));
    REQUIRE(smatchet::activity::IsBackgroundIdle(now));
    // A fresh service's deadline is already due; ticking must still not launch anything.
    for (int i = 0; i < 20; ++i) {
        svc.TickTrackerConnectivityMonitor(cfg);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(deps.Fake()->ProbeReachabilityCallCount() == 0);
    CHECK(svc.GetLastState() == TrackerConnectivityState::Unknown);

    smatchet::activity::NoteFrame(/*focused=*/true, /*hadInput=*/false, std::chrono::steady_clock::now());
    CHECK(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::AuthenticatedReachable));
}

TEST_CASE("ConnectivityMonitorService explicit probe request passes the background-idle gate") {
    ActivityHubReset reset;
    FakeConnectivityDeps deps;
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    ConnectivityMonitorService svc(deps);
    TrackerConfig cfg;

    smatchet::activity::NoteFrame(/*focused=*/false, /*hadInput=*/false,
                                  std::chrono::steady_clock::now() - smatchet::activity::kBackgroundIdleAfter -
                                      std::chrono::seconds(1));
    svc.RequestProbeNow(); // a live request just failed at the transport level
    bool settled = false;
    for (int i = 0; i < 200 && !settled; ++i) {
        svc.TickTrackerConnectivityMonitor(cfg);
        settled = svc.GetLastState() == TrackerConnectivityState::AuthenticatedReachable;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(settled);
}

TEST_CASE("AppActivity::NoteActivity (command dispatch) keeps an unfocused app out of idle") {
    ActivityHubReset reset;
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    smatchet::activity::NoteFrame(/*focused=*/false, /*hadInput=*/false, t0);
    smatchet::activity::NoteActivity(t0 + std::chrono::minutes(4));
    CHECK_FALSE(smatchet::activity::IsBackgroundIdle(t0 + std::chrono::minutes(8)));
    CHECK(smatchet::activity::IsBackgroundIdle(t0 + std::chrono::minutes(10)));
}

TEST_CASE("ConnectivityMonitorService down streak resets on recovery after a real loss") {
    FakeConnectivityDeps deps;
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    ConnectivityMonitorService svc(deps);
    TrackerConfig cfg;
    REQUIRE(PumpUntilStateSettled(svc, cfg, TrackerConnectivityState::AuthenticatedReachable));

    // Reachable -> down sets the transient loss warning, which the recovery probe still sees
    // (ConsumeTrackerConnectivityRecovery clears it later in the frame).
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::TransportDown, "refused");
    for (int i = 0; i < 4; ++i) {
        RunOneProbe(svc, cfg);
    }
    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::AuthenticatedReachable, "ok");
    RunOneProbe(svc, cfg);
    REQUIRE(svc.GetLastState() == TrackerConnectivityState::AuthenticatedReachable);

    deps.Fake()->SetReachabilityResult(TrackerReachabilityProbeKind::TransportDown, "refused");
    CHECK(DelayNear(RunOneProbe(svc, cfg), std::chrono::seconds(20)));
}
