// EditMetaCacheService bucket-A tests — exercise the per-issue + per-issue-type tracker
// edit-metadata cache extracted from AppController (god-object decomposition Phase 1) through
// the IEditMetaDeps interface bundle. Tests drive the service against a FakeEditMetaDeps fixture
// (header-only: shared_ptr<FakeTrackerClient> backend + plain-member active-tickets / catalog /
// shutdown state) so no AppController, ImGui, cpr, HTTP, or SQLite surface is touched.
//
// The fixture's LaunchBackgroundTask runs inline-synchronously by default, so the async warm
// methods complete on the calling thread — these cases stay deterministic and single-threaded.
// The real-thread mode + the WarmIssueEditMetaAsync vs CanEditFieldForIssue race live in the
// sibling EditMetaCacheConcurrent.test.cpp (TSan target).
//
// Per-case isolation: every TEST_CASE constructs its own FakeEditMetaDeps + fresh
// EditMetaCacheService. No statics, no shared world state, no order dependencies.

#include "../support/FakeEditMetaDeps.h"
#include "../support/FakeLookupCache.h"
#include "../support/FakeTrackerClient.h"

#include "CachedTicketTypes.h"
#include "EditMetaCacheService.h"
#include "LookupPayloadsPure.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using smatchet_tests::FakeEditMetaDeps;
using smatchet_tests::FakeTrackerClient;

namespace {

CachedTicket MakeTicket(const std::string& id, const std::string& issueType = "story") {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = "summary";
    t.fieldValues["issuetype"] = issueType;
    return t;
}

// The first launch throws, as std::thread creation does under resource exhaustion; later launches
// run inline like the base fixture.
class FirstLaunchThrowsEditMetaDeps : public FakeEditMetaDeps {
  public:
    bool FailNextLaunch = true;
    void LaunchBackgroundTask(std::function<void()> task) override {
        if (FailNextLaunch) {
            FailNextLaunch = false;
            ++LaunchBackgroundTaskCalls;
            throw std::runtime_error("thread creation failed");
        }
        FakeEditMetaDeps::LaunchBackgroundTask(std::move(task));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// CanEditFieldForIssue — the optimistic / carve-out / loaded-deny matrix.
// ---------------------------------------------------------------------------

TEST_CASE("EditMetaCacheService::CanEditFieldForIssue returns true (optimistic) when editmeta is unloaded") {
    FakeEditMetaDeps deps;
    EditMetaCacheService svc(deps);
    // Nothing loaded yet, no issuetype-level cache → optimistic allow.
    CHECK(svc.CanEditFieldForIssue("ABC-1", "summary"));
    CHECK(svc.CanEditFieldForIssue("ABC-1", "assignee"));
}

TEST_CASE("EditMetaCacheService::CanEditFieldForIssue treats `status` as always editable (transition path)") {
    FakeEditMetaDeps deps;
    EditMetaCacheService svc(deps);
    // Even with a loaded map that omits/denies status, status is special-cased true (Jira
    // applies status via transitions, not editmeta) — this short-circuits before any map read.
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}});
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK(svc.CanEditFieldForIssue("ABC-1", "status"));
    CHECK(svc.CanEditFieldForIssue("ABC-1", "Status")); // case-folded fieldKey == "status"
}

TEST_CASE("EditMetaCacheService::CanEditFieldForIssue force-allows priority/components omitted from a loaded map") {
    FakeEditMetaDeps deps;
    EditMetaCacheService svc(deps);
    // Loaded editmeta lists only summary — Jira often omits priority (Epics) and components
    // (cross-project / filter-id views) while PUT still accepts them: force-editable carve-out.
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}});
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());

    CHECK(svc.CanEditFieldForIssue("ABC-1", "priority"));
    CHECK(svc.CanEditFieldForIssue("ABC-1", "components"));
    // A different absent field is denied (proves the carve-out is field-specific, not blanket).
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));
}

TEST_CASE("EditMetaCacheService::CanEditFieldForIssue returns true for sprint + editable timetracking fields") {
    FakeEditMetaDeps deps;
    EditMetaCacheService svc(deps);

    SUBCASE("editable timetracking estimate short-circuits before any map read") {
        deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}}); // omits timeoriginalestimate
        REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
        CHECK(svc.CanEditFieldForIssue("ABC-1", "timeoriginalestimate"));
        CHECK(svc.CanEditFieldForIssue("ABC-1", "timeestimate"));
    }

    SUBCASE("a sprint catalog field is always editable via FindFieldById") {
        TrackerField sprint;
        sprint.Id = "customfield_10020";
        sprint.Name = "Sprint";
        sprint.Family = TrackerFieldFamily::Sprint;
        deps.AvailableFieldsImpl.push_back(sprint);
        deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}}); // omits the sprint field
        REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
        CHECK(svc.CanEditFieldForIssue("ABC-1", "customfield_10020"));
    }
}

TEST_CASE("EditMetaCacheService::CanEditFieldForIssue denies an absent field once editmeta is loaded") {
    FakeEditMetaDeps deps;
    EditMetaCacheService svc(deps);
    // Loaded map explicitly allows summary, denies description, and omits labels.
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}, {"description", false}});
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());

    CHECK(svc.CanEditFieldForIssue("ABC-1", "summary"));           // present + true
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "description")); // present + false
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));      // absent, not a carve-out
}

TEST_CASE("EditMetaCacheService::CanEditFieldForIssue falls back to the issuetype-level cache for an unloaded issue") {
    FakeEditMetaDeps deps;
    // The issue's per-issue cache is NOT loaded, but its issuetype ("bug") is — resolved from
    // the active-tickets snapshot. The type map should drive the answer.
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "bug"));
    EditMetaCacheService svc(deps);

    // Load the issuetype-level cache by loading a sibling issue of the same type, then prune the
    // per-issue entry so only the type-level cache remains.
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"description", false}});
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    svc.InvalidateIssueEditMeta("ABC-1"); // drop per-issue; type-level "bug" survives

    CHECK(svc.CanEditFieldForIssue("ABC-1", "summary"));           // from type map
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "description")); // from type map
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));      // absent in type map, not carve-out
    // Carve-out still applies at the type level.
    CHECK(svc.CanEditFieldForIssue("ABC-1", "priority"));
}

// ---------------------------------------------------------------------------
// EnsureIssueEditMetaLoaded — success marks loaded + arms notify; failure stays optimistic.
// ---------------------------------------------------------------------------

TEST_CASE("EditMetaCacheService::EnsureIssueEditMetaLoaded marks loaded + arms deferred-notify only on success") {
    FakeEditMetaDeps deps;
    EditMetaCacheService svc(deps);
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}, {"labels", false}});

    const VoidResult r = svc.EnsureIssueEditMetaLoaded("ABC-1");
    CHECK(r.has_value());                 // Ok on success
    CHECK(deps.DeferredNotifyCalls == 1); // success counts as a reachable-backend signal
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);

    // Loaded now: the denied field is enforced.
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));

    // Second call is a cache hit — no refetch, no extra notify.
    CHECK(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
    CHECK(deps.DeferredNotifyCalls == 1);
}

TEST_CASE("EditMetaCacheService::EnsureIssueEditMetaLoaded on fetch failure stays optimistic + reports Err") {
    FakeEditMetaDeps deps;
    EditMetaCacheService svc(deps);
    deps.Fake()->SetIssueEditMetaFailure("ABC-1", "HTTP 503: backend unreachable");

    const VoidResult r = svc.EnsureIssueEditMetaLoaded("ABC-1");
    CHECK_FALSE(r.has_value());
    CHECK(r.error() == "HTTP 503: backend unreachable");
    CHECK(deps.DeferredNotifyCalls == 0); // a failed fetch is NOT a reachable-backend signal

    // The empty-but-loaded bug regression: a failed fetch must leave the issue optimistic, not
    // deny every field. (Pre-fix it stored loaded=true with an empty map → all-deny offline.)
    CHECK(svc.CanEditFieldForIssue("ABC-1", "summary"));
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels"));

    // A later successful retry flips it to loaded + arms notify.
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}});
    CHECK(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK(deps.DeferredNotifyCalls == 1);
}

// ---------------------------------------------------------------------------
// RefreshIssueEditMeta — invalidate then refetch.
// ---------------------------------------------------------------------------

TEST_CASE("EditMetaCacheService::RefreshIssueEditMeta invalidates the cached entry and refetches") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    EditMetaCacheService svc(deps);

    // First load: labels denied.
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}, {"labels", false}});
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);

    // Backend now allows labels — Refresh must drop the stale entry and refetch.
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}, {"labels", true}});
    CHECK(svc.RefreshIssueEditMeta("ABC-1").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u); // a genuine refetch happened
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels"));      // new value reflected
}

// ---------------------------------------------------------------------------
// PruneEditMetaCacheToActiveTickets — drop entries absent from the active set.
// ---------------------------------------------------------------------------

TEST_CASE("EditMetaCacheService::PruneEditMetaCacheToActiveTickets drops absent issues + types, keeps present") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("KEEP-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("DROP-1", "bug"));
    EditMetaCacheService svc(deps);

    // Load both — denying labels so the cached state is observable post-prune.
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    REQUIRE(svc.EnsureIssueEditMetaLoaded("KEEP-1").has_value());
    REQUIRE(svc.EnsureIssueEditMetaLoaded("DROP-1").has_value());
    CHECK_FALSE(svc.CanEditFieldForIssue("KEEP-1", "labels"));
    CHECK_FALSE(svc.CanEditFieldForIssue("DROP-1", "labels"));

    // Shrink the active set to only KEEP-1 (type "story"), then prune.
    deps.ActiveTicketsImpl = {MakeTicket("KEEP-1", "story")};
    svc.PruneEditMetaCacheToActiveTickets();

    // KEEP-1's loaded state survives: labels still denied (no refetch).
    CHECK_FALSE(svc.CanEditFieldForIssue("KEEP-1", "labels"));
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);

    // DROP-1's per-issue entry was pruned. Its issuetype ("bug") was also pruned (no active bug),
    // so it is fully optimistic again — labels allowed.
    CHECK(svc.CanEditFieldForIssue("DROP-1", "labels"));
}

// ---------------------------------------------------------------------------
// WarmIssueTypeEditMetaAtStartAsync — one representative per issue type.
// ---------------------------------------------------------------------------

TEST_CASE("EditMetaCacheService::WarmIssueTypeEditMetaAtStartAsync populates the issue-type cache") {
    FakeEditMetaDeps deps; // inline-synchronous LaunchBackgroundTask → warm completes in-call
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("XYZ-9", "bug"));
    EditMetaCacheService svc(deps);
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});

    TrackerConfig cfg;
    svc.WarmIssueTypeEditMetaAtStartAsync(cfg);

    // One representative per issue type was fetched; an unloaded sibling of type "story" now answers
    // from the type cache (labels denied).
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-2", "labels"));
    CHECK(svc.CanEditFieldForIssue("ABC-2", "summary"));
    // Component options are not this service's job any more (ProjectComponentsCacheService).
    CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 0u);

    // A second warm skips the types that are already live.
    svc.WarmIssueTypeEditMetaAtStartAsync(cfg);
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
}

// ---------------------------------------------------------------------------
// Quality Pillar 6 (offline-first S6): no editmeta request while offline; back off after a failure.
// ---------------------------------------------------------------------------

TEST_CASE("EditMetaCacheService makes no editmeta request while the tracker is offline") {
    FakeEditMetaDeps deps;
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);
    deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;

    const VoidResult r = svc.EnsureIssueEditMetaLoaded("ABC-1");
    CHECK_FALSE(r.has_value());
    svc.WarmIssueEditMetaAsync("ABC-1");

    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 0u);
    CHECK(deps.LaunchBackgroundTaskCalls == 0);
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels")); // nothing loaded: stays optimistic

    // Skipping offline records no backoff: the first warmup after reconnect fetches.
    deps.ConnectivityImpl = TrackerConnectivityState::AuthenticatedReachable;
    svc.WarmIssueEditMetaAsync("ABC-1");
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));
}

TEST_CASE("EditMetaCacheService::WarmIssueEditMetaAsync backs off after a failed fetch") {
    FakeEditMetaDeps deps;
    deps.Fake()->SetIssueEditMetaFailure("ABC-1", "HTTP 503: backend unreachable");
    EditMetaCacheService svc(deps);

    // The grid calls this every frame for the active row; after one failure it must not refetch.
    for (int frame = 0; frame < 5; ++frame) {
        svc.WarmIssueEditMetaAsync("ABC-1");
    }
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
    CHECK(deps.LaunchBackgroundTaskCalls == 1);
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels")); // the failure leaves the issue optimistic

    // An explicit load (a user's commit) is not throttled by the warmup backoff.
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}});
    CHECK(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
}

TEST_CASE("EditMetaCacheService::WarmIssueEditMetaAsync retries after a warmup that failed to start") {
    FirstLaunchThrowsEditMetaDeps deps;
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});
    EditMetaCacheService svc(deps);

    CHECK_NOTHROW(svc.WarmIssueEditMetaAsync("ABC-1"));
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 0u);

    // The in-flight marker was released, so the next frame starts the warmup.
    svc.WarmIssueEditMetaAsync("ABC-1");
    CHECK(deps.LaunchBackgroundTaskCalls == 2);
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
}

// ---------------------------------------------------------------------------
// Quality Pillar 6 (offline-first S11): per-type permissions saved for offline use, keyed by tracker.
// ---------------------------------------------------------------------------

namespace {

std::unordered_map<std::string, bool> SavedPermissions(FakeLookupCache& store, const std::string& backendKey,
                                                       const std::string& issueType) {
    LookupCacheRow row;
    std::unordered_map<std::string, bool> permissions;
    if (store.TryGetLookup(backendKey, smatchet::lookup::kEditMetaTypeKind, issueType, row)) {
        smatchet::lookup::ParseEditPermissions(row.PayloadJson, permissions);
    }
    return permissions;
}

} // namespace

TEST_CASE("EditMetaCacheService saves each fetched issue type's permissions to the lookup cache") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());

    CHECK(store->UpsertCalls.load() == 1);
    const std::unordered_map<std::string, bool> saved = SavedPermissions(*store, "Jira", "story");
    REQUIRE(saved.size() == 2u);
    CHECK(saved.at("summary"));
    CHECK_FALSE(saved.at("labels"));

    // A failed fetch saves nothing.
    deps.Fake()->SetIssueEditMetaFailure("ABC-3", "HTTP 503");
    CHECK_FALSE(svc.EnsureIssueEditMetaLoaded("ABC-3", nullptr).has_value());
    CHECK(store->UpsertCalls.load() == 1);
}

TEST_CASE("EditMetaCacheService: saved permissions answer offline but never stand in for a live fetch") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaTypeKind, "story",
                        smatchet::lookup::SerializeEditPermissions({{"summary", true}, {"labels", false}}));
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", true}});
    EditMetaCacheService svc(deps);

    // The warm loads the saved rows, then skips the network while offline.
    TrackerConfig cfg;
    svc.WarmIssueTypeEditMetaAtStartAsync(cfg);
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 0u);
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels")); // the saved answer, not optimistic
    CHECK(svc.CanEditFieldForIssue("ABC-1", "summary"));

    // Reachable again: the restored entry does not satisfy a load, so the issue's own editmeta is
    // fetched, and the live answer replaces the saved one.
    deps.ConnectivityImpl = TrackerConnectivityState::AuthenticatedReachable;
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels"));
    CHECK(SavedPermissions(*store, "Jira", "story").at("labels"));
}

TEST_CASE("EditMetaCacheService: the saved copy never overrides permissions fetched meanwhile") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"labels", true}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value()); // live "story": labels allowed
    // An older saved copy that disagrees, loaded by the next warm.
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaTypeKind, "story",
                        smatchet::lookup::SerializeEditPermissions({{"labels", false}}));
    TrackerConfig cfg;
    svc.WarmIssueTypeEditMetaAtStartAsync(cfg);

    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    CHECK(svc.CanEditFieldForIssue("ABC-2", "labels"));
}

TEST_CASE("EditMetaCacheService: two trackers never share an issue's or an issue type's permissions") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));
    CHECK_FALSE(svc.CanEditFieldForIssueWithType("Jira", "ABC-7", "labels", nullptr, "story"));

    // The same id and type on another Jira site: nothing is known there, so both stay optimistic.
    CHECK(svc.CanEditFieldForIssueWithType("Jira:other.example", "ABC-1", "labels", nullptr, "story"));
    deps.CacheBackendKeyImpl = "Jira:other.example";
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels"));
}

TEST_CASE("EditMetaCacheService::PruneEditMetaCacheToActiveTickets keeps saved (restored) issue types") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaTypeKind, "epic",
                        smatchet::lookup::SerializeEditPermissions({{"labels", false}}));
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
    EditMetaCacheService svc(deps);
    TrackerConfig cfg;
    svc.WarmIssueTypeEditMetaAtStartAsync(cfg);

    svc.PruneEditMetaCacheToActiveTickets(); // no active "epic"

    // Restored entries load once per tracker, so a prune must not lose them for the session.
    CHECK_FALSE(svc.CanEditFieldForIssueWithType("Jira", "ABC-9", "labels", nullptr, "epic"));
}

TEST_CASE("EditMetaCacheService::OnConnectivityRecovered lets the per-frame warmup retry at once") {
    FakeEditMetaDeps deps;
    deps.Fake()->SetIssueEditMetaFailure("ABC-1", "HTTP 503: backend unreachable");
    EditMetaCacheService svc(deps);

    svc.WarmIssueEditMetaAsync("ABC-1");
    svc.WarmIssueEditMetaAsync("ABC-1"); // inside the backoff
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);

    svc.OnConnectivityRecovered();
    svc.WarmIssueEditMetaAsync("ABC-1");
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
}
