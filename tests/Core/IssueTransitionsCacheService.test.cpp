// IssueTransitionsCacheService bucket-A tests — the status combo's transitions lookup (Quality
// Pillar 6 offline-first): no fetch while offline, a failure backs off and retries after reconnect,
// every successful fetch is remembered per (project, issue type, from status), and the remembered
// workflow answers while the tracker is unreachable. Fakes only (FakeEditMetaDeps + FakeTrackerClient
// + FakeLookupCache): no AppController, ImGui, cpr or SQLite. The fake runs LaunchBackgroundTask inline
// unless RunOnRealThread is set (the TSan case at the end).

#include "../support/FakeEditMetaDeps.h"
#include "../support/FakeLookupCache.h"
#include "../support/FakeTrackerClient.h"
#include "../support/ScopedLimits.h"

#include "IssueTransitionsCacheService.h"
#include "LearnedWorkflowPure.h"
#include "OfflineFirstPure.h"
#include "TrackerError.h"
#include "Tracker/TrackerFieldSchema.h"
#include "Types/TransitionsTypes.h"

#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using smatchet::offline::DataFreshness;
using smatchet_tests::FakeEditMetaDeps;

namespace {

TrackerFieldOption Option(const std::string& id, const std::string& value) {
    TrackerFieldOption opt;
    opt.Id = id;
    opt.Value = value;
    return opt;
}

TransitionsQuery Query(const std::string& issueId, const std::string& fromStatus = "1") {
    TransitionsQuery q;
    q.IssueId = issueId;
    q.ProjectKey = "PROJ";
    q.IssueTypeKey = "bug";
    q.FromStatusKey = fromStatus;
    return q;
}

// A Jira-like fake: transitions supported, a lookup store installed, "PROJ-1" scripted with two targets.
std::shared_ptr<FakeLookupCache> SetUpJiraLike(FakeEditMetaDeps& deps) {
    deps.Fake()->SetSupportsIssueTransitions(true);
    deps.Fake()->SetIssueTransitions("PROJ-1", {Option("2", "In Progress"), Option("3", "Done")});
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    return store;
}

// Runs tasks inline but contains a throw the way AppController::LaunchBackgroundTask does (the task
// is abandoned, the app continues), so a test sees the production path where the service's own
// launch-time catch is never reached.
class FirewalledEditMetaDeps : public FakeEditMetaDeps {
  public:
    int FirewalledThrows = 0;
    void LaunchBackgroundTask(std::function<void()> task) override {
        ++LaunchBackgroundTaskCalls;
        try {
            task();
        } catch (const std::exception&) {
            ++FirewalledThrows; // abandoned like a production worker
        }
    }
};

// A lookup store whose first LoadLookups throws, then behaves normally.
class ThrowOnceLookupCache : public FakeLookupCache {
  public:
    std::vector<LookupCacheRow> LoadLookups(const std::string& backendKey, const std::string& kind) override {
        if (!thrown_) {
            thrown_ = true;
            throw std::runtime_error("scripted LoadLookups throw");
        }
        return FakeLookupCache::LoadLookups(backendKey, kind);
    }

  private:
    bool thrown_ = false;
};

// A store whose first LoadLookups runs a hook after it has read its rows and before it returns them: the window in
// which the saved-workflow load holds a row that another thread then makes obsolete.
class HookedLoadLookupCache : public FakeLookupCache {
  public:
    std::function<void()> AfterRead;
    std::vector<LookupCacheRow> LoadLookups(const std::string& backendKey, const std::string& kind) override {
        std::vector<LookupCacheRow> rows = FakeLookupCache::LoadLookups(backendKey, kind);
        if (AfterRead) {
            const std::function<void()> hook = std::move(AfterRead);
            AfterRead = nullptr;
            hook();
        }
        return rows;
    }
};

} // namespace

TEST_SUITE("IssueTransitionsCacheService") {

    TEST_CASE("offline: no fetch, still applicable, nothing to show") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 0);
        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(lookup.applicable);
        CHECK(lookup.options.empty());
        CHECK_FALSE(lookup.fromLearned);
        CHECK(lookup.freshness == DataFreshness::UnavailableNoCache);
    }

    TEST_CASE("online success: Fresh live options and the edge is remembered once") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); // already live: no second fetch

        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1);
        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(lookup.applicable);
        CHECK(lookup.freshness == DataFreshness::Fresh);
        CHECK_FALSE(lookup.fromLearned);
        CHECK(lookup.fromLive);
        REQUIRE(lookup.options.size() == 2);
        CHECK(lookup.options[0].Id == "2");

        CHECK(store->UpsertCalls.load() == 1);
        LookupCacheRow row;
        REQUIRE(store->TryGetLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|1", row));
        std::vector<TrackerFieldOption> stored;
        REQUIRE(smatchet::workflow::ParseTransitionTargets(row.PayloadJson, stored));
        CHECK(stored.size() == 2);
    }

    TEST_CASE("the remembered workflow answers another issue with the same project, type and status") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-2"));
        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-2"));

        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1); // PROJ-2 was never fetched
        CHECK(lookup.fromLearned);
        CHECK(lookup.freshness == DataFreshness::CachedOffline);
        CHECK(lookup.options.size() == 2);
        // A different from-status has no remembered edge.
        CHECK(svc.GetAvailableTransitions(Query("PROJ-2", "3")).options.empty());
    }

    TEST_CASE("a new service loads the stored workflow while offline (survives a restart)") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        {
            IssueTransitionsCacheService first(deps);
            first.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        }
        REQUIRE(store->UpsertCalls.load() == 1);

        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        deps.Fake()->ResetCalls();
        IssueTransitionsCacheService restarted(deps);
        restarted.EnsureIssueTransitionsLoaded(Query("PROJ-7"));
        const TransitionsLookup lookup = restarted.GetAvailableTransitions(Query("PROJ-7"));

        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 0);
        CHECK(lookup.fromLearned);
        CHECK(lookup.freshness == DataFreshness::CachedOffline);
        REQUIRE(lookup.options.size() == 2);
        CHECK(lookup.options[1].Value == "Done");
    }

    TEST_CASE("an issue type containing the key separator does not borrow another workflow") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("2", "In Progress")});
        IssueTransitionsCacheService svc(deps);
        TransitionsQuery learnedQ = Query("PROJ-1", "special|1");
        learnedQ.IssueTypeKey = "bug"; // remembered under ("PROJ", "bug", "special|1")
        svc.EnsureIssueTransitionsLoaded(learnedQ);

        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        TransitionsQuery otherQ = Query("PROJ-2", "1");
        otherQ.IssueTypeKey = "bug|special"; // same joined text before escaping
        CHECK(svc.GetAvailableTransitions(otherQ).options.empty());
        CHECK_FALSE(svc.GetAvailableTransitions(otherQ).fromLearned);
    }

    TEST_CASE("the stored workflow is namespaced per backend") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        {
            IssueTransitionsCacheService first(deps);
            first.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        }
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        deps.CacheBackendKeyImpl = "Jira:other-site";
        IssueTransitionsCacheService other(deps);
        other.EnsureIssueTransitionsLoaded(Query("PROJ-7"));
        CHECK(other.GetAvailableTransitions(Query("PROJ-7")).options.empty());
    }

    TEST_CASE("a transport failure backs off; connectivity recovery clears the backoff") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.Fake()->SetIssueTransitionsError(TrackerErrorTransport("connection refused", 0));
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-9")); // unscripted id -> the scripted Transport error
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1);
        const TransitionsLookup failed = svc.GetAvailableTransitions(Query("PROJ-9"));
        CHECK(failed.applicable);
        CHECK(failed.options.empty());
        CHECK(failed.freshness == DataFreshness::UnavailableNoCache); // not remembered as loaded

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-9"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1); // inside the backoff window

        svc.OnConnectivityRecovered();
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-9"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 2);
    }

    TEST_CASE("a failed refresh keeps showing the remembered workflow as stale") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.Fake()->SetIssueTransitionsError(TrackerErrorServer("HTTP 500", 500));
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); // learns PROJ|bug|1

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-9")); // same edge, fetch fails online
        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-9"));
        CHECK(lookup.fromLearned);
        CHECK(lookup.options.size() == 2);
        CHECK(lookup.freshness == DataFreshness::CachedStale);
    }

    TEST_CASE("a backend without transitions: no fetch, no store, not applicable") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        deps.Fake()->SetSupportsIssueTransitions(false);
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 0);
        CHECK(deps.LaunchBackgroundTaskCalls == 0);
        CHECK_FALSE(svc.GetAvailableTransitions(Query("PROJ-1")).applicable);
        CHECK(store->UpsertCalls.load() == 0);
    }

    TEST_CASE("no backend: not applicable and nothing launched") {
        FakeEditMetaDeps deps;
        deps.BackendImpl.reset();
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        CHECK(deps.LaunchBackgroundTaskCalls == 0);
        CHECK_FALSE(svc.GetAvailableTransitions(Query("PROJ-1")).applicable);
    }

    TEST_CASE("a throwing fetch never escapes, never stays in flight, and backs off") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.Fake()->SetIssueTransitionsThrows(true);
        IssueTransitionsCacheService svc(deps);

        CHECK_NOTHROW(svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")));

        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(lookup.freshness != DataFreshness::Refreshing);
        CHECK(lookup.freshness != DataFreshness::LoadingNoCache);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1);
    }

    TEST_CASE("a throw on a production-style worker still records the failure (no in-flight latch)") {
        FirewalledEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.Fake()->SetIssueTransitionsThrows(true);
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        CHECK(deps.FirewalledThrows == 1); // the worker died; the service's launch catch never ran
        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(lookup.freshness != DataFreshness::Refreshing);
        CHECK(lookup.freshness != DataFreshness::LoadingNoCache);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1); // backing off, not stuck in flight
        svc.OnConnectivityRecovered();
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 2);
    }

    TEST_CASE("a failed saved-workflow load backs off, then is retried") {
        FirewalledEditMetaDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        const auto store = std::make_shared<ThrowOnceLookupCache>();
        store->UpsertLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|1",
                            smatchet::workflow::SerializeTransitionTargets({Option("2", "In Progress")}));
        deps.LookupCacheImpl = store;
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); // the load worker throws
        CHECK(deps.FirewalledThrows == 1);
        CHECK(svc.GetAvailableTransitions(Query("PROJ-1")).options.empty());

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); // inside the backoff: no reload
        CHECK(svc.GetAvailableTransitions(Query("PROJ-1")).options.empty());

        svc.OnConnectivityRecovered();                     // clears the backoff
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); // loads again, succeeds
        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(lookup.fromLearned);
        CHECK(lookup.options.size() == 1);
    }

    TEST_CASE("a fetched list containing the from-status is not remembered") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        // The server's status moved to "2": its targets include our stale from-status "1".
        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("1", "To Do"), Option("3", "Done")});
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        // The targets belong to another status, so they are neither remembered nor served as this one's live set.
        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK_FALSE(lookup.fromLive);
        CHECK(lookup.options.empty());
        CHECK(lookup.freshness != DataFreshness::Fresh);
        CHECK(store->UpsertCalls.load() == 0);
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        CHECK(svc.GetAvailableTransitions(Query("PROJ-2")).options.empty());
    }

    TEST_CASE("a fetched list containing the from-status under another id (merged status) is not remembered") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        // The issue's "To Do" is this workflow's id "11"; the merged catalog row it resolved to keeps id "1".
        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("11", "to do"), Option("3", "Done")});
        IssueTransitionsCacheService svc(deps);
        TransitionsQuery q = Query("PROJ-1");
        q.FromStatusName = "To Do";

        svc.EnsureIssueTransitionsLoaded(q);

        CHECK_FALSE(svc.GetAvailableTransitions(q).fromLive);
        CHECK(svc.GetAvailableTransitions(q).options.empty());
        CHECK(store->UpsertCalls.load() == 0);
    }

    TEST_CASE("a from-status offered by a global or looped transition is remembered without itself") {
        // Every status of a team-managed workflow allows all: Jira lists the current status as a target.
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        TrackerFieldOption self = Option("1", "To Do");
        self.ReachableFromAnyStatus = true;
        deps.Fake()->SetIssueTransitions("PROJ-1", {self, Option("3", "Done")});
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        CHECK(store->UpsertCalls.load() == 1);
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        const TransitionsLookup offline = svc.GetAvailableTransitions(Query("PROJ-2"));
        CHECK(offline.fromLearned);
        REQUIRE(offline.options.size() == 1);
        CHECK(offline.options[0].Id == "3");
    }

    TEST_CASE("a fetch offering only a global self-target drops the edge learned before") {
        // An earlier fetch learned "1 -> 3". Now the only option from "1" is the global self-target, so
        // "3" is no longer reachable and must not be offered offline, from memory or from the saved row.
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        store->UpsertLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|1",
                            smatchet::workflow::SerializeTransitionTargets({Option("3", "Done")}));
        TrackerFieldOption self = Option("1", "To Do");
        self.ReachableFromAnyStatus = true;
        deps.Fake()->SetIssueTransitions("PROJ-1", {self});
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        LookupCacheRow row;
        CHECK_FALSE(store->TryGetLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|1", row));
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        const TransitionsLookup offline = svc.GetAvailableTransitions(Query("PROJ-2"));
        CHECK_FALSE(offline.fromLearned);
        CHECK(offline.options.empty());
    }

    TEST_CASE("a live list stays marked live once the tracker goes offline, even when empty") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.Fake()->SetIssueTransitions("PROJ-1", {}); // the tracker allows no move from here
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;

        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(lookup.fromLive);
        CHECK_FALSE(lookup.fromLearned);
        CHECK(lookup.options.empty());
        CHECK(lookup.freshness != DataFreshness::Fresh);
    }

    TEST_CASE("an empty live list is Fresh but not remembered") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        deps.Fake()->SetIssueTransitions("PROJ-1", {});
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(lookup.freshness == DataFreshness::Fresh);
        CHECK(lookup.options.empty());
        CHECK(store->UpsertCalls.load() == 0);
    }

    TEST_CASE("an incomplete query is fetched live but never remembered") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);
        TransitionsQuery q = Query("PROJ-1");
        q.IssueTypeKey.clear();

        svc.EnsureIssueTransitionsLoaded(q);

        CHECK(svc.GetAvailableTransitions(q).freshness == DataFreshness::Fresh);
        CHECK(store->UpsertCalls.load() == 0);
    }

    TEST_CASE("invalidation after a status change refetches on the next use") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        REQUIRE(deps.Fake()->FetchIssueTransitionsCalls() == 1);

        svc.InvalidateIssueTransitions("PROJ-1");
        CHECK_FALSE(svc.GetAvailableTransitions(Query("PROJ-1")).freshness == DataFreshness::Fresh);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 2);
    }

    TEST_CASE("invalidating another tracker namespace leaves this one alone; the named namespace is dropped") {
        FakeEditMetaDeps deps; // the focused pane's namespace is "Jira"
        SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        REQUIRE(deps.Fake()->FetchIssueTransitionsCalls() == 1);

        svc.InvalidateIssueTransitions("Plane", "PROJ-1"); // a pane on another tracker saw a remote change
        CHECK(svc.GetAvailableTransitions(Query("PROJ-1")).freshness == DataFreshness::Fresh);

        svc.InvalidateIssueTransitions("Jira", "PROJ-1");
        CHECK_FALSE(svc.GetAvailableTransitions(Query("PROJ-1")).freshness == DataFreshness::Fresh);
    }

    TEST_CASE("without a local store the live path still works") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.LookupCacheImpl.reset();
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        CHECK(svc.GetAvailableTransitions(Query("PROJ-1")).options.size() == 2);
    }

    TEST_CASE("real threads: lookups race the fetch and the stored-workflow load without a data race") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        store->UpsertLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|task|1",
                            smatchet::workflow::SerializeTransitionTargets({Option("4", "Blocked")}));
        deps.RunOnRealThread = true;
        IssueTransitionsCacheService svc(deps);

        TransitionsQuery taskQuery = Query("PROJ-5");
        taskQuery.IssueTypeKey = "task";
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        for (int i = 0; i < 200; ++i) {
            (void)svc.GetAvailableTransitions(Query("PROJ-1"));
            (void)svc.GetAvailableTransitions(taskQuery);
        }
        deps.JoinLaunchedThreads();

        CHECK(svc.GetAvailableTransitions(Query("PROJ-1")).freshness == DataFreshness::Fresh);
        CHECK(svc.GetAvailableTransitions(taskQuery).options.size() == 1);
    }

} // TEST_SUITE

TEST_SUITE("IssueTransitionsCacheService from-status and max age") {

    // Queues launched tasks so a test decides when they run (a fetch "in flight").
    class DeferredEditMetaDeps : public FakeEditMetaDeps {
      public:
        std::deque<std::function<void()>> Pending;
        void LaunchBackgroundTask(std::function<void()> task) override {
            ++LaunchBackgroundTaskCalls;
            Pending.push_back(std::move(task));
        }
        // Runs everything queued so far; a task that queues another (the saved-workflow load) is run too.
        void RunAll() {
            while (!Pending.empty()) {
                std::function<void()> task = std::move(Pending.front());
                Pending.pop_front();
                task();
            }
        }
    };

    TEST_CASE("a live set is served only for the from-status it was fetched for; another status refetches once") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "1"));
        REQUIRE(svc.GetAvailableTransitions(Query("PROJ-1", "1")).fromLive);

        // The row now shows status "2" (a sync saw the issue move): the set fetched for "1" is not its live set.
        const TransitionsLookup moved = svc.GetAvailableTransitions(Query("PROJ-1", "2"));
        CHECK_FALSE(moved.fromLive);
        CHECK(moved.options.empty());
        CHECK(moved.freshness != DataFreshness::Fresh);

        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("1", "To Do"), Option("4", "Review")});
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "2"));
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "2")); // the per-frame call: nothing more to fetch
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 2);
        const TransitionsLookup now = svc.GetAvailableTransitions(Query("PROJ-1", "2"));
        CHECK(now.fromLive);
        CHECK(now.freshness == DataFreshness::Fresh);
        REQUIRE(now.options.size() == 2u);
        CHECK(now.options[1].Id == "4");
        // and the old status is no longer the live one
        CHECK_FALSE(svc.GetAvailableTransitions(Query("PROJ-1", "1")).fromLive);
    }

    TEST_CASE("offline, a live set for another status is not shown; the remembered workflow for the new one is") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        store->UpsertLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|2",
                            smatchet::workflow::SerializeTransitionTargets({Option("5", "Closed")}));
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "1"));
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "2")); // offline: no fetch
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1);
        const TransitionsLookup lookup = svc.GetAvailableTransitions(Query("PROJ-1", "2"));
        CHECK_FALSE(lookup.fromLive);
        CHECK(lookup.fromLearned);
        REQUIRE(lookup.options.size() == 1u);
        CHECK(lookup.options[0].Id == "5");
        CHECK(lookup.freshness == DataFreshness::CachedOffline);
    }

    TEST_CASE("a fetch the issue had moved on from is never served live and is not refetched every frame") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("1", "To Do"), Option("3", "Done")});
        IssueTransitionsCacheService svc(deps);

        for (int frame = 0; frame < 20; ++frame) {
            svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "1"));
            CHECK_FALSE(svc.GetAvailableTransitions(Query("PROJ-1", "1")).fromLive);
        }
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1);

        // Once the issue's status is known to have changed (a sync or our own write invalidates), it refetches.
        svc.InvalidateIssueTransitions("PROJ-1");
        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("3", "Done")});
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "2"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 2);
        CHECK(svc.GetAvailableTransitions(Query("PROJ-1", "2")).fromLive);
    }

    TEST_CASE("a status edit in flight: the combo keeps asking every frame and only one fetch runs") {
        DeferredEditMetaDeps deps;
        SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "1"));
        deps.RunAll();
        REQUIRE(svc.GetAvailableTransitions(Query("PROJ-1", "1")).fromLive);

        // The optimistic row already shows "2"; the write's invalidation has not arrived yet.
        for (int frame = 0; frame < 20; ++frame) {
            svc.EnsureIssueTransitionsLoaded(Query("PROJ-1", "2"));
        }
        CHECK(deps.Pending.size() == 1u);
        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("3", "Done"), Option("4", "Review")});
        // While it runs the old set is not offered as the new status's: the combo shows no live options.
        const TransitionsLookup during = svc.GetAvailableTransitions(Query("PROJ-1", "2"));
        CHECK_FALSE(during.fromLive);
        deps.RunAll();
        CHECK(svc.GetAvailableTransitions(Query("PROJ-1", "2")).fromLive);
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 2);
    }

    TEST_CASE("a name resolving to an id after the first fetch costs one extra fetch, not one per frame") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);
        TransitionsQuery byName = Query("PROJ-1", "To Do"); // the row only knew the status by name
        svc.EnsureIssueTransitionsLoaded(byName);
        const TransitionsQuery byId = Query("PROJ-1", "1"); // the catalog resolved it to the id
        for (int frame = 0; frame < 10; ++frame) {
            svc.EnsureIssueTransitionsLoaded(byId);
        }
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 2);
        CHECK(svc.GetAvailableTransitions(byId).fromLive);
    }

    TEST_CASE("a live set past the max age is fetched again, stays on screen meanwhile, and keeps through a failure") {
        DeferredEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.AdvanceNow(std::chrono::seconds(0));
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        deps.RunAll();

        deps.AdvanceNow(std::chrono::seconds(1799));
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        CHECK(deps.Pending.empty());

        deps.AdvanceNow(std::chrono::seconds(1));
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        REQUIRE(deps.Pending.size() == 1u);
        const TransitionsLookup refreshing = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(refreshing.fromLive);
        CHECK(refreshing.freshness == DataFreshness::Refreshing);
        CHECK(refreshing.options.size() == 2u);

        // A scripted answer wins over a scripted error in the fake, so fail the fetch by throwing: the cache records
        // the failure (Unknown) and the worker's firewall would contain the rethrow.
        deps.Fake()->SetIssueTransitionsThrows(true);
        CHECK_THROWS_AS(deps.RunAll(), std::runtime_error);
        const TransitionsLookup failed = svc.GetAvailableTransitions(Query("PROJ-1"));
        CHECK(failed.fromLive);
        CHECK(failed.freshness == DataFreshness::CachedStale);
        CHECK(failed.options.size() == 2u);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); // inside the failure backoff
        CHECK(deps.Pending.empty());
    }

    TEST_CASE("a max age of 0 never revalidates a live set") {
        smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.LookupLiveMaxAgeSec = 0; });
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        deps.AdvanceNow(std::chrono::seconds(0));
        IssueTransitionsCacheService svc(deps);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        deps.AdvanceNow(std::chrono::hours(72));
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1);
    }

    TEST_CASE("an incomplete query keeps one live set per issue and does not loop") {
        FakeEditMetaDeps deps;
        SetUpJiraLike(deps);
        IssueTransitionsCacheService svc(deps);
        TransitionsQuery q = Query("PROJ-1", "");
        for (int frame = 0; frame < 5; ++frame) {
            svc.EnsureIssueTransitionsLoaded(q);
        }
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 1);
        CHECK(svc.GetAvailableTransitions(q).fromLive);
    }
}

TEST_SUITE("IssueTransitionsCacheService learned-edge tombstones") {

    TrackerFieldOption GlobalSelf() {
        TrackerFieldOption self = Option("1", "To Do");
        self.ReachableFromAnyStatus = true;
        return self;
    }

    TEST_CASE("an edge erased while the saved-workflow load holds its row is not brought back by the load") {
        FakeEditMetaDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        // The only option from "1" is the global self-target: the edge learned earlier ("1 -> 3") is obsolete.
        deps.Fake()->SetIssueTransitions("PROJ-1", {GlobalSelf()});
        const auto store = std::make_shared<HookedLoadLookupCache>();
        store->UpsertLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|1",
                            smatchet::workflow::SerializeTransitionTargets({Option("3", "Done")}));
        deps.LookupCacheImpl = store;
        IssueTransitionsCacheService svc(deps);
        // The load has read the row; before it applies it, a fetch for another issue in the same status erases the
        // edge.
        store->AfterRead = [&svc]() { svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); };

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-2"));

        LookupCacheRow row;
        CHECK_FALSE(store->TryGetLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|1", row));
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        const TransitionsLookup offline = svc.GetAvailableTransitions(Query("PROJ-3"));
        CHECK_FALSE(offline.fromLearned);
        CHECK(offline.options.empty());
    }

    TEST_CASE("an edge learned again after an erase is offered again") {
        FakeEditMetaDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        const auto store = std::make_shared<FakeLookupCache>();
        deps.LookupCacheImpl = store;
        IssueTransitionsCacheService svc(deps);

        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("3", "Done")});
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));
        deps.Fake()->SetIssueTransitions("PROJ-1", {GlobalSelf()});
        svc.InvalidateIssueTransitions("PROJ-1");
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); // erases "1 -> 3"
        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("5", "Closed")});
        svc.InvalidateIssueTransitions("PROJ-1");
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); // learns "1 -> 5"

        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        const TransitionsLookup offline = svc.GetAvailableTransitions(Query("PROJ-2"));
        CHECK(offline.fromLearned);
        REQUIRE(offline.options.size() == 1u);
        CHECK(offline.options[0].Id == "5");
    }

    TEST_CASE("an erase does not touch another status's edge") {
        FakeEditMetaDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        const auto store = std::make_shared<HookedLoadLookupCache>();
        store->UpsertLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|2",
                            smatchet::workflow::SerializeTransitionTargets({Option("4", "Review")}));
        store->UpsertLookup("Jira", smatchet::workflow::kLearnedTransitionsKind, "PROJ|bug|1",
                            smatchet::workflow::SerializeTransitionTargets({Option("3", "Done")}));
        deps.LookupCacheImpl = store;
        deps.Fake()->SetIssueTransitions("PROJ-1", {GlobalSelf()});
        IssueTransitionsCacheService svc(deps);
        store->AfterRead = [&svc]() { svc.EnsureIssueTransitionsLoaded(Query("PROJ-1")); };

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-2"));

        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        const TransitionsLookup other = svc.GetAvailableTransitions(Query("PROJ-3", "2"));
        CHECK(other.fromLearned);
        REQUIRE(other.options.size() == 1u);
        CHECK(other.options[0].Id == "4");
    }
}

TEST_SUITE("IssueTransitionsCacheService live map bound") {

    class DeferredPruneDeps : public FakeEditMetaDeps {
      public:
        std::deque<std::function<void()>> Pending;
        void LaunchBackgroundTask(std::function<void()> task) override {
            ++LaunchBackgroundTaskCalls;
            Pending.push_back(std::move(task));
        }
        void RunAll() {
            while (!Pending.empty()) {
                std::function<void()> task = std::move(Pending.front());
                Pending.pop_front();
                task();
            }
        }
    };

    std::string IssueId(int n) { return "PROJ-" + std::to_string(n); }

    CachedTicket TicketOf(const std::string& id) {
        CachedTicket t;
        t.id = id;
        return t;
    }

    // Loads a live set for PROJ-1..PROJ-n.
    void LoadIssues(FakeEditMetaDeps & deps, IssueTransitionsCacheService & svc, int n) {
        for (int i = 1; i <= n; ++i) {
            deps.Fake()->SetIssueTransitions(IssueId(i), {Option("2", "In Progress")});
            svc.EnsureIssueTransitionsLoaded(Query(IssueId(i)));
        }
    }

    TEST_CASE("under the cap a prune changes nothing, even for issues no pane lists") {
        smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.TransitionsLiveCacheMax = 16; });
        FakeEditMetaDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        IssueTransitionsCacheService svc(deps);
        LoadIssues(deps, svc, 16); // exactly at the cap
        deps.ActiveTicketsImpl = {TicketOf(IssueId(1))};

        svc.PruneLiveToActiveTickets();

        for (int i = 1; i <= 16; ++i) {
            CHECK(svc.GetAvailableTransitions(Query(IssueId(i))).fromLive);
        }
    }

    TEST_CASE("over the cap a prune keeps the issues a pane lists and drops the rest") {
        smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.TransitionsLiveCacheMax = 16; });
        FakeEditMetaDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        IssueTransitionsCacheService svc(deps);
        LoadIssues(deps, svc, 20);
        deps.ActiveTicketsImpl = {TicketOf(IssueId(3)), TicketOf(IssueId(17)), TicketOf("")};

        svc.PruneLiveToActiveTickets();

        for (int i = 1; i <= 20; ++i) {
            const bool listed = i == 3 || i == 17;
            CHECK(svc.GetAvailableTransitions(Query(IssueId(i))).fromLive == listed);
        }
        // A dropped issue is simply fetched again the next time its combo opens.
        svc.EnsureIssueTransitionsLoaded(Query(IssueId(5)));
        CHECK(svc.GetAvailableTransitions(Query(IssueId(5))).fromLive);
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 21u);
    }

    TEST_CASE("a set being fetched survives a prune and its result lands") {
        smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.TransitionsLiveCacheMax = 16; });
        DeferredPruneDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        IssueTransitionsCacheService svc(deps);
        for (int i = 1; i <= 20; ++i) {
            deps.Fake()->SetIssueTransitions(IssueId(i), {Option("2", "In Progress")});
            svc.EnsureIssueTransitionsLoaded(Query(IssueId(i)));
        }
        deps.RunAll();
        // PROJ-21 starts a fetch that is still running when the prune comes.
        deps.Fake()->SetIssueTransitions(IssueId(21), {Option("2", "In Progress")});
        svc.EnsureIssueTransitionsLoaded(Query(IssueId(21)));
        REQUIRE(deps.Pending.size() == 1u);
        deps.ActiveTicketsImpl = {TicketOf(IssueId(1))};

        svc.PruneLiveToActiveTickets();
        deps.RunAll();

        CHECK(svc.GetAvailableTransitions(Query(IssueId(21))).fromLive);
        CHECK(svc.GetAvailableTransitions(Query(IssueId(1))).fromLive);
        CHECK_FALSE(svc.GetAvailableTransitions(Query(IssueId(2))).fromLive);
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == 21u); // the prune started no fetch
    }

    TEST_CASE("a failed fetch's backoff survives a prune, so the prune cannot start a retry storm") {
        smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.TransitionsLiveCacheMax = 16; });
        FakeEditMetaDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        deps.AdvanceNow(std::chrono::seconds(0));
        IssueTransitionsCacheService svc(deps);
        LoadIssues(deps, svc, 20);
        // PROJ-99 is listed by a pane but its fetch fails (a throw is recorded as a failure).
        deps.Fake()->SetIssueTransitionsThrows(true);
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-99"));
        deps.Fake()->SetIssueTransitionsThrows(false);
        deps.Fake()->SetIssueTransitions("PROJ-99", {Option("2", "In Progress")});
        deps.ActiveTicketsImpl = {TicketOf("PROJ-99")};
        const std::size_t callsBefore = deps.Fake()->FetchIssueTransitionsCalls();

        svc.PruneLiveToActiveTickets();
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-99")); // inside the 30 s backoff
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == callsBefore);

        deps.AdvanceNow(std::chrono::seconds(30));
        svc.EnsureIssueTransitionsLoaded(Query("PROJ-99"));
        CHECK(deps.Fake()->FetchIssueTransitionsCalls() == callsBefore + 1);
    }

    TEST_CASE("the union over every pane is kept") {
        smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.TransitionsLiveCacheMax = 16; });
        class TwoPaneDeps : public FakeEditMetaDeps {
          public:
            std::vector<std::shared_ptr<const std::vector<CachedTicket>>>
            GetActiveTicketsSnapshotsAllPanes() const override {
                std::vector<std::shared_ptr<const std::vector<CachedTicket>>> panes;
                panes.push_back(
                    std::make_shared<const std::vector<CachedTicket>>(std::vector<CachedTicket>{TicketOf("PROJ-2")}));
                panes.push_back(nullptr); // a pane without a published roster
                panes.push_back(
                    std::make_shared<const std::vector<CachedTicket>>(std::vector<CachedTicket>{TicketOf("PROJ-19")}));
                return panes;
            }
        };
        TwoPaneDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        IssueTransitionsCacheService svc(deps);
        LoadIssues(deps, svc, 20);

        svc.PruneLiveToActiveTickets();

        CHECK(svc.GetAvailableTransitions(Query("PROJ-2")).fromLive);
        CHECK(svc.GetAvailableTransitions(Query("PROJ-19")).fromLive);
        CHECK_FALSE(svc.GetAvailableTransitions(Query("PROJ-10")).fromLive);
    }

    TEST_CASE("real threads: prune races fetches and lookups without a data race") {
        smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.TransitionsLiveCacheMax = 16; });
        FakeEditMetaDeps deps;
        deps.Fake()->SetSupportsIssueTransitions(true);
        for (int i = 1; i <= 64; ++i) {
            deps.Fake()->SetIssueTransitions(IssueId(i), {Option("2", "In Progress")});
        }
        deps.ActiveTicketsImpl = {TicketOf(IssueId(1)), TicketOf(IssueId(2))};
        deps.RunOnRealThread = true;
        IssueTransitionsCacheService svc(deps);

        std::thread pruner([&svc]() {
            for (int i = 0; i < 200; ++i) {
                svc.PruneLiveToActiveTickets();
            }
        });
        for (int round = 0; round < 3; ++round) {
            for (int i = 1; i <= 64; ++i) {
                svc.EnsureIssueTransitionsLoaded(Query(IssueId(i)));
                (void)svc.GetAvailableTransitions(Query(IssueId(i)));
            }
        }
        pruner.join();
        deps.JoinLaunchedThreads();
        svc.PruneLiveToActiveTickets();

        CHECK_FALSE(svc.GetAvailableTransitions(Query(IssueId(40))).fromLive);
    }
}
