#include "OfflineFirstPure.h"

#include <doctest/doctest.h>

using namespace smatchet::offline;

TEST_SUITE("OfflineFirstPure") {

TEST_CASE("IsOfflineState recognizes TransportDown") {
    CHECK(IsOfflineState(TrackerConnectivityState::TransportDown));
}

TEST_CASE("IsOfflineState recognizes ServiceUnavailable") {
    CHECK(IsOfflineState(TrackerConnectivityState::ServiceUnavailable));
}

TEST_CASE("IsOfflineState rejects Unknown") {
    CHECK_FALSE(IsOfflineState(TrackerConnectivityState::Unknown));
}

TEST_CASE("IsOfflineState rejects AuthenticatedReachable") {
    CHECK_FALSE(IsOfflineState(TrackerConnectivityState::AuthenticatedReachable));
}

TEST_CASE("IsOfflineState rejects ReachableAuthOrConfigError") {
    CHECK_FALSE(IsOfflineState(TrackerConnectivityState::ReachableAuthOrConfigError));
}

TEST_CASE("ShouldAttemptNetwork blocks while offline") {
    const auto now = Clock::now();
    CHECK_FALSE(ShouldAttemptNetwork(TrackerConnectivityState::TransportDown, now, Clock::time_point()));
    CHECK_FALSE(ShouldAttemptNetwork(TrackerConnectivityState::ServiceUnavailable, now, Clock::time_point()));
}

TEST_CASE("ShouldAttemptNetwork blocks while backoff is active") {
    const auto now = Clock::now();
    const auto future = now + std::chrono::seconds(10);
    CHECK_FALSE(ShouldAttemptNetwork(TrackerConnectivityState::AuthenticatedReachable, now, future));
}

TEST_CASE("ShouldAttemptNetwork allows when online and backoff passed") {
    const auto now = Clock::now();
    const auto past = now - std::chrono::seconds(10);
    CHECK(ShouldAttemptNetwork(TrackerConnectivityState::AuthenticatedReachable, now, past));
}

TEST_CASE("ClassifyFreshness: no cache, no flight") {
    FreshnessInputs in;
    CHECK(ClassifyFreshness(in) == DataFreshness::UnavailableNoCache);
}

TEST_CASE("ClassifyFreshness: no cache, in flight") {
    FreshnessInputs in;
    in.InFlight = true;
    CHECK(ClassifyFreshness(in) == DataFreshness::LoadingNoCache);
}

TEST_CASE("ClassifyFreshness: has cache, live, no failure") {
    FreshnessInputs in;
    in.HasCache = true;
    in.Live = true;
    CHECK(ClassifyFreshness(in) == DataFreshness::Fresh);
}

TEST_CASE("ClassifyFreshness: has cache, in flight") {
    FreshnessInputs in;
    in.HasCache = true;
    in.InFlight = true;
    CHECK(ClassifyFreshness(in) == DataFreshness::Refreshing);
}

TEST_CASE("ClassifyFreshness: has cache, offline") {
    FreshnessInputs in;
    in.HasCache = true;
    in.Connectivity = TrackerConnectivityState::TransportDown;
    CHECK(ClassifyFreshness(in) == DataFreshness::CachedOffline);
}

TEST_CASE("ClassifyFreshness: has cache, stale") {
    FreshnessInputs in;
    in.HasCache = true;
    in.LastAttemptFailed = true;
    in.Connectivity = TrackerConnectivityState::AuthenticatedReachable;
    CHECK(ClassifyFreshness(in) == DataFreshness::CachedStale);
}

TEST_CASE("ShouldRenderContent: LoadingNoCache") {
    CHECK_FALSE(ShouldRenderContent(DataFreshness::LoadingNoCache));
}

TEST_CASE("ShouldRenderContent: UnavailableNoCache") {
    CHECK_FALSE(ShouldRenderContent(DataFreshness::UnavailableNoCache));
}

TEST_CASE("ShouldRenderContent: Fresh") {
    CHECK(ShouldRenderContent(DataFreshness::Fresh));
}

TEST_CASE("ShouldRenderContent: Refreshing") {
    CHECK(ShouldRenderContent(DataFreshness::Refreshing));
}

TEST_CASE("ShouldRenderContent: CachedOffline") {
    CHECK(ShouldRenderContent(DataFreshness::CachedOffline));
}

TEST_CASE("ShouldRenderContent: CachedStale") {
    CHECK(ShouldRenderContent(DataFreshness::CachedStale));
}

TEST_CASE("RouteWrite: read-only preference rejects") {
    CHECK(RouteWrite(TrackerConnectivityState::AuthenticatedReachable, true, true) == WriteRoute::Reject);
}

TEST_CASE("RouteWrite: offline and queueable queues") {
    CHECK(RouteWrite(TrackerConnectivityState::TransportDown, true, false) == WriteRoute::QueueImmediately);
}

TEST_CASE("RouteWrite: offline and not queueable networks") {
    CHECK(RouteWrite(TrackerConnectivityState::TransportDown, false, false) == WriteRoute::NetworkFirst);
}

TEST_CASE("RouteWrite: online goes network") {
    CHECK(RouteWrite(TrackerConnectivityState::AuthenticatedReachable, true, false) == WriteRoute::NetworkFirst);
}

} // TEST_SUITE
