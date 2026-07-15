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
#include "../support/FakeTrackerClient.h"

#include "CachedTicketTypes.h"
#include "EditMetaCacheService.h"
#include "GridLiveContext.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <string>
#include <unordered_map>
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
// WarmIssueTypeEditMetaAtStartAsync — #975 kick-time catalog write proof.
// ---------------------------------------------------------------------------

TEST_CASE(
    "EditMetaCacheService::WarmIssueTypeEditMetaAtStartAsync populates type cache + kick-time component options") {
    FakeEditMetaDeps deps; // inline-synchronous LaunchBackgroundTask → warm completes in-call
    // Two active tickets across two projects (key prefixes ABC / XYZ), distinct issuetypes.
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-1", "story"));
    deps.ActiveTicketsImpl.push_back(MakeTicket("XYZ-9", "bug"));
    EditMetaCacheService svc(deps);

    deps.Fake()->SetDefaultIssueEditMetaSuccess({{"summary", true}, {"labels", false}});
    // Per-project components scripted for both project keys.
    TrackerFieldOption optAbc;
    optAbc.Id = "100";
    optAbc.Value = "Backend";
    TrackerFieldOption optXyz;
    optXyz.Id = "200";
    optXyz.Value = "Frontend";
    deps.Fake()->SetProjectComponentsSuccess("ABC", {optAbc});
    deps.Fake()->SetProjectComponentsSuccess("XYZ", {optXyz});

    TrackerConfig cfg;
    svc.WarmIssueTypeEditMetaAtStartAsync(cfg);

    // #975 proof: the warm worker wrote the per-project options into the KICK-TIME catalog
    // (CatalogImpl, returned by KickTimeFieldCatalog) — not a re-resolved completion-time one.
    {
        std::lock_guard<std::mutex> lock(deps.CatalogImpl.availableFieldsMutex_);
        REQUIRE(deps.CatalogImpl.projectComponentOptions_.count("ABC") == 1u);
        REQUIRE(deps.CatalogImpl.projectComponentOptions_.count("XYZ") == 1u);
        REQUIRE(deps.CatalogImpl.projectComponentOptions_["ABC"].size() == 1u);
        CHECK(deps.CatalogImpl.projectComponentOptions_["ABC"][0].Value == "Backend");
        CHECK(deps.CatalogImpl.projectComponentOptions_["XYZ"][0].Value == "Frontend");
        // In-flight markers erased on success (every exit path erases — no "Loading…" leak).
        CHECK(deps.CatalogImpl.projectComponentsInFlight_.empty());
        // Success clears any prior backoff.
        CHECK(deps.CatalogImpl.projectComponentsRetryAfter_.empty());
    }

    // The issuetype-level editmeta cache was warmed for both representatives: an unloaded
    // sibling issue of type "story" now answers from the type cache (labels denied).
    deps.ActiveTicketsImpl.push_back(MakeTicket("ABC-2", "story"));
    CHECK_FALSE(svc.CanEditFieldForIssue("ABC-2", "labels"));
    CHECK(svc.CanEditFieldForIssue("ABC-2", "summary"));

    // Both per-project component fetches happened exactly once.
    CHECK(deps.Fake()->FetchProjectComponentsCallCount() == 2u);
}
