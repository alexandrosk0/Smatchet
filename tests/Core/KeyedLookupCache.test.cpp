#include "KeyedLookupCache.h"
#include "SmatchetResult.h"
#include "Tracker/TrackerError.h"

#include <doctest/doctest.h>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace smatchet::offline;
using ::Result;

TEST_SUITE("KeyedLookupCache") {

    TEST_CASE("TryBeginFetch returns true on first call, false while in flight") {
        KeyedLookupCache<int> cache;
        KeyedLookupCache<int>::Ticket t1, t2;
        auto now = Clock::now();

        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t1));
        CHECK_FALSE(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t2));
    }

    TEST_CASE("CompleteSuccess marks fresh and later TryBeginFetch returns false") {
        KeyedLookupCache<int> cache;
        KeyedLookupCache<int>::Ticket t;
        auto now = Clock::now();

        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));
        cache.CompleteSuccess(t, 42);

        auto entry = cache.Get("key1");
        CHECK(entry.HasValue);
        CHECK(entry.Live);
        CHECK(entry.Payload == 42);

        CHECK_FALSE(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));
    }

    TEST_CASE("SeedFromStore sets value, CompleteFailure keeps it, retry-after delays next fetch") {
        KeyedLookupCache<int> cache;
        auto now = Clock::now();

        cache.SeedFromStore("key1", 100);

        KeyedLookupCache<int>::Ticket t;
        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));
        cache.CompleteFailure(t, TrackerErrorServer("error", 500), now);

        auto entry = cache.Get("key1");
        CHECK(entry.HasValue);
        CHECK(entry.Payload == 100);
        CHECK(entry.LastAttemptFailed);
        CHECK_FALSE(entry.Live);

        CHECK_FALSE(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));

        const auto later = now + std::chrono::seconds(31);
        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, later, t));
    }

    TEST_CASE("TryBeginFetch blocks while connectivity is offline") {
        KeyedLookupCache<int> cache;
        KeyedLookupCache<int>::Ticket t;
        auto now = Clock::now();

        CHECK_FALSE(cache.TryBeginFetch("key1", TrackerConnectivityState::TransportDown, now, t));

        auto fresh = cache.Freshness("key1", TrackerConnectivityState::TransportDown);
        CHECK(fresh == DataFreshness::UnavailableNoCache);
    }

    TEST_CASE("TryBeginFetch returns true after offline with cached data") {
        KeyedLookupCache<int> cache;
        KeyedLookupCache<int>::Ticket t;
        auto now = Clock::now();

        cache.SeedFromStore("key1", 50);

        CHECK_FALSE(cache.TryBeginFetch("key1", TrackerConnectivityState::TransportDown, now, t));
        auto fresh = cache.Freshness("key1", TrackerConnectivityState::TransportDown);
        CHECK(fresh == DataFreshness::CachedOffline);
    }

    TEST_CASE("OnConnectivityRecovered clears backoff") {
        KeyedLookupCache<int> cache;
        auto now = Clock::now();

        KeyedLookupCache<int>::Ticket t;
        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));
        cache.CompleteFailure(t, TrackerErrorTransport("offline", 0), now);

        CHECK_FALSE(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));

        cache.OnConnectivityRecovered();
        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));
    }

    TEST_CASE("Invalidate clears entry, stale CompleteSuccess becomes no-op") {
        KeyedLookupCache<int> cache;
        auto now = Clock::now();

        KeyedLookupCache<int>::Ticket t1;
        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t1));
        cache.Invalidate("key1");

        cache.CompleteSuccess(t1, 99);
        auto entry = cache.Get("key1");
        CHECK_FALSE(entry.HasValue);
    }

    TEST_CASE("RunKeyedFetch with exception records Unknown failure and rethrows") {
        KeyedLookupCache<int> cache;
        auto now = Clock::now();

        KeyedLookupCache<int>::Ticket t;
        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));

        auto lambda = []() -> Result<int, TrackerError> { throw std::runtime_error("test"); };
        CHECK_THROWS(RunKeyedFetch(cache, t, lambda));

        auto entry = cache.Get("key1");
        CHECK_FALSE(entry.InFlight);
        CHECK(entry.LastAttemptFailed);
    }

    TEST_CASE("SeedFromStore never overrides a live value") {
        KeyedLookupCache<int> cache;
        auto now = Clock::now();

        KeyedLookupCache<int>::Ticket t;
        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));
        cache.CompleteSuccess(t, 42);

        cache.SeedFromStore("key1", 100);

        auto entry = cache.Get("key1");
        CHECK(entry.Payload == 42);
    }

    TEST_CASE("Concurrent TryBeginFetch on same key, at most one wins") {
        KeyedLookupCache<int> cache;
        const auto now = Clock::now();
        std::atomic<int> wins{0};

        std::vector<std::thread> threads;
        threads.reserve(8);
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&cache, &wins, now]() {
                for (int n = 0; n < 1000; ++n) {
                    KeyedLookupCache<int>::Ticket t;
                    if (cache.TryBeginFetch("shared", TrackerConnectivityState::AuthenticatedReachable, now, t)) {
                        wins.fetch_add(1);
                    }
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }

        CHECK(wins.load() == 1);
        CHECK(cache.Get("shared").InFlight);
    }

} // TEST_SUITE
