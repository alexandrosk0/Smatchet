// Views class seam tests — Slice 0 Workstream 1 of
// docs/plans/active/multi-grid-tabs.md (characterize-existing regression net).
//
// Pins the CURRENT behaviour of the Views state holder (Source/Core/src/Ui/Views.cpp,
// header Source/Core/include/Ui/Views.h) BEFORE the multi-pane refactor:
//   * EnsureLoaded — backend-keyed slice selection + bootstrap-on-empty.
//   * Activate / Create / UpdateActive / DeleteActive contracts.
//   * GetRevision() monotonicity + the explicit BumpRevision cache-key contract.
//   * Cross-backend slice swap flushes the outgoing slice into its disk bucket.
//
// Views persists through ConfigManager statics, so each case runs under a
// TestEnvGuard-redirected user-data dir; ViewsFileCleanup removes the views
// file the guard itself doesn't own.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"
#include "Views.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

/// Removes smatchet_views.json while the TestEnvGuard redirect is still active.
/// Declare AFTER the guard so this dtor runs first (reverse declaration order).
struct ViewsFileCleanup {
    ~ViewsFileCleanup() { std::remove(ConfigManager::GetViewsPath().c_str()); }
};

TrackerConfig MakeCfg(const std::string& trackerType, const std::string& jql = std::string()) {
    TrackerConfig cfg;
    cfg.TrackerType = trackerType;
    cfg.JqlQuery = jql;
    return cfg;
}

} // namespace

TEST_CASE("Views::EnsureLoaded bootstraps the default view for the backend and bumps the revision") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    CHECK(views.GetRevision() == 0);
    CHECK(views.GetActiveView() == nullptr);

    views.EnsureLoaded(MakeCfg("Jira"));
    const std::uint64_t revAfterLoad = views.GetRevision();
    CHECK(revAfterLoad > 0);
    REQUIRE(views.GetStore().Views.size() == 1);
    CHECK(views.GetStore().ActiveViewId == "default_view");
    REQUIRE(views.GetActiveView() != nullptr);
    CHECK(views.GetActiveView()->Id == "default_view");

    // Re-loading with the SAME backend is a no-op: no slice reset, no revision bump.
    views.EnsureLoaded(MakeCfg("Jira"));
    CHECK(views.GetRevision() == revAfterLoad);
    CHECK(views.GetStore().Views.size() == 1);
}

TEST_CASE("Views::EnsureLoaded swaps per-backend slices and preserves edits across the switch" *
          doctest::test_suite("[high-risk]")) {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    REQUIRE(views.GetStore().Views.size() == 1);

    // Customize the Jira slice via the public mutation API.
    ViewDefinition proto;
    proto.Name = "My Jira View";
    proto.Jql = "project = CUST";
    proto.Fields = {"summary", "status"};
    REQUIRE(views.Create(proto));
    CHECK(views.GetStore().Views.size() == 2);
    CHECK(views.GetStore().ActiveViewId == "my_jira_view");

    // Switch to Plane: slice becomes the Plane bucket's bootstrap default.
    const std::uint64_t revBeforeSwap = views.GetRevision();
    views.EnsureLoaded(MakeCfg("Plane"));
    CHECK(views.GetRevision() > revBeforeSwap);
    REQUIRE(views.GetStore().Views.size() == 1);
    CHECK(views.GetStore().ActiveViewId == "plane_default_view");

    // Switch back to Jira: the custom view + its active selection survived the
    // round-trip through the disk bucket (outgoing slice flushed on swap).
    views.EnsureLoaded(MakeCfg("Jira"));
    REQUIRE(views.GetStore().Views.size() == 2);
    CHECK(views.GetStore().ActiveViewId == "my_jira_view");
    REQUIRE(views.GetActiveView() != nullptr);
    CHECK(views.GetActiveView()->Jql == "project = CUST");

    // A brand-new Views instance sees the same persisted state (Save round-trip).
    Views fresh;
    fresh.EnsureLoaded(MakeCfg("Jira"));
    REQUIRE(fresh.GetStore().Views.size() == 2);
    CHECK(fresh.GetStore().ActiveViewId == "my_jira_view");
}

TEST_CASE("Views::Activate switches the active view and rejects unknown ids without a revision bump") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    ViewDefinition proto;
    proto.Name = "Second";
    REQUIRE(views.Create(proto)); // id "second", becomes active

    REQUIRE(views.Activate("default_view"));
    CHECK(views.GetStore().ActiveViewId == "default_view");
    const std::uint64_t rev = views.GetRevision();

    // Unknown id: rejected, active view + revision untouched.
    CHECK_FALSE(views.Activate("does_not_exist"));
    CHECK(views.GetStore().ActiveViewId == "default_view");
    CHECK(views.GetRevision() == rev);

    // Known id: accepted, revision bumps by exactly one.
    REQUIRE(views.Activate("second"));
    CHECK(views.GetStore().ActiveViewId == "second");
    CHECK(views.GetRevision() == rev + 1);
}

TEST_CASE("Views::Create builds ids from names, dedupes collisions, and auto-builds column order") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));

    ViewDefinition proto;
    proto.Name = "My View";
    proto.Fields = {"summary", "status"};
    REQUIRE(views.Create(proto));
    REQUIRE(views.GetActiveView() != nullptr);
    const ViewDefinition& created = *views.GetActiveView();
    CHECK(created.Id == "my_view"); // lowercased, space -> underscore
    CHECK(created.Name == "My View");
    // Empty prototype ColumnOrder is auto-built: "id" + field:<each field>.
    const std::vector<std::string> expectedOrder = {"id", "field:summary", "field:status"};
    CHECK(created.ColumnOrder == expectedOrder);

    // Same name again: id collision resolved with a numeric suffix.
    REQUIRE(views.Create(proto));
    REQUIRE(views.GetActiveView() != nullptr);
    CHECK(views.GetActiveView()->Id == "my_view_2");

    SUBCASE("empty name and id fall back to the generic view id") {
        ViewDefinition blank;
        REQUIRE(views.Create(blank));
        REQUIRE(views.GetActiveView() != nullptr);
        CHECK(views.GetActiveView()->Id == "view");
        CHECK(views.GetActiveView()->Name == "view");
    }
}

TEST_CASE("Views::UpdateActive preserves the active id and falls back the name") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    REQUIRE(views.GetStore().ActiveViewId == "default_view");

    ViewDefinition updated;
    updated.Id = "attempted_id_override"; // must NOT take effect
    updated.Name = "";                    // falls back to the preserved id
    updated.Jql = "project = UPD";
    updated.Fields = {"summary"};
    const std::uint64_t rev = views.GetRevision();
    REQUIRE(views.UpdateActive(updated));
    CHECK(views.GetRevision() == rev + 1);

    REQUIRE(views.GetActiveView() != nullptr);
    CHECK(views.GetActiveView()->Id == "default_view"); // id preserved
    CHECK(views.GetActiveView()->Name == "default_view"); // empty name -> id
    CHECK(views.GetActiveView()->Jql == "project = UPD");
}

TEST_CASE("Views::DeleteActive refuses the last view and picks the neighbour after a delete") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    ViewDefinition a;
    a.Name = "A";
    ViewDefinition b;
    b.Name = "B";
    REQUIRE(views.Create(a)); // [default_view, a]
    REQUIRE(views.Create(b)); // [default_view, a, b], active b

    // Delete the middle view: the next view at the same index ("b") becomes active.
    REQUIRE(views.Activate("a"));
    REQUIRE(views.DeleteActive());
    CHECK(views.GetStore().Views.size() == 2);
    CHECK(views.GetStore().ActiveViewId == "b");

    // Delete the LAST view: the previous neighbour becomes active (no jump to front... it
    // IS the front here with 2 views — previous index is what the contract picks).
    REQUIRE(views.DeleteActive());
    CHECK(views.GetStore().Views.size() == 1);
    CHECK(views.GetStore().ActiveViewId == "default_view");

    // Single remaining view can never be deleted.
    CHECK_FALSE(views.DeleteActive());
    CHECK(views.GetStore().Views.size() == 1);
}

TEST_CASE("Views mutators on an unloaded instance are rejected and leave the revision at zero") {
    Views views; // never EnsureLoaded — empty slice, no active backend
    CHECK_FALSE(views.Activate("anything"));
    ViewDefinition v;
    v.Name = "X";
    CHECK_FALSE(views.UpdateActive(v));
    CHECK_FALSE(views.DeleteActive());
    CHECK(views.GetActiveView() == nullptr);
    // characterizes existing behaviour — Create() on an unloaded Views still
    // succeeds in-memory (it appends to the empty slice and bumps the revision);
    // only the Save() inside it is gated on Loaded/HasActiveBackend.
    CHECK(views.Create(v));
    CHECK(views.GetRevision() == 1);
}

TEST_CASE("Views::GetRevision is monotonic across mutations and BumpRevision is an explicit cache-key signal") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    std::uint64_t last = views.GetRevision();

    ViewDefinition proto;
    proto.Name = "Rev Probe";
    REQUIRE(views.Create(proto));
    CHECK(views.GetRevision() > last);
    last = views.GetRevision();

    REQUIRE(views.Activate("default_view"));
    CHECK(views.GetRevision() > last);
    last = views.GetRevision();

    ViewDefinition upd;
    upd.Name = "Renamed";
    REQUIRE(views.UpdateActive(upd));
    CHECK(views.GetRevision() > last);
    last = views.GetRevision();

    REQUIRE(views.DeleteActive());
    CHECK(views.GetRevision() > last);
    last = views.GetRevision();

    // BumpRevision: external in-place mutators (GetStoreMutable / GetActiveViewMutable
    // callers) must be able to invalidate caches even when nothing else changed.
    views.GetStoreMutable().Views[0].Name = "Edited In Place";
    views.BumpRevision();
    CHECK(views.GetRevision() == last + 1);

    // Reads never bump.
    (void)views.GetStore();
    (void)views.GetActiveView();
    CHECK(views.GetRevision() == last + 1);
}
