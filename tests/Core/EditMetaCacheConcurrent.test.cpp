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
#include "../support/FakeTrackerClient.h"

#include "CachedTicketTypes.h"
#include "EditMetaCacheService.h"

#include <doctest/doctest.h>

#include <atomic>
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
