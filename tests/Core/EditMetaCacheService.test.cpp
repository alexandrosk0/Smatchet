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
#include "EditMetaScopePure.h"
#include "LookupPayloadsPure.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <ctime>
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

    SUBCASE("a sprint catalog field is always editable via FindFieldSnapshotById") {
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

TEST_CASE("EditMetaCacheService::PruneEditMetaCacheToActiveTickets drops absent issues, keeps every issue type") {
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

    // DROP-1's per-issue entry was pruned, and with DROP-1 out of the active set its type cannot be
    // resolved, so it reads optimistic.
    CHECK(svc.CanEditFieldForIssue("DROP-1", "labels"));

    // Its issuetype ("bug") entry is kept — demoted, not erased. When DROP-1 comes back (the user
    // switches back to its view, served from the cache) the type's permissions still answer, labels
    // denied, with no new fetch: erasing the entry would lose them for the session, offline included,
    // since saved rows load only once per backend.
    deps.ActiveTicketsImpl.push_back(MakeTicket("DROP-1", "bug"));
    CHECK_FALSE(svc.CanEditFieldForIssue("DROP-1", "labels"));
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
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
// Quality Pillar 6 (offline-first S11): per-scope permissions saved for offline use, keyed by tracker.
// ---------------------------------------------------------------------------

namespace {

// The saved permissions of one (project, issue type) scope; empty when nothing is saved for it.
std::unordered_map<std::string, bool> SavedPermissions(FakeLookupCache& store, const std::string& backendKey,
                                                       const std::string& project, const std::string& issueType) {
    LookupCacheRow row;
    std::unordered_map<std::string, bool> permissions;
    if (store.TryGetLookup(backendKey, smatchet::lookup::kEditMetaProjectTypeKind,
                           smatchet::editmeta::BuildScopeStoreKey(project, issueType), row)) {
        smatchet::lookup::ParseEditPermissions(row.PayloadJson, permissions);
    }
    return permissions;
}

} // namespace

TEST_CASE("EditMetaCacheService saves each representative scope's permissions to the lookup cache") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());

    CHECK(store->UpsertCalls.load() == 1);
    const std::unordered_map<std::string, bool> saved = SavedPermissions(*store, "Jira", "ABC", "story");
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
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaProjectTypeKind,
                        smatchet::editmeta::BuildScopeStoreKey("ABC", "story"),
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
    CHECK(SavedPermissions(*store, "Jira", "ABC", "story").at("labels"));
}

TEST_CASE("EditMetaCacheService: a refresh that fails keeps the saved type permissions") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaProjectTypeKind,
                        smatchet::editmeta::BuildScopeStoreKey("ABC", "story"),
                        smatchet::lookup::SerializeEditPermissions({{"labels", false}}));
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
    EditMetaCacheService svc(deps);
    TrackerConfig cfg;
    svc.WarmIssueTypeEditMetaAtStartAsync(cfg); // loads the saved "story" row
    REQUIRE_FALSE(svc.CanEditFieldForIssueWithType("Jira", "ABC-1", "labels", nullptr, "story"));

    // A 400 triggers a refresh; the tracker drops before it answers.
    deps.ConnectivityImpl = TrackerConnectivityState::AuthenticatedReachable;
    deps.Fake()->SetIssueEditMetaFailure("ABC-1", "HTTP 503");
    CHECK_FALSE(svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", "ABC-1", "story").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);

    CHECK_FALSE(svc.CanEditFieldForIssueWithType("Jira", "ABC-1", "labels", nullptr, "story"));
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
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaProjectTypeKind,
                        smatchet::editmeta::BuildScopeStoreKey("ABC", "story"),
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
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaProjectTypeKind,
                        smatchet::editmeta::BuildScopeStoreKey("ABC", "epic"),
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

// ---------------------------------------------------------------------------
// EM-2: (project, issue type) scopes, representative-only promotion, capability gate, startup candidates.
// ---------------------------------------------------------------------------

namespace {

// Launches nothing: a task is parked until the test runs it, so the order of queued saves is observable.
class ParkingEditMetaDeps : public FakeEditMetaDeps {
  public:
    std::vector<std::function<void()>> Parked;
    void LaunchBackgroundTask(std::function<void()> task) override {
        ++LaunchBackgroundTaskCalls;
        Parked.push_back(std::move(task));
    }
};

const std::unordered_map<std::string, bool> kEditable = {{"summary", true}, {"labels", true}};
const std::unordered_map<std::string, bool> kReadOnly = {{"summary", false}, {"labels", false}};

} // namespace

TEST_CASE("EditMetaCacheService: a non-representative result stays with its issue and never narrows the scope") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story")); // a Done issue: nothing editable
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", kReadOnly);
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "summary")); // its own answer
    CHECK(svc.CanEditFieldForIssue("ABC-2", "summary"));       // the sibling is untouched (optimistic)
    CHECK(store->UpsertCalls.load() == 0);                     // nothing saved for the scope
    CHECK(SavedPermissions(*store, "Jira", "ABC", "story").empty());
}

TEST_CASE("EditMetaCacheService: a scope is one project and one issue type") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("XYZ-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-3", "bug"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-2", "labels")); // same project, same type: from the scope
    CHECK(svc.CanEditFieldForIssue("XYZ-1", "labels"));       // another project: optimistic
    CHECK(svc.CanEditFieldForIssue("ABC-3", "labels"));       // another type: optimistic
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
}

TEST_CASE("EditMetaCacheService: a local or draft id has no project, so its answer stays its own") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("local_draft_7", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("local_draft_8", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("local_draft_7").has_value());
    CHECK_FALSE(svc.CanEditFieldForIssue("local_draft_7", "labels"));
    CHECK(svc.CanEditFieldForIssue("local_draft_8", "labels")); // optimistic: no scope was promoted
    CHECK(store->UpsertCalls.load() == 0);
}

TEST_CASE("EditMetaCacheService: a backend whose answer ignores the issue gets one fetch per issue type") {
    FakeEditMetaDeps deps;
    deps.Fake()->SetEditMetaVariesPerIssue(false);
    deps.ActiveTicketsImpl.push_back(MakeTicket("11111111-aaaa", "task")); // a UUID-style id: no project
    deps.ActiveTicketsImpl.push_back(MakeTicket("22222222-bbbb", "task"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("11111111-aaaa").has_value());
    REQUIRE(svc.EnsureIssueEditMetaLoaded("22222222-bbbb").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u); // the scope stands in for the second
    CHECK_FALSE(svc.CanEditFieldForIssue("22222222-bbbb", "labels"));
}

TEST_CASE("EditMetaCacheService: the capability gate limits focused-row fetches to backends that vary per issue") {
    for (const bool varies : {false, true}) {
        FakeEditMetaDeps deps;
        deps.Fake()->SetEditMetaVariesPerIssue(varies);
        for (int i = 1; i <= 5; ++i) {
            deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-" + std::to_string(i), "story"));
        }
        deps.Fake()->SetDefaultIssueEditMetaSuccess(kEditable);
        EditMetaCacheService svc(deps);

        for (int i = 1; i <= 5; ++i) {
            svc.WarmIssueEditMetaAsync("ABC-" + std::to_string(i));
        }
        // Varying: each distinct focused row asks for its own. Not varying: the first fills the scope.
        CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == (varies ? 5u : 1u));
    }
}

TEST_CASE("EditMetaCacheService: a commit-like row (varies per issue, all read-only) never speaks for its scope") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "commit"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "commit"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess(kReadOnly);
    EditMetaCacheService svc(deps);

    svc.WarmIssueEditMetaAsync("ABC-1");
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "summary"));
    CHECK(svc.CanEditFieldForIssue("ABC-2", "summary")); // optimistic until its own fetch lands
    svc.WarmIssueEditMetaAsync("ABC-2");
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
}

TEST_CASE("EditMetaCacheService: the startup warm passes over a non-representative candidate") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story")); // Done: nothing editable
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story")); // Done too
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-3", "story")); // open: representative
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-4", "story")); // never needed
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", kReadOnly);
    deps.Fake()->SetIssueEditMetaSuccess("ABC-2", kReadOnly);
    deps.Fake()->SetIssueEditMetaSuccess("ABC-3", {{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());

    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 3u);  // stops at the first representative one
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-4", "labels")); // the scope answers for a ticket nobody fetched
}

TEST_CASE("EditMetaCacheService: a scope with no representative candidate is not retried for 30 minutes") {
    FakeEditMetaDeps deps;
    for (int i = 1; i <= 5; ++i) {
        deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-" + std::to_string(i), "story"));
    }
    deps.Fake()->SetDefaultIssueEditMetaSuccess(kReadOnly);
    EditMetaCacheService svc(deps);

    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == smatchet::editmeta::kMaxCandidatesPerScope);
    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig()); // backing off: no new fetches
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == smatchet::editmeta::kMaxCandidatesPerScope);
}

TEST_CASE("EditMetaCacheService: a warm spends at most 8 extra candidate fetches and 64 scopes") {
    FakeEditMetaDeps deps;
    // 70 projects, each with three Done candidates: only the first 64 scopes are covered, and past the first
    // candidate of each only 8 extra fetches are made in total.
    for (int p = 0; p < 70; ++p) {
        const std::string project = "P" + std::to_string(p);
        for (int i = 1; i <= 3; ++i) {
            deps.ActiveTicketsImpl.push_back(MakeTicket(project + "-" + std::to_string(i), "story"));
        }
    }
    deps.Fake()->SetDefaultIssueEditMetaSuccess(kReadOnly);
    EditMetaCacheService svc(deps);

    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());

    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() ==
          smatchet::editmeta::kMaxScopesPerWarm + smatchet::editmeta::kMaxExtraCandidateFetchesPerWarm);
}

TEST_CASE("EditMetaCacheService: a failing candidate is skipped, its sibling covers the scope") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story")); // a deleted or restricted issue
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("XYZ-1", "story")); // another scope
    deps.Fake()->SetIssueEditMetaFailure("ABC-1", "HTTP 404");
    deps.Fake()->SetIssueEditMetaSuccess("ABC-2", {{"summary", true}, {"labels", false}});
    deps.Fake()->SetIssueEditMetaSuccess("XYZ-1", {{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());

    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 3u); // ABC-1 failed, ABC-2 filled ABC, XYZ-1 filled XYZ
    CHECK_FALSE(svc.CanEditFieldForIssueWithType("Jira", "ABC-9", "labels", nullptr, "story"));
    CHECK_FALSE(svc.CanEditFieldForIssueWithType("Jira", "XYZ-9", "labels", nullptr, "story"));
}

TEST_CASE("EditMetaCacheService: a scope whose candidates all fail records no backoff") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    deps.Fake()->SetIssueEditMetaFailure("ABC-1", "HTTP 404");
    deps.Fake()->SetIssueEditMetaFailure("ABC-2", "HTTP 403");
    EditMetaCacheService svc(deps);

    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig()); // failures are not "no representative": retried now
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 4u);
}

namespace {

// Reachable except for its second query: the fetch's own offline gate is the first, the warm's check after a failed
// fetch the second. The tracker "drops" exactly there, so only the warm's early exit keeps it from going on.
class DropsAtSecondQueryDeps : public FakeEditMetaDeps {
  public:
    TrackerConnectivityState TrackerConnectivity() const override {
        return ++Queries == 2 ? TrackerConnectivityState::TransportDown
                              : TrackerConnectivityState::AuthenticatedReachable;
    }
    mutable int Queries = 0;
};

} // namespace

TEST_CASE("EditMetaCacheService: a warm that finds the tracker offline after a failed fetch stops there") {
    DropsAtSecondQueryDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("XYZ-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("QRS-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaFailure("HTTP 503");
    EditMetaCacheService svc(deps);

    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u); // the later scopes are left for the next warm
}

TEST_CASE("EditMetaCacheService: legacy per-type rows are deleted once and never read") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaTypeKind, "story",
                        smatchet::lookup::SerializeEditPermissions({{"labels", false}}));
    store->UpsertLookup("Jira", smatchet::lookup::kEditMetaProjectTypeKind,
                        smatchet::editmeta::BuildScopeStoreKey("ABC", "bug"),
                        smatchet::lookup::SerializeEditPermissions({{"labels", false}}));
    store->UpsertLookup("Plane", smatchet::lookup::kEditMetaTypeKind, "story",
                        smatchet::lookup::SerializeEditPermissions({{"labels", false}})); // another backend
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
    EditMetaCacheService svc(deps);

    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());

    LookupCacheRow row;
    CHECK_FALSE(store->TryGetLookup("Jira", smatchet::lookup::kEditMetaTypeKind, "story", row));
    CHECK(store->TryGetLookup("Plane", smatchet::lookup::kEditMetaTypeKind, "story", row)); // not this backend's
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels"));                                     // legacy row not read
    // The new kind coexists with the legacy one and is read.
    CHECK_FALSE(svc.CanEditFieldForIssueWithType("Jira", "ABC-2", "labels", nullptr, "bug"));
}

TEST_CASE("EditMetaCacheService: a saved scope older than 30 days is not restored") {
    for (const int ageDays : {29, 31}) {
        FakeEditMetaDeps deps;
        const auto store = std::make_shared<FakeLookupCache>();
        store->StampEpochSec.store(static_cast<std::int64_t>(std::time(nullptr)) -
                                   static_cast<std::int64_t>(ageDays) * 24 * 60 * 60);
        store->UpsertLookup("Jira", smatchet::lookup::kEditMetaProjectTypeKind,
                            smatchet::editmeta::BuildScopeStoreKey("ABC", "story"),
                            smatchet::lookup::SerializeEditPermissions({{"labels", false}}));
        store->StampEpochSec.store(0);
        deps.LookupCacheImpl = store;
        deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
        deps.ConnectivityImpl = TrackerConnectivityState::TransportDown;
        EditMetaCacheService svc(deps);

        svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());

        // Within the ceiling the saved answer applies offline; past it the cell is optimistic again.
        CHECK(svc.CanEditFieldForIssue("ABC-1", "labels") == (ageDays > 30));
    }
}

TEST_CASE("EditMetaCacheService: scope saves run in order off the calling thread and a superseded save is skipped") {
    ParkingEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("XYZ-1", "story"));
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"labels", false}, {"summary", true}});
    deps.Fake()->SetIssueEditMetaSuccess("XYZ-1", {{"labels", true}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    REQUIRE(svc.EnsureIssueEditMetaLoaded("XYZ-1").has_value());
    // A refresh of ABC-1 with a different answer queues a newer save of the same scope.
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"labels", true}, {"summary", true}});
    REQUIRE(svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", "ABC-1", "story").has_value());

    CHECK(store->UpsertCalls.load() == 0); // nothing written on the calling thread
    REQUIRE(deps.Parked.size() == 1u);     // one drain, parked
    deps.Parked[0]();
    CHECK(store->UpsertCalls.load() == 2); // ABC's first save was skipped (superseded); XYZ's and ABC's last ran
    CHECK(SavedPermissions(*store, "Jira", "ABC", "story").at("labels"));
    CHECK(SavedPermissions(*store, "Jira", "XYZ", "story").at("labels"));
}

TEST_CASE("EditMetaCacheService::FlushScopeSaves runs a save whose drain was dropped") {
    FakeEditMetaDeps deps;
    deps.DropTasks = true;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"labels", false}, {"summary", true}});
    EditMetaCacheService svc(deps);

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    CHECK(store->UpsertCalls.load() == 0); // the launch was dropped: the save is still queued
    CHECK(svc.FlushScopeSaves(std::chrono::milliseconds(100)));
    CHECK(store->UpsertCalls.load() == 1);
    CHECK_FALSE(SavedPermissions(*store, "Jira", "ABC", "story").at("labels"));
}

TEST_CASE("EditMetaCacheService: a refresh that fails keeps the scope entry") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"labels", false}, {"summary", true}});
    EditMetaCacheService svc(deps);
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());

    deps.Fake()->SetIssueEditMetaFailure("ABC-1", "HTTP 503");
    CHECK_FALSE(svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", "ABC-1", "story").has_value());
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-2", "labels")); // the scope still answers
}

TEST_CASE("EditMetaCacheService: a prune demotes a scope no pane shows, per project, and keeps its saved answer") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("XYZ-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    REQUIRE(svc.EnsureIssueEditMetaLoaded("XYZ-1").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);

    // Only ABC's story remains on screen: XYZ's scope goes stale (refetched when next needed online) but
    // keeps answering. A warm refetches XYZ and leaves ABC (still live) alone.
    deps.ActiveTicketsImpl = {MakeTicket("ABC-1", "story")};
    svc.PruneEditMetaCacheToActiveTickets();
    CHECK_FALSE(svc.CanEditFieldForIssueWithType("Jira", "XYZ-9", "labels", nullptr, "story"));
    deps.ActiveTicketsImpl = {MakeTicket("ABC-1", "story"), MakeTicket("XYZ-1", "story")};
    svc.WarmIssueTypeEditMetaAtStartAsync(TrackerConfig());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 3u);
}

TEST_CASE("EditMetaCacheService: a refetch that says the same does not queue another save") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    svc.WarmIssueEditMetaAsync("ABC-1");
    svc.WarmIssueEditMetaAsync("ABC-2"); // its own fetch (Jira varies per issue), same answer
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
    CHECK(store->UpsertCalls.load() == 1);
}

// ---------------------------------------------------------------------------
// EM-3: fetch generations. A Refresh supersedes the fetch it overtakes, an Invalidate retires an in-flight one,
// a failed fetch never replaces a loaded entry, and the in-flight latch is released however a launch ends.
// ---------------------------------------------------------------------------

TEST_CASE("EditMetaCacheService: a Refresh that lands while an older fetch is in flight wins") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}, {"labels", false}}); // the old answer
    EditMetaCacheService svc(deps);
    bool refreshed = false;
    deps.Fake()->SetFetchIssueEditMetaHook([&](const std::string&) {
        if (refreshed) {
            return; // the Refresh's own fetch
        }
        refreshed = true;
        // The tracker changed while the first fetch was in flight: a Refresh with the new answer overtakes it.
        deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}, {"labels", true}});
        REQUIRE(svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", "ABC-1", "story").has_value());
    });

    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value()); // returns the OLD answer, which is dropped

    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels")); // the newer answer stands
    CHECK(store->UpsertCalls.load() == 1);              // only the newer answer reached the store
    CHECK(SavedPermissions(*store, "Jira", "ABC", "story").at("labels"));
}

TEST_CASE("EditMetaCacheService: an Invalidate while a fetch is in flight drops its answer") {
    FakeEditMetaDeps deps;
    const auto store = std::make_shared<FakeLookupCache>();
    deps.LookupCacheImpl = store;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);
    deps.Fake()->SetFetchIssueEditMetaHook(
        [&](const std::string& id) { svc.InvalidateIssueEditMeta(id); }); // the issue changed under the fetch

    CHECK(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value()); // not a failure: the answer is just stale

    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels")); // nothing stored: optimistic again
    CHECK(store->UpsertCalls.load() == 0);              // and nothing promoted to the scope or saved
    deps.Fake()->SetFetchIssueEditMetaHook(nullptr);
    CHECK(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value()); // the latch was released: a new fetch runs
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));
}

TEST_CASE("EditMetaCacheService: an Ensure that finds the issue already being fetched asks again for nothing") {
    ParkingEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    svc.WarmIssueEditMetaAsync("ABC-1"); // launched, parked: the fetch is registered
    REQUIRE(deps.Parked.size() == 1u);
    CHECK(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value()); // refused: no second request for the same issue
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 0u);
    CHECK(svc.CanEditFieldForIssue("ABC-1", "labels")); // optimistic until the warm lands

    deps.Parked[0]();
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));
}

TEST_CASE("EditMetaCacheService: a Refresh supersedes a parked warm, which then does nothing") {
    ParkingEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    svc.WarmIssueEditMetaAsync("ABC-1");
    REQUIRE(deps.Parked.size() == 1u);
    REQUIRE(svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", "ABC-1", "story").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);

    deps.Parked[0](); // the warm runs late: the issue is loaded already
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
}

TEST_CASE("EditMetaCacheService: a failing Refresh keeps the loaded entry of the issue") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetIssueEditMetaSuccess("ABC-1", {{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);
    REQUIRE(svc.EnsureIssueEditMetaLoaded("ABC-1").has_value());
    REQUIRE_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels"));

    deps.Fake()->SetIssueEditMetaFailure("ABC-1", "HTTP 503");
    CHECK_FALSE(svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", "ABC-1", "story").has_value());

    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-1", "labels")); // the previous answer still applies
    svc.WarmIssueEditMetaAsync("ABC-1");                      // still loaded: no refetch
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 2u);
}

TEST_CASE("EditMetaCacheService: a warm whose task is dropped unrun releases its latch") {
    FakeEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    EditMetaCacheService svc(deps);

    deps.DropTasks = true; // shutdown has begun: the launcher destroys the task
    svc.WarmIssueEditMetaAsync("ABC-1");
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 0u);

    deps.DropTasks = false;
    svc.WarmIssueEditMetaAsync("ABC-1"); // not stuck "in flight" forever
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u);
}

TEST_CASE("EditMetaCacheService: another issue's fetch in flight never blocks a Refresh") {
    ParkingEditMetaDeps deps;
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}});
    EditMetaCacheService svc(deps);

    svc.WarmIssueEditMetaAsync("ABC-1"); // parked
    REQUIRE(svc.RefreshIssueEditMetaFor(deps.BackendImpl, "Jira", "ABC-2", "story").has_value());
    CHECK(deps.Fake()->FetchIssueEditMetaCallCount() == 1u); // another issue's fetch never blocks this one
}
