// IssueTransitionsCacheService bucket-A tests — the status combo's transitions lookup (Quality
// Pillar 6 offline-first): no fetch while offline, a failure backs off and retries after reconnect,
// every successful fetch is remembered per (project, issue type, from status), and the remembered
// workflow answers while the tracker is unreachable. Fakes only (FakeEditMetaDeps + FakeTrackerClient
// + FakeLookupCache): no AppController, ImGui, cpr or SQLite. The fake runs LaunchBackgroundTask inline
// unless RunOnRealThread is set (the TSan case at the end).

#include "../support/FakeEditMetaDeps.h"
#include "../support/FakeLookupCache.h"
#include "../support/FakeTrackerClient.h"

#include "IssueTransitionsCacheService.h"
#include "LearnedWorkflowPure.h"
#include "OfflineFirstPure.h"
#include "Tracker/TrackerError.h"
#include "Tracker/TrackerFieldSchema.h"
#include "Types/TransitionsTypes.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
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

    TEST_CASE("a fetched list containing the from-status is not remembered") {
        FakeEditMetaDeps deps;
        const auto store = SetUpJiraLike(deps);
        // The server's status moved to "2": its targets include our stale from-status "1".
        deps.Fake()->SetIssueTransitions("PROJ-1", {Option("1", "To Do"), Option("3", "Done")});
        IssueTransitionsCacheService svc(deps);

        svc.EnsureIssueTransitionsLoaded(Query("PROJ-1"));

        CHECK(svc.GetAvailableTransitions(Query("PROJ-1")).freshness == DataFreshness::Fresh);
        CHECK(store->UpsertCalls.load() == 0);
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        CHECK(svc.GetAvailableTransitions(Query("PROJ-2")).options.empty());
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
