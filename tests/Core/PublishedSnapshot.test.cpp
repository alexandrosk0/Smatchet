#include <doctest/doctest.h>

#include "Sync/PublishedSnapshot.h"
#include "Sync/SourcedSnapshot.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using smatchet::PublishedSnapshot;
using smatchet::SourcedSnapshot;

namespace {

struct View {
    int Value = 0;
};

const auto kAlwaysCurrent = []() { return true; };
const auto kRunInline = [](std::function<void()> task) { task(); };

} // namespace

TEST_CASE("PublishedSnapshot publishes a rebuilt view and keeps the old one when a read fails") {
    PublishedSnapshot<View> snap;
    REQUIRE(static_cast<bool>(snap.Get()));
    CHECK_FALSE(snap.Loaded());
    CHECK(snap.Get()->Value == 0);

    CHECK(snap.Rebuild([](View& v) { v.Value = 7; }, kAlwaysCurrent).empty());
    CHECK(snap.Loaded());
    CHECK(snap.Get()->Value == 7);

    const std::string err = snap.Rebuild([](View&) { throw std::runtime_error("disk gone"); }, kAlwaysCurrent);
    CHECK(err == "disk gone");
    CHECK(snap.Get()->Value == 7);
    // A read that throws something other than std::exception is reported the same way, never thrown.
    CHECK(snap.Rebuild([](View&) { throw 42; }, kAlwaysCurrent) == "unknown exception");
    CHECK(snap.Get()->Value == 7);

    // A read whose source was replaced meanwhile is dropped, not published.
    CHECK(snap.Rebuild([](View& v) { v.Value = 9; }, []() { return false; }).empty());
    CHECK(snap.Get()->Value == 7);

    snap.Invalidate();
    CHECK_FALSE(snap.Loaded());
    CHECK(snap.Get()->Value == 7); // the old view stays readable until a rebuild replaces it
}

TEST_CASE("PublishedSnapshot paces unforced requests and never forced ones") {
    PublishedSnapshot<View> snap;
    int runs = 0;
    const auto rebuild = [&runs]() { ++runs; };
    CHECK(snap.RequestRebuild(kRunInline, rebuild, /*force=*/false, std::chrono::seconds(60)).empty());
    CHECK(runs == 1);
    CHECK(snap.RequestRebuild(kRunInline, rebuild, /*force=*/false, std::chrono::seconds(60)).empty());
    CHECK(runs == 1); // inside the backoff
    CHECK(snap.RequestRebuild(kRunInline, rebuild, /*force=*/true, std::chrono::seconds(60)).empty());
    CHECK(runs == 2);
    snap.Invalidate(); // clears the backoff
    CHECK(snap.RequestRebuild(kRunInline, rebuild, /*force=*/false, std::chrono::seconds(60)).empty());
    CHECK(runs == 3);
}

TEST_CASE("PublishedSnapshot reruns a rebuild requested while one runs") {
    PublishedSnapshot<View> snap;
    std::vector<std::function<void()>> launched;
    const auto deferLaunch = [&launched](std::function<void()> task) { launched.push_back(std::move(task)); };
    int runs = 0;
    std::function<void()> rebuild = [&]() {
        ++runs;
        if (runs == 1) {
            // A request arriving mid-rebuild: no second worker, the running one goes round again.
            CHECK(snap.RequestRebuild(deferLaunch, rebuild, true, std::chrono::seconds(0)).empty());
        }
    };
    CHECK(snap.RequestRebuild(deferLaunch, rebuild, true, std::chrono::seconds(0)).empty());
    CHECK(snap.RequestRebuild(deferLaunch, rebuild, true, std::chrono::seconds(0)).empty()); // coalesced
    REQUIRE(launched.size() == 1);
    launched[0]();
    CHECK(runs == 2);
    CHECK(launched.size() == 1);
    // Idle again: the next request starts a new worker.
    CHECK(snap.RequestRebuild(deferLaunch, rebuild, true, std::chrono::seconds(0)).empty());
    CHECK(launched.size() == 2);
}

TEST_CASE("PublishedSnapshot recovers from a failed launch and from a rebuild that throws") {
    PublishedSnapshot<View> snap;
    int runs = 0;
    const auto rebuild = [&runs]() { ++runs; };
    const std::string err = snap.RequestRebuild([](std::function<void()>) { throw std::runtime_error("no threads"); },
                                                rebuild, true, std::chrono::seconds(0));
    CHECK(err == "no threads");
    CHECK(snap.RequestRebuild(kRunInline, rebuild, true, std::chrono::seconds(0)).empty());
    CHECK(runs == 1);

    // A rebuild that throws something other than std::exception still releases the single-flight latch.
    CHECK_THROWS(snap.RequestRebuild(kRunInline, []() { throw 42; }, true, std::chrono::seconds(0)));
    CHECK(snap.RequestRebuild(kRunInline, rebuild, true, std::chrono::seconds(0)).empty());
    CHECK(runs == 2);
}

TEST_CASE("PublishedSnapshot rebuilds never overlap and the last request is always seen") {
    PublishedSnapshot<View> snap;
    std::mutex threadsMutex;
    std::vector<std::thread> workers;
    const auto launchThread = [&](std::function<void()> task) {
        std::lock_guard<std::mutex> lock(threadsMutex);
        workers.emplace_back(std::move(task));
    };
    std::atomic<int> latest{0};
    std::atomic<int> active{0};
    std::atomic<bool> overlapped{false};
    const auto rebuild = [&]() {
        if (active.fetch_add(1) != 0) {
            overlapped.store(true);
        }
        const int seen = latest.load();
        snap.Rebuild([seen](View& v) { v.Value = seen; }, kAlwaysCurrent);
        active.fetch_sub(1);
    };

    constexpr int kRequesters = 8;
    constexpr int kRequestsEach = 200;
    std::vector<std::thread> requesters;
    for (int t = 0; t < kRequesters; ++t) {
        requesters.emplace_back([&, t]() {
            for (int i = 1; i <= kRequestsEach; ++i) {
                latest.store(t * kRequestsEach + i);
                snap.RequestRebuild(launchThread, rebuild, true, std::chrono::seconds(0));
            }
        });
    }
    for (std::thread& r : requesters) {
        r.join();
    }
    for (;;) {
        std::vector<std::thread> batch;
        {
            std::lock_guard<std::mutex> lock(threadsMutex);
            batch.swap(workers);
        }
        if (batch.empty()) {
            break;
        }
        for (std::thread& w : batch) {
            w.join();
        }
    }
    CHECK_FALSE(overlapped.load());
    CHECK(snap.Get()->Value == latest.load());
}

namespace {

struct Table {
    int Rows = 0;
    bool Broken = false;
};

// The shape SourcedSnapshot binds to: the offline-queue deps' current cache plus a worker launcher.
struct FakeDeps {
    std::shared_ptr<Table> Current = std::make_shared<Table>();
    std::vector<std::function<void()>> Launched;
    bool ThrowOnCurrent = false;
    std::shared_ptr<Table> CacheShared() const {
        if (ThrowOnCurrent) {
            throw std::runtime_error("cache handle unavailable");
        }
        return Current;
    }
    void LaunchBackgroundTask(std::function<void()> task) { Launched.push_back(std::move(task)); }
};

void FillFromTable(Table& table, View& next) {
    if (table.Broken) {
        throw std::runtime_error("table unreadable");
    }
    next.Value = table.Rows;
}

} // namespace

TEST_CASE("SourcedSnapshot reads the source current when its worker runs") {
    FakeDeps deps;
    SourcedSnapshot<View, Table> snap("test", deps, FillFromTable);
    deps.Current->Rows = 3;
    snap.RequestAsync(/*force=*/true, std::chrono::seconds(0));
    // The source is replaced before the worker runs: the worker reads the new one.
    deps.Current = std::make_shared<Table>();
    deps.Current->Rows = 5;
    REQUIRE(deps.Launched.size() == 1);
    deps.Launched[0]();
    CHECK(snap.Loaded());
    CHECK(snap.Get()->Value == 5);
}

TEST_CASE("SourcedSnapshot drops a read of a replaced source and keeps the view when a read fails") {
    FakeDeps deps;
    SourcedSnapshot<View, Table> snap("test", deps, FillFromTable);
    deps.Current->Rows = 2;
    snap.Publish(*deps.Current);
    CHECK(snap.Get()->Value == 2);

    // A worker that latched the old source publishes nothing once the source was replaced.
    const std::shared_ptr<Table> old = deps.Current;
    old->Rows = 9;
    deps.Current = std::make_shared<Table>();
    snap.Publish(*old);
    CHECK(snap.Get()->Value == 2);

    // An unreadable source keeps the previous view.
    deps.Current->Broken = true;
    snap.Publish(*deps.Current);
    CHECK(snap.Get()->Value == 2);

    // With no source at all, a request reads nothing and publishes nothing.
    deps.Current.reset();
    snap.RequestAsync(/*force=*/true, std::chrono::seconds(0));
    REQUIRE(deps.Launched.size() == 1);
    deps.Launched[0]();
    CHECK(snap.Get()->Value == 2);
}

TEST_CASE("SourcedSnapshot::Publish never throws, so the replay exit guards can call it") {
    FakeDeps deps;
    SourcedSnapshot<View, Table> snap("test", deps, FillFromTable);
    deps.Current->Rows = 4;
    snap.Publish(*deps.Current);
    REQUIRE(snap.Get()->Value == 4);

    // A fill that throws a non-standard exception keeps the view.
    SourcedSnapshot<View, Table> odd("test", deps, [](Table&, View&) { throw 42; });
    CHECK_NOTHROW(odd.Publish(*deps.Current));
    CHECK_FALSE(odd.Loaded());

    // So does a failure outside the read: here the current-source check throws.
    deps.Current->Rows = 8;
    deps.ThrowOnCurrent = true;
    CHECK_NOTHROW(snap.Publish(*deps.Current));
    CHECK(snap.Get()->Value == 4);
}
