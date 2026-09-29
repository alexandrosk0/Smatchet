// ITrackerIssueReader's default FetchIssuesChangedSince / FetchIssueKeysForView (used by backends
// without a native query) keep the error kind the backend classified (Quality Pillar 6): an
// unreachable tracker stays Transport, so the change monitor keeps its cached view and retries
// instead of reading the failure as permanent. Only an unclassified failure falls back to Unknown.

#include "Config/ConfigManager.h" // TrackerConfig, ViewsStore
#include "FakeNetworkSwitch.h"
#include "FakeTrackerClient.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

using smatchet_tests::FakeNetworkMode;
using smatchet_tests::FakeTrackerClient;
using smatchet_tests::GlobalFakeNetwork;
using smatchet_tests::ScopedFakeNetworkReset;

TEST_SUITE("TrackerIssueReaderDefaults") {

    TEST_CASE("an unreachable tracker stays Transport through both defaults") {
        ScopedFakeNetworkReset reset;
        FakeTrackerClient client;
        client.AttachNetwork(&GlobalFakeNetwork());
        GlobalFakeNetwork().Set(FakeNetworkMode::TransportDown);
        const TrackerConfig cfg;
        const ViewsStore views;

        const auto keys = client.Reader().FetchIssueKeysForView(cfg, views);
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error().Kind == TrackerErrorKind::Transport);
        CHECK(keys.error().IsRetryable());

        const auto changed =
            client.Reader().FetchIssuesChangedSince(cfg, views, std::chrono::seconds(300), std::vector<std::string>());
        REQUIRE_FALSE(changed.has_value());
        CHECK(changed.error().Kind == TrackerErrorKind::Transport);
        CHECK(changed.error().IsRetryable());
    }

    TEST_CASE("a service outage stays a retryable server error") {
        ScopedFakeNetworkReset reset;
        FakeTrackerClient client;
        client.AttachNetwork(&GlobalFakeNetwork());
        GlobalFakeNetwork().Set(FakeNetworkMode::ServiceUnavailable);

        const auto keys = client.Reader().FetchIssueKeysForView(TrackerConfig(), ViewsStore());
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error().Kind == TrackerErrorKind::ServerError);
        CHECK(keys.error().IsRetryable());
    }

    TEST_CASE("a failure the backend did not classify still reads Unknown, with its text") {
        FakeTrackerClient client;
        client.EnqueueFetchResult({}, false, "legacy failure text");

        const auto keys = client.Reader().FetchIssueKeysForView(TrackerConfig(), ViewsStore());
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error().Kind == TrackerErrorKind::Unknown);
        CHECK(keys.error().Detail == "legacy failure text");
    }
}
