#include "KeyedLookupCache.h"
#include "SmatchetResult.h"
#include "StoreLoadLatch.h"
#include "TrackerError.h"

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

    TEST_CASE("TryBeginFetch is refused offline even with cached data, which reads CachedOffline") {
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

TEST_CASE("KeyedLookupCache: CompleteFromSaved holds a saved copy, defers the refetch to its expiry, and ClearBackoff "
          "reopens it") {
    KeyedLookupCache<int> cache;
    const auto t0 = Clock::now();
    KeyedLookupCache<int>::Ticket ticket;
    REQUIRE(cache.TryBeginFetch("k", TrackerConnectivityState::AuthenticatedReachable, t0, ticket));

    cache.CompleteFromSaved(ticket, 7, t0 + std::chrono::seconds(100));
    cache.CompleteSuccess(ticket, 99); // the spent ticket: a normal success afterwards must not make it live

    const KeyedLookupCache<int>::Entry e = cache.Get("k");
    CHECK(e.HasValue);
    CHECK(e.Payload == 7);
    CHECK_FALSE(e.Live);
    CHECK_FALSE(e.InFlight);
    CHECK(cache.Freshness("k", TrackerConnectivityState::AuthenticatedReachable) == DataFreshness::CachedStale);

    KeyedLookupCache<int>::Ticket again;
    CHECK_FALSE(cache.TryBeginFetch("k", TrackerConnectivityState::AuthenticatedReachable,
                                    t0 + std::chrono::seconds(50), again));
    CHECK(cache.TryBeginFetch("k", TrackerConnectivityState::AuthenticatedReachable, t0 + std::chrono::seconds(100),
                              again));

    cache.CompleteFromSaved(again, 7, t0 + std::chrono::seconds(500));
    CHECK_FALSE(cache.TryBeginFetch("k", TrackerConnectivityState::AuthenticatedReachable,
                                    t0 + std::chrono::seconds(200), again));
    cache.ClearBackoff("k"); // a user's Refresh
    CHECK(cache.TryBeginFetch("k", TrackerConnectivityState::AuthenticatedReachable, t0 + std::chrono::seconds(200),
                              again));
}

namespace {

constexpr TrackerConnectivityState kUp = TrackerConnectivityState::AuthenticatedReachable;

// Records a Live value for `key` at `at`.
void MakeLive(KeyedLookupCache<int>& cache, const std::string& key, int value, Clock::time_point at) {
    KeyedLookupCache<int>::Ticket t;
    REQUIRE(cache.TryBeginFetch(key, kUp, at, t));
    cache.CompleteSuccess(t, value, at);
}

} // namespace

TEST_SUITE("KeyedLookupCache admission") {

    TEST_CASE("the default admission never refetches a Live value, however old") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "k", 1, t0);
        KeyedLookupCache<int>::Ticket t;
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, t0 + std::chrono::hours(1000), t));
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, t0 + std::chrono::hours(1000), t, FetchAdmission()));
        CHECK(cache.IsCurrent("k", t0 + std::chrono::hours(1000), FetchAdmission()));
    }

    TEST_CASE("a refused TryBeginFetch creates no entry") {
        KeyedLookupCache<int> cache;
        KeyedLookupCache<int>::Ticket t;
        CHECK_FALSE(cache.TryBeginFetch("k", TrackerConnectivityState::TransportDown, Clock::now(), t));
        CHECK(cache.Size() == 0);
        CHECK(cache.TryBeginFetch("k", kUp, Clock::now(), t));
        CHECK(cache.Size() == 1);
    }

    TEST_CASE("LiveMaxAge admits a Live value exactly at its age limit and not before") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "k", 1, t0);
        FetchAdmission admit;
        admit.LiveMaxAge = std::chrono::seconds(100);

        KeyedLookupCache<int>::Ticket t;
        CHECK(cache.IsCurrent("k", t0 + std::chrono::seconds(99), admit));
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, t0 + std::chrono::seconds(99), t, admit));
        CHECK_FALSE(cache.IsCurrent("k", t0 + std::chrono::seconds(100), admit));
        REQUIRE(cache.TryBeginFetch("k", kUp, t0 + std::chrono::seconds(100), t, admit));
        CHECK(t.WasLive);
    }

    TEST_CASE("a zero or negative LiveMaxAge disables the age limit") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "k", 1, t0);
        FetchAdmission admit;
        admit.LiveMaxAge = std::chrono::seconds(-5);
        KeyedLookupCache<int>::Ticket t;
        CHECK(cache.IsCurrent("k", t0 + std::chrono::hours(5), admit));
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, t0 + std::chrono::hours(5), t, admit));
    }

    TEST_CASE("a revalidation keeps the Live value, reads Refreshing, and a success restamps its age") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "k", 1, t0);
        FetchAdmission admit;
        admit.LiveMaxAge = std::chrono::seconds(100);

        const auto t1 = t0 + std::chrono::seconds(150);
        KeyedLookupCache<int>::Ticket t;
        REQUIRE(cache.TryBeginFetch("k", kUp, t1, t, admit));
        KeyedLookupCache<int>::Entry e = cache.Get("k");
        CHECK(e.HasValue);
        CHECK(e.Live);
        CHECK(e.InFlight);
        CHECK(e.Payload == 1);
        CHECK(cache.Freshness("k", kUp) == DataFreshness::Refreshing);
        // one winner: nothing else starts while it runs
        KeyedLookupCache<int>::Ticket other;
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, t1, other, admit));

        cache.CompleteSuccess(t, 2, t1);
        e = cache.Get("k");
        CHECK(e.Payload == 2);
        CHECK(e.Live);
        CHECK_FALSE(e.InFlight);
        CHECK(cache.IsCurrent("k", t1 + std::chrono::seconds(99), admit));
        CHECK_FALSE(cache.IsCurrent("k", t1 + std::chrono::seconds(100), admit));
    }

    TEST_CASE("a failed revalidation keeps the value and reads CachedStale, then backs off") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "k", 1, t0);
        FetchAdmission admit;
        admit.Revalidate = true;

        KeyedLookupCache<int>::Ticket t;
        REQUIRE(cache.TryBeginFetch("k", kUp, t0, t, admit));
        cache.CompleteFailure(t, TrackerErrorServer("boom", 500), t0);

        const KeyedLookupCache<int>::Entry e = cache.Get("k");
        CHECK(e.HasValue);
        CHECK(e.Live);
        CHECK(e.LastAttemptFailed);
        CHECK(e.Payload == 1);
        CHECK(cache.Freshness("k", kUp) == DataFreshness::CachedStale);
        CHECK_FALSE(cache.IsCurrent("k", t0, FetchAdmission()));

        KeyedLookupCache<int>::Ticket again;
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, t0 + std::chrono::seconds(29), again, admit));
        CHECK(cache.TryBeginFetch("k", kUp, t0 + std::chrono::seconds(30), again, admit));
    }

    TEST_CASE("Revalidate admits a Live value but still waits for connectivity, backoff and in-flight") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "k", 1, t0);
        FetchAdmission admit;
        admit.Revalidate = true;
        KeyedLookupCache<int>::Ticket t;

        CHECK_FALSE(cache.IsCurrent("k", t0, admit));
        CHECK_FALSE(cache.TryBeginFetch("k", TrackerConnectivityState::TransportDown, t0, t, admit));
        CHECK(cache.Get("k").Live);
        REQUIRE(cache.TryBeginFetch("k", kUp, t0, t, admit));
        KeyedLookupCache<int>::Ticket second;
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, t0, second, admit));
        cache.CompleteFailure(t, TrackerErrorServer("boom", 500), t0);
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, t0 + std::chrono::seconds(1), second, admit));
        cache.ClearBackoff("k");
        CHECK(cache.TryBeginFetch("k", kUp, t0 + std::chrono::seconds(1), second, admit));
    }

    TEST_CASE("a ticket for a value that was not Live reports WasLive false") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        KeyedLookupCache<int>::Ticket t;
        REQUIRE(cache.TryBeginFetch("fresh", kUp, t0, t));
        CHECK_FALSE(t.WasLive);
        cache.SeedFromStore("seeded", 5);
        REQUIRE(cache.TryBeginFetch("seeded", kUp, t0, t));
        CHECK_FALSE(t.WasLive);
    }

    TEST_CASE("the stale predicate admits a Live value and sees its payload") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "k", 7, t0);
        KeyedLookupCache<int>::Ticket t;
        int seen = 0;
        CHECK_FALSE(cache.TryBeginFetchIf("k", kUp, t0, t, FetchAdmission(), [&seen](const int& v) {
            seen = v;
            return false;
        }));
        CHECK(seen == 7);
        CHECK(cache.TryBeginFetchIf("k", kUp, t0, t, FetchAdmission(), [](const int& v) { return v == 7; }));
        CHECK(t.WasLive);
        // the predicate is not consulted for a key that is not Live
        KeyedLookupCache<int> empty;
        bool called = false;
        CHECK(empty.TryBeginFetchIf("x", kUp, t0, t, FetchAdmission(), [&called](const int&) {
            called = true;
            return true;
        }));
        CHECK_FALSE(called);
    }

    TEST_CASE("Invalidate during a revalidation drops its result and the next fetch starts clean") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "k", 1, t0);
        FetchAdmission admit;
        admit.Revalidate = true;
        KeyedLookupCache<int>::Ticket t;
        REQUIRE(cache.TryBeginFetch("k", kUp, t0, t, admit));
        cache.Invalidate("k");
        cache.CompleteSuccess(t, 99, t0);
        CHECK_FALSE(cache.Get("k").HasValue);
        KeyedLookupCache<int>::Ticket next;
        REQUIRE(cache.TryBeginFetch("k", kUp, t0, next));
        CHECK_FALSE(next.WasLive);
    }

    TEST_CASE("CompleteFromSaved clears the live stamp: the copy is never current") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        KeyedLookupCache<int>::Ticket t;
        REQUIRE(cache.TryBeginFetch("k", kUp, t0, t));
        cache.CompleteFromSaved(t, 3, t0 + std::chrono::seconds(10));
        CHECK_FALSE(cache.IsCurrent("k", t0, FetchAdmission()));
    }

    TEST_CASE("RetainIf drops rejected entries and empty ones, and never an in-flight one") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        MakeLive(cache, "keep", 1, t0);
        MakeLive(cache, "drop", 2, t0);
        cache.SeedFromStore("seeded-drop", 3);
        KeyedLookupCache<int>::Ticket failed;
        REQUIRE(cache.TryBeginFetch("empty-failed", kUp, t0, failed));
        cache.CompleteFailure(failed, TrackerErrorServer("boom", 500), t0); // no value, not in flight
        KeyedLookupCache<int>::Ticket inflight;
        REQUIRE(cache.TryBeginFetch("inflight", kUp, t0, inflight));
        KeyedLookupCache<int>::Ticket revalidating;
        MakeLive(cache, "inflight-live", 4, t0);
        FetchAdmission admit;
        admit.Revalidate = true;
        REQUIRE(cache.TryBeginFetch("inflight-live", kUp, t0, revalidating, admit));
        REQUIRE(cache.Size() == 6);

        const std::size_t erased = cache.RetainIf([](const std::string& key, const KeyedLookupCache<int>::Entry& e) {
            return key == "keep" && e.Payload == 1;
        });

        CHECK(erased == 3); // "drop", "seeded-drop", "empty-failed"
        CHECK(cache.Size() == 3);
        CHECK(cache.Get("keep").HasValue);
        CHECK_FALSE(cache.Get("drop").HasValue);
        CHECK(cache.Get("inflight").InFlight);
        CHECK(cache.Get("inflight-live").InFlight);
        // the in-flight fetches still complete into their entries
        cache.CompleteSuccess(inflight, 10, t0);
        cache.CompleteSuccess(revalidating, 40, t0);
        CHECK(cache.Get("inflight").Payload == 10);
        CHECK(cache.Get("inflight-live").Payload == 40);
    }

    TEST_CASE("RunKeyedFetch stamps the outcome with the clock it is given") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        KeyedLookupCache<int>::Ticket t;
        REQUIRE(cache.TryBeginFetch("k", kUp, t0, t));
        const auto fake = t0 + std::chrono::seconds(500);
        RunKeyedFetch(cache, t, []() { return Result<int, TrackerError>::Ok(5); }, [fake]() { return fake; });
        FetchAdmission admit;
        admit.LiveMaxAge = std::chrono::seconds(100);
        CHECK(cache.IsCurrent("k", fake + std::chrono::seconds(99), admit));
        CHECK_FALSE(cache.IsCurrent("k", fake + std::chrono::seconds(100), admit));
    }

    TEST_CASE("RunKeyedFetch with a throwing fetch records the failure with the given clock and rethrows") {
        KeyedLookupCache<int> cache;
        const auto t0 = Clock::now();
        KeyedLookupCache<int>::Ticket t;
        REQUIRE(cache.TryBeginFetch("k", kUp, t0, t));
        const auto fake = t0 + std::chrono::seconds(1000);
        CHECK_THROWS_AS(RunKeyedFetch(
                            cache, t, []() -> Result<int, TrackerError> { throw std::runtime_error("x"); },
                            [fake]() { return fake; }),
                        std::runtime_error);
        KeyedLookupCache<int>::Ticket again;
        CHECK_FALSE(cache.TryBeginFetch("k", kUp, fake + std::chrono::seconds(29), again));
        CHECK(cache.TryBeginFetch("k", kUp, fake + std::chrono::seconds(30), again));
    }

    TEST_CASE("concurrent revalidation attempts admit exactly one winner") {
        for (int round = 0; round < 50; ++round) {
            KeyedLookupCache<int> cache;
            const auto t0 = Clock::now();
            MakeLive(cache, "k", 1, t0);
            FetchAdmission admit;
            admit.LiveMaxAge = std::chrono::seconds(10);
            const auto later = t0 + std::chrono::seconds(60);
            std::atomic<int> winners{0};
            std::atomic<bool> go{false};
            std::vector<std::thread> threads;
            for (int i = 0; i < 4; ++i) {
                threads.emplace_back([&]() {
                    while (!go.load()) {
                        std::this_thread::yield();
                    }
                    KeyedLookupCache<int>::Ticket t;
                    if (cache.TryBeginFetch("k", kUp, later, t, admit)) {
                        winners.fetch_add(1);
                    }
                });
            }
            go.store(true);
            for (std::thread& th : threads) {
                th.join();
            }
            CHECK(winners.load() == 1);
        }
    }

} // TEST_SUITE
