#include "KeyedLookupCache.h"
#include "SmatchetResult.h"
#include "StoreLoadLatch.h"
#include "Tracker/TrackerError.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace smatchet::offline;
using ::Result;

namespace {

// Value whose move-assignment throws: CompleteSuccess stores the payload by move-assignment, so this
// exercises a completion that throws after the fetch itself succeeded.
struct ThrowOnMoveAssign {
    int V = 0;
    ThrowOnMoveAssign() = default;
    ThrowOnMoveAssign(const ThrowOnMoveAssign&) = default;
    ThrowOnMoveAssign(ThrowOnMoveAssign&&) = default;
    ThrowOnMoveAssign& operator=(const ThrowOnMoveAssign&) = default;
    ThrowOnMoveAssign& operator=(ThrowOnMoveAssign&&) { throw std::runtime_error("move-assign"); }
};

// Duck-typed store + deps for LoadSavedRowsOnce: rows are plain strings, tasks run inline, and a throw
// from a task is contained the way AppController::LaunchBackgroundTask's firewall contains it.
struct FakeRowsStore {
    bool FailNextRead = false;
    int Reads = 0;
    std::vector<std::string> LoadLookups(const std::string& /*backendKey*/, const std::string& /*kind*/) {
        ++Reads;
        if (FailNextRead) {
            FailNextRead = false;
            throw std::runtime_error("store read failed");
        }
        return {"row-1", "row-2"};
    }
};

struct FakeRowsDeps {
    std::shared_ptr<FakeRowsStore> Store;
    bool LaunchThrows = false;
    int Contained = 0;
    std::shared_ptr<FakeRowsStore> LookupCacheShared() { return Store; }
    void LaunchBackgroundTask(std::function<void()> task) {
        if (LaunchThrows) {
            throw std::runtime_error("no thread");
        }
        try {
            task();
        } catch (const std::exception&) {
            ++Contained;
        }
    }
};

} // namespace

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

    TEST_CASE("RunKeyedFetch: a throwing completion still clears InFlight and the key can retry") {
        KeyedLookupCache<ThrowOnMoveAssign> cache;
        const auto now = Clock::now();
        KeyedLookupCache<ThrowOnMoveAssign>::Ticket t;
        REQUIRE(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));

        auto fetch = []() { return Result<ThrowOnMoveAssign, TrackerError>::Ok(ThrowOnMoveAssign()); };
        CHECK_THROWS(RunKeyedFetch(cache, t, fetch));

        const auto entry = cache.Get("key1");
        CHECK_FALSE(entry.InFlight);
        CHECK(entry.LastAttemptFailed);
        CHECK_FALSE(entry.HasValue);
        cache.OnConnectivityRecovered();
        CHECK(cache.TryBeginFetch("key1", TrackerConnectivityState::AuthenticatedReachable, now, t));
    }
} // TEST_SUITE

TEST_SUITE("StoreLoadLatch") {
    TEST_CASE("a key is claimed once; a failed load releases it after a backoff") {
        StoreLoadLatch latch(30);
        const Clock::time_point now = Clock::now();

        CHECK(latch.TryClaim("Jira", now));
        CHECK_FALSE(latch.TryClaim("Jira", now)); // loading
        CHECK(latch.TryClaim("Plane", now));      // keys are independent

        latch.MarkFailed("Jira", now);
        CHECK_FALSE(latch.TryClaim("Jira", now + std::chrono::seconds(29)));
        CHECK(latch.TryClaim("Jira", now + std::chrono::seconds(31)));

        latch.MarkLoaded("Jira");
        CHECK_FALSE(latch.TryClaim("Jira", now + std::chrono::seconds(3600))); // loaded: never again
    }

    TEST_CASE("ClearBackoff lets a released key be claimed at once") {
        StoreLoadLatch latch(30);
        const Clock::time_point now = Clock::now();
        REQUIRE(latch.TryClaim("Jira", now));
        latch.MarkFailed("Jira", now);
        CHECK_FALSE(latch.TryClaim("Jira", now));

        latch.ClearBackoff();
        CHECK(latch.TryClaim("Jira", now));
    }

    TEST_CASE("LoadSavedRowsOnce: loads once; a failed read backs off; no store claims nothing") {
        FakeRowsDeps deps;
        StoreLoadLatch latch(30);
        int applied = 0;
        const auto apply = [&applied](const std::string& key, const std::vector<std::string>& rows) {
            CHECK(key == "Jira");
            applied += static_cast<int>(rows.size());
        };

        // No store yet: nothing is claimed, so the call after the store appears loads at once.
        CHECK(LoadSavedRowsOnce(deps, latch, "Jira", "kind", "test rows", apply));
        CHECK(applied == 0);

        deps.Store = std::make_shared<FakeRowsStore>();
        deps.Store->FailNextRead = true;
        CHECK(LoadSavedRowsOnce(deps, latch, "Jira", "kind", "test rows", apply));
        CHECK(deps.Contained == 1); // the read threw on the worker
        CHECK(applied == 0);
        // Released, but backing off: an immediate retry does not read again.
        CHECK(LoadSavedRowsOnce(deps, latch, "Jira", "kind", "test rows", apply));
        CHECK(deps.Store->Reads == 1);

        latch.ClearBackoff();
        CHECK(LoadSavedRowsOnce(deps, latch, "Jira", "kind", "test rows", apply));
        CHECK(applied == 2);
        // Loaded: later calls do nothing.
        CHECK(LoadSavedRowsOnce(deps, latch, "Jira", "kind", "test rows", apply));
        CHECK(deps.Store->Reads == 2);
    }

    TEST_CASE("LoadSavedRowsOnce: a launch that throws returns false and releases the claim") {
        FakeRowsDeps deps;
        deps.Store = std::make_shared<FakeRowsStore>();
        deps.LaunchThrows = true;
        StoreLoadLatch latch(30);

        CHECK_FALSE(LoadSavedRowsOnce(deps, latch, "Jira", "kind", "test rows",
                                      [](const std::string&, const std::vector<std::string>&) {}));
        CHECK_FALSE(latch.TryClaim("Jira", Clock::now())); // backing off, not stuck claimed forever
        latch.ClearBackoff();
        CHECK(latch.TryClaim("Jira", Clock::now()));
    }
} // TEST_SUITE
