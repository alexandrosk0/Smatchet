// ProjectComponentsCacheService bucket-A tests (Quality Pillar 6): the per-project component options
// behind the grid's components editor, cached per (tracker, project), saved to the lookup cache and
// offered offline. Driven through FakeEditMetaDeps + FakeLookupCache, so no AppController, ImGui, cpr
// or SQLite is touched. Also covers ComponentOptionsPickPure (which list a row's editor uses).

#include "../support/FakeEditMetaDeps.h"
#include "../support/FakeLookupCache.h"
#include "../support/FakeTrackerClient.h"

#include "CachedTicketTypes.h"
#include "ComponentOptionsPickPure.h"
#include "FieldOptionsJsonPure.h"
#include "LookupPayloadsPure.h"
#include "OfflineFirstPure.h"
#include "ProjectComponentsCacheService.h"
#include "Types/ProjectComponentsTypes.h"

#include <doctest/doctest.h>

#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using smatchet::offline::DataFreshness;
using smatchet_tests::FakeEditMetaDeps;

namespace {

TrackerFieldOption Component(const std::string& id, const std::string& name) {
    TrackerFieldOption option;
    option.Id = id;
    option.Value = name;
    option.SecondaryValue = name + " team";
    option.PayloadJson = R"({"id":")" + id + R"(","name":")" + name + R"("})";
    return option;
}

CachedTicket Ticket(const std::string& id) {
    CachedTicket ticket;
    ticket.id = id;
    return ticket;
}

void SaveComponents(FakeLookupCache& store, const std::string& backendKey, const std::string& projectKey,
                    const std::vector<TrackerFieldOption>& options) {
    store.UpsertLookup(backendKey, smatchet::lookup::kProjectComponentsKind, projectKey,
                       smatchet::fieldoptions::SerializeFieldOptions(options));
}

// Queues launched tasks so a test decides when (and in which order) they run.
class DeferredLaunchDeps : public FakeEditMetaDeps {
  public:
    std::deque<std::function<void()>> Pending;
    void LaunchBackgroundTask(std::function<void()> task) override {
        ++LaunchBackgroundTaskCalls;
        Pending.push_back(std::move(task));
    }
    void RunFront() {
        std::function<void()> task = std::move(Pending.front());
        Pending.pop_front();
        task();
    }
    void RunBack() {
        std::function<void()> task = std::move(Pending.back());
        Pending.pop_back();
        task();
    }
};

// Every launch throws, as std::thread creation does under resource exhaustion.
class ThrowingLaunchDeps : public FakeEditMetaDeps {
  public:
    void LaunchBackgroundTask(std::function<void()> /*task*/) override {
        ++LaunchBackgroundTaskCalls;
        throw std::runtime_error("thread creation failed");
    }
};

// A store whose writes throw, as a failing disk would if an implementation did not catch.
class ThrowingWriteLookupCache : public FakeLookupCache {
  public:
    bool UpsertLookup(const std::string&, const std::string&, const std::string&, const std::string&) override {
        throw std::runtime_error("scripted UpsertLookup throw");
    }
};

} // namespace

TEST_SUITE("ProjectComponentsCacheService") {

    TEST_CASE("offline: no fetch, and the saved list is offered with an offline cue") {
        FakeEditMetaDeps deps;
        const auto store = std::make_shared<FakeLookupCache>();
        SaveComponents(*store, "Jira", "ABC", {Component("100", "Backend")});
        deps.LookupCacheImpl = store;
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        ProjectComponentsCacheService svc(deps);

        CHECK_FALSE(svc.EnsureComponentsLoaded("ABC"));

        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 0u);
        const ProjectComponentsLookup lookup = svc.GetComponentOptions("ABC");
        REQUIRE(lookup.options != nullptr);
        REQUIRE(lookup.options->size() == 1u);
        CHECK(lookup.options->at(0).Value == "Backend");
        CHECK(lookup.options->at(0).PayloadJson == Component("100", "Backend").PayloadJson);
        CHECK(lookup.freshness == DataFreshness::CachedOffline);
        // A project with nothing saved says so instead of spinning.
        CHECK(svc.GetComponentOptions("XYZ").freshness == DataFreshness::UnavailableNoCache);
    }

    TEST_CASE("a live fetch is Fresh and saved for the next session") {
        FakeEditMetaDeps deps;
        const auto store = std::make_shared<FakeLookupCache>();
        deps.LookupCacheImpl = store;
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("100", "Backend"), Component("101", "Frontend")});
        ProjectComponentsCacheService svc(deps);

        CHECK(svc.EnsureComponentsLoaded("ABC"));
        CHECK_FALSE(svc.EnsureComponentsLoaded("ABC")); // live: never refetched this session

        const ProjectComponentsLookup lookup = svc.GetComponentOptions("ABC");
        CHECK(lookup.freshness == DataFreshness::Fresh);
        REQUIRE(lookup.options != nullptr);
        CHECK(lookup.options->size() == 2u);
        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 1u);

        LookupCacheRow row;
        REQUIRE(store->TryGetLookup("Jira", smatchet::lookup::kProjectComponentsKind, "ABC", row));
        std::vector<TrackerFieldOption> saved;
        REQUIRE(smatchet::fieldoptions::ParseFieldOptions(row.PayloadJson, saved));
        REQUIRE(saved.size() == 2u);
        CHECK(saved[1].Value == "Frontend");
        CHECK(saved[1].SecondaryValue == "Frontend team");

        // A new session (fresh service) offline offers what the first one saved.
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        ProjectComponentsCacheService nextSession(deps);
        nextSession.EnsureComponentsLoaded("ABC");
        const ProjectComponentsLookup restored = nextSession.GetComponentOptions("ABC");
        REQUIRE(restored.options != nullptr);
        CHECK(restored.options->size() == 2u);
        CHECK(restored.freshness == DataFreshness::CachedOffline);
        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 1u);
    }

    TEST_CASE("a project with no components settles to an empty live list, not a spinner") {
        FakeEditMetaDeps deps;
        deps.Fake()->SetProjectComponentsSuccess("ABC", {});
        ProjectComponentsCacheService svc(deps);

        svc.EnsureComponentsLoaded("ABC");

        const ProjectComponentsLookup lookup = svc.GetComponentOptions("ABC");
        CHECK(lookup.freshness == DataFreshness::Fresh);
        REQUIRE(lookup.options != nullptr);
        CHECK(lookup.options->empty());
        CHECK_FALSE(svc.EnsureComponentsLoaded("ABC"));
    }

    TEST_CASE("a failed fetch keeps the saved list, backs off, and retries after reconnect") {
        FakeEditMetaDeps deps;
        const auto store = std::make_shared<FakeLookupCache>();
        SaveComponents(*store, "Jira", "ABC", {Component("100", "Backend")});
        deps.LookupCacheImpl = store;
        deps.Fake()->SetProjectComponentsFailure("ABC", "HTTP 503");
        ProjectComponentsCacheService svc(deps);

        CHECK(svc.EnsureComponentsLoaded("ABC"));
        const ProjectComponentsLookup lookup = svc.GetComponentOptions("ABC");
        CHECK(lookup.freshness == DataFreshness::CachedStale);
        CHECK(lookup.lastAttemptFailed);
        CHECK_FALSE(lookup.lastError.empty());
        REQUIRE(lookup.options != nullptr);
        CHECK(lookup.options->at(0).Value == "Backend");

        CHECK_FALSE(svc.EnsureComponentsLoaded("ABC")); // inside the backoff
        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 1u);

        svc.OnConnectivityRecovered();
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("200", "Platform")});
        CHECK(svc.EnsureComponentsLoaded("ABC"));
        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 2u);
        CHECK(svc.GetComponentOptions("ABC").options->at(0).Value == "Platform");
    }

    TEST_CASE("a store failure never costs the live list") {
        FakeEditMetaDeps deps;
        deps.LookupCacheImpl = std::make_shared<ThrowingWriteLookupCache>();
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("100", "Backend")});
        ProjectComponentsCacheService svc(deps);

        CHECK_NOTHROW(svc.EnsureComponentsLoaded("ABC"));

        CHECK(svc.GetComponentOptions("ABC").freshness == DataFreshness::Fresh);
    }

    TEST_CASE("the warm fetches each active project once and skips live ones") {
        FakeEditMetaDeps deps;
        deps.ActiveTicketsImpl = {Ticket("ABC-1"), Ticket("ABC-2"), Ticket("XYZ-9"), Ticket("")};
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("100", "Backend")});
        deps.Fake()->SetProjectComponentsSuccess("XYZ", {Component("200", "Frontend")});
        ProjectComponentsCacheService svc(deps);
        TrackerConfig cfg;

        svc.WarmForActiveTicketsAsync(cfg);
        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 2u);
        CHECK(svc.GetComponentOptions("ABC").freshness == DataFreshness::Fresh);
        CHECK(svc.GetComponentOptions("XYZ").options->at(0).Value == "Frontend");

        svc.WarmForActiveTicketsAsync(cfg);
        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 2u);
    }

    TEST_CASE("the warm skips every project while offline") {
        FakeEditMetaDeps deps;
        deps.ActiveTicketsImpl = {Ticket("ABC-1")};
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("100", "Backend")});
        ProjectComponentsCacheService svc(deps);
        TrackerConfig cfg;

        svc.WarmForActiveTicketsAsync(cfg);

        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 0u);
        CHECK_FALSE(svc.GetComponentOptions("ABC").inFlight);
    }

    TEST_CASE("two trackers keep separate lists for the same project key") {
        FakeEditMetaDeps deps;
        const auto store = std::make_shared<FakeLookupCache>();
        deps.LookupCacheImpl = store;
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("100", "Backend")});
        ProjectComponentsCacheService svc(deps);

        svc.EnsureComponentsLoaded("ABC");
        deps.CacheBackendKeyImpl = "Jira:other.example";

        CHECK(svc.GetComponentOptions("ABC").options == nullptr);
        LookupCacheRow row;
        CHECK_FALSE(store->TryGetLookup("Jira:other.example", smatchet::lookup::kProjectComponentsKind, "ABC", row));
    }

    TEST_CASE("a focus switch mid-fetch cannot strand the project on Loading (#975 class)") {
        DeferredLaunchDeps deps;
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("100", "Backend")});
        ProjectComponentsCacheService svc(deps);

        REQUIRE(svc.EnsureComponentsLoaded("ABC"));
        CHECK(svc.GetComponentOptions("ABC").freshness == DataFreshness::LoadingNoCache);

        // Focus moves to a pane on another tracker before the worker runs; the fetch still lands on
        // the tracker that started it, and the other tracker's entry is untouched.
        deps.CacheBackendKeyImpl = "Jira:other.example";
        deps.RunFront();
        CHECK(svc.GetComponentOptions("ABC").freshness == DataFreshness::UnavailableNoCache);

        deps.CacheBackendKeyImpl = "Jira";
        const ProjectComponentsLookup lookup = svc.GetComponentOptions("ABC");
        CHECK(lookup.freshness == DataFreshness::Fresh);
        CHECK_FALSE(lookup.inFlight);
    }

    TEST_CASE("the saved list never overrides a list fetched live meanwhile") {
        DeferredLaunchDeps deps;
        const auto store = std::make_shared<FakeLookupCache>();
        SaveComponents(*store, "Jira", "ABC", {Component("100", "Old name")});
        deps.LookupCacheImpl = store;
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("100", "Backend")});
        ProjectComponentsCacheService svc(deps);

        REQUIRE(svc.EnsureComponentsLoaded("ABC"));
        REQUIRE(deps.Pending.size() == 2u); // [load saved rows, live fetch]
        deps.RunBack();                     // the live fetch lands first
        deps.RunFront();                    // then the saved rows

        const ProjectComponentsLookup lookup = svc.GetComponentOptions("ABC");
        CHECK(lookup.freshness == DataFreshness::Fresh);
        CHECK(lookup.options->at(0).Value == "Backend");
    }

    TEST_CASE("a launch that throws records a failure instead of staying in flight") {
        ThrowingLaunchDeps deps;
        deps.LookupCacheImpl = std::make_shared<FakeLookupCache>();
        deps.Fake()->SetProjectComponentsSuccess("ABC", {Component("100", "Backend")});
        ProjectComponentsCacheService svc(deps);

        CHECK(svc.EnsureComponentsLoaded("ABC")); // the entry changed: re-read it

        const ProjectComponentsLookup lookup = svc.GetComponentOptions("ABC");
        CHECK_FALSE(lookup.inFlight);
        CHECK(lookup.lastAttemptFailed);
        CHECK(lookup.freshness == DataFreshness::UnavailableNoCache);
        CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 0u);
        CHECK_FALSE(svc.EnsureComponentsLoaded("ABC")); // backing off, not relaunching every frame
    }
}

TEST_SUITE("ComponentOptionsPickPure") {

    TEST_CASE("the row's own list wins, live or saved") {
        ProjectComponentsLookup lookup;
        lookup.options = std::make_shared<const std::vector<TrackerFieldOption>>(
            std::vector<TrackerFieldOption>{Component("100", "Backend")});
        lookup.freshness = DataFreshness::CachedOffline;
        const std::vector<TrackerFieldOption> catalog{Component("900", "Other project")};

        const smatchet::components::ComponentOptionsPick pick =
            smatchet::components::PickComponentOptions(lookup, true, catalog, TrackerConnectivityState::TransportDown);

        REQUIRE(pick.Options->size() == 1u);
        CHECK(pick.Options->at(0).Value == "Backend");
        CHECK(pick.Freshness == DataFreshness::CachedOffline);
        CHECK_FALSE(pick.FromCatalog);
    }

    TEST_CASE("the catalog stands in only when it was fetched for the row's project") {
        ProjectComponentsLookup lookup; // nothing cached for the row's project
        const std::vector<TrackerFieldOption> catalog{Component("100", "Backend")};

        const auto sameProject =
            smatchet::components::PickComponentOptions(lookup, true, catalog, TrackerConnectivityState::TransportDown);
        CHECK(sameProject.FromCatalog);
        CHECK(sameProject.Options == &catalog);
        CHECK(sameProject.Freshness == DataFreshness::CachedOffline);

        const auto otherProject =
            smatchet::components::PickComponentOptions(lookup, false, catalog, TrackerConnectivityState::TransportDown);
        CHECK_FALSE(otherProject.FromCatalog);
        CHECK(otherProject.Options->empty());
        CHECK(otherProject.Freshness == DataFreshness::UnavailableNoCache);
    }

    TEST_CASE("a catalog stand-in reports the project's fetch state") {
        ProjectComponentsLookup lookup;
        lookup.inFlight = true;
        lookup.freshness = DataFreshness::LoadingNoCache;
        const std::vector<TrackerFieldOption> catalog{Component("100", "Backend")};

        const auto pick = smatchet::components::PickComponentOptions(lookup, true, catalog,
                                                                     TrackerConnectivityState::AuthenticatedReachable);
        CHECK(pick.Freshness == DataFreshness::Refreshing);

        // An empty catalog list is no stand-in: the loading state shows instead of an empty combo.
        const std::vector<TrackerFieldOption> emptyCatalog;
        const auto none = smatchet::components::PickComponentOptions(lookup, true, emptyCatalog,
                                                                     TrackerConnectivityState::AuthenticatedReachable);
        CHECK(none.Options->empty());
        CHECK(none.Freshness == DataFreshness::LoadingNoCache);
    }
}
