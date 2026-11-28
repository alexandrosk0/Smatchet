// EditMetaCacheConcurrent.test.cpp — EditMetaCacheService TSan pass (AppController god-object
// decomposition Phase 1).
//
// EnsureIssueEditMetaLoaded is called from BOTH the UI thread (CanEditFieldForIssue's callers
// trigger a warm) AND the background warm worker, so editMetaMutex_ is a genuine cross-thread
// guard. This case reproduces that contention shape: the background warm worker
// (WarmIssueEditMetaAsync → EnsureIssueEditMetaLoaded) takes editMetaMutex_ to insert the loaded
// entry while the main thread spins CanEditFieldForIssue for the SAME issue (which takes
// editMetaMutex_ to read the maps). TSan instruments this to catch any data race on the three
// editmeta containers.
//
// LaunchBackgroundTask runs on a REAL std::thread here (FakeEditMetaDeps::RunOnRealThread) so the
// worker and the reader genuinely overlap. Modelled on LocalCacheConcurrentSeed.test.cpp /
// BackendSwitchRace1081.test.cpp.
//
// ImGui/SQLite/cpr-free closure: EditMetaCacheService.cpp + its pure deps (GridLiveContext,
// ConfigManager, TrackerFieldValueUtils/Parser, ProjectResolver) — all already in the TSan list.

#include "../support/FakeEditMetaDeps.h"
#include "../support/FakeLookupCache.h"
#include "../support/FakeTrackerClient.h"

#include "CachedTicketTypes.h"
#include "EditMetaCacheService.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

using smatchet_tests::FakeEditMetaDeps;

namespace {

constexpr const char* kIssue = "ISSUE-1";
constexpr int kReaderSpins = 2000;

CachedTicket MakeTicket(const std::string& id, const std::string& issueType) {
    CachedTicket t;
    t.id = id;
    t.fieldValues["issuetype"] = issueType;
    return t;
}

} // namespace

TEST_CASE("EditMetaCacheService: concurrent WarmIssueEditMetaAsync worker vs CanEditFieldForIssue reader") {
    FakeEditMetaDeps deps;
    deps.RunOnRealThread = true; // warm body runs on its own std::thread → genuine overlap
    deps.ActiveTicketsImpl.push_back(MakeTicket(kIssue, "story"));

    // Scripted editmeta: the worker's EnsureIssueEditMetaLoaded will store this map under
    // editMetaMutex_ while the reader concurrently scans the same containers.
    std::unordered_map<std::string, bool> meta;
    meta["summary"] = true;
    meta["labels"] = false;
    deps.Fake()->SetIssueEditMetaSuccess(kIssue, meta);

    EditMetaCacheService svc(deps);

    // Reader thread: spin CanEditFieldForIssue for the same issue while the warm worker loads it.
    // Each call latches editMetaMutex_ to read issueEditMeta_ / issueTypeEditMeta_.
    std::atomic<int> readCount(0);
    std::thread reader([&] {
        for (int i = 0; i < kReaderSpins; ++i) {
            // Result legitimately changes from optimistic-true to the loaded value mid-run;
            // we assert no crash / no race, not a fixed value (the answer is timing-dependent).
            (void)svc.CanEditFieldForIssue(kIssue, "summary");
            (void)svc.CanEditFieldForIssue(kIssue, "labels");
            ++readCount;
        }
    });

    // Kick the background warm (real-thread mode → spawns the worker into LaunchedThreads).
    svc.WarmIssueEditMetaAsync(kIssue);

    reader.join();
    deps.JoinLaunchedThreads(); // join the warm worker before asserting final state

    CHECK(readCount.load() == kReaderSpins);

    // Consistent final state: the warm completed, so the issue is loaded and the denied field is
    // now enforced deterministically (all writers joined).
    CHECK(svc.CanEditFieldForIssue(kIssue, "summary"));
    CHECK_FALSE(svc.CanEditFieldForIssue(kIssue, "labels"));
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
}

TEST_CASE("EditMetaCacheService: Refresh, Invalidate, Ensure and readers race without losing the latch") {
    // Four threads hammer one issue: Refresh (supersedes), Invalidate (retires an in-flight fetch), Ensure and
    // WarmIssueEditMetaAsync (both refused while a fetch runs) and CanEditFieldForIssue (reads). The scope saves
    // run on real threads too. TSan watches the containers, the generation map and the save queue.
    FakeEditMetaDeps deps;
    deps.RunOnRealThread = true;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket(kIssue, "story"));
    deps.Fake()->SetIssueEditMetaSuccess(kIssue, {{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    constexpr int kRounds = 150;
    std::atomic<bool> start{false};
    const auto run = [&](const std::function<void(int)>& body) {
        return std::thread([&start, &body]() {
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kRounds; ++i) {
                body(i);
            }
        });
    };
    const std::function<void(int)> refresher = [&](int) {
        (void)svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", kIssue, "story");
    };
    const std::function<void(int)> invalidator = [&](int i) {
        svc.InvalidateIssueEditMeta(kIssue);
        if (i % 7 == 0) {
            svc.PruneEditMetaCacheToActiveTickets();
        }
    };
    const std::function<void(int)> ensurer = [&](int) {
        (void)svc.EnsureIssueEditMetaLoaded(kIssue);
        (void)svc.CanEditFieldForIssue(kIssue, "labels");
    };
    std::thread t1 = run(refresher);
    std::thread t2 = run(invalidator);
    std::thread t3 = run(ensurer);
    std::thread t4 = run([&](int) {
        svc.WarmIssueEditMetaAsync(kIssue); // launches real worker threads while the others race
        (void)svc.CanEditFieldForIssue(kIssue, "summary");
    });
    start.store(true);
    t1.join();
    t2.join();
    t3.join();
    t4.join();
    deps.JoinLaunchedThreads();
    REQUIRE(svc.FlushScopeSaves(std::chrono::seconds(10)));

    // Settled: nothing is stuck in flight, so a refresh now produces the scripted answer. Its scope save (if one is
    // due) runs inline, so no thread outlives the service.
    deps.RunOnRealThread = false;
    REQUIRE(svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", kIssue, "story").has_value());
    CHECK_FALSE(svc.CanEditFieldForIssue(kIssue, "labels"));
    CHECK(svc.CanEditFieldForIssue(kIssue, "summary"));
    CHECK(svc.FlushScopeSaves(std::chrono::seconds(10)));
}
