// ConfigManager view-store seam tests — Slice 0 Workstream 1 of
// docs/plans/active/multi-grid-tabs.md (characterize-existing regression net).
//
// Pins the CURRENT single-grid view-persistence behaviour of
// Source/Core/src/Config/ConfigManager_Views.cpp BEFORE the multi-pane refactor:
//   * NormalizeViewsBackendKey — case-insensitive tracker-type → bucket-key mapping.
//   * LoadViewsOrBootstrap — per-backend default field-sets (Jira / Plane / GitHub).
//   * v2 `backends` map save → load round-trip (widths, sort specs, zero-direction drop).
//   * MULTI-BUCKET COEXISTENCE — writing one backend's bucket must never clobber
//     another's. THE core invariant the multi-grid work builds on.
//   * Legacy v1 (top-level active_view_id + views) → Jira-bucket migration.
//   * EnsureViewBucketBootstrapped idempotency + empty-active-id repair.
//
// Every case runs under TestEnvGuard so smatchet_views.json lives in a private
// temp dir; ViewsFileCleanup removes the views file the guard itself doesn't own.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

/// Removes smatchet_views.json while the TestEnvGuard redirect is still active.
/// Declare AFTER the guard so this dtor runs first (reverse declaration order).
struct ViewsFileCleanup {
    ~ViewsFileCleanup() { std::remove(ConfigManager::GetViewsPath().c_str()); }
};

void WriteViewsFileRaw(const std::string& content) {
    std::ofstream f(ConfigManager::GetViewsPath(), std::ios::binary | std::ios::trunc);
    f << content;
}

bool HasColumn(const ViewDefinition& v, const std::string& col) {
    for (const auto& c : v.ColumnOrder) {
        if (c == col) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("ConfigManager::NormalizeViewsBackendKey maps tracker type case-insensitively, defaulting to Jira") {
    CHECK(ConfigManager::NormalizeViewsBackendKey("plane") == "Plane");
    CHECK(ConfigManager::NormalizeViewsBackendKey("Plane") == "Plane");
    CHECK(ConfigManager::NormalizeViewsBackendKey("PLANE") == "Plane");

    CHECK(ConfigManager::NormalizeViewsBackendKey("github") == "GitHub");
    CHECK(ConfigManager::NormalizeViewsBackendKey("GitHub") == "GitHub");
    CHECK(ConfigManager::NormalizeViewsBackendKey("GITHUB") == "GitHub");

    CHECK(ConfigManager::NormalizeViewsBackendKey("jira") == "Jira");
    CHECK(ConfigManager::NormalizeViewsBackendKey("Jira") == "Jira");

    // Everything unrecognized falls back to the default backend bucket (Jira).
    CHECK(ConfigManager::NormalizeViewsBackendKey("") == "Jira");
    CHECK(ConfigManager::NormalizeViewsBackendKey("asana") == "Jira");
    // No whitespace trimming — characterizes existing behaviour: a padded type
    // misses the plane match and lands in the Jira bucket.
    CHECK(ConfigManager::NormalizeViewsBackendKey("plane ") == "Jira");
}

TEST_CASE("ConfigManager::LoadViewsOrBootstrap seeds the Jira default field-set on a fresh install") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.JqlQuery = "";

    ViewsStore store = ConfigManager::LoadViewsOrBootstrap(cfg);
    CHECK(store.Version == 2);
    REQUIRE(store.Views.size() == 1);
    const ViewDefinition& v = store.Views[0];
    CHECK(store.ActiveViewId == "default_view");
    CHECK(v.Id == "default_view");
    CHECK(v.Name == "Default View");
    // Empty configured JQL falls back to the currentUser default.
    CHECK(v.Jql == "assignee=currentUser()");
    const std::vector<std::string> expectedFields = {"summary",   "assignee",    "priority", "status",
                                                     "issuetype", "description", "created",  "updated"};
    CHECK(v.Fields == expectedFields);
    REQUIRE(v.ColumnOrder.size() == expectedFields.size() + 1);
    CHECK(v.ColumnOrder[0] == "id");
    CHECK(HasColumn(v, "field:summary"));
    CHECK(HasColumn(v, "field:issuetype"));
    REQUIRE(v.ColumnWidths.count("id") == 1);
    CHECK(v.ColumnWidths.at("id") == doctest::Approx(90.0f));

    // A non-empty configured JQL is carried into the seeded view.
    SUBCASE("configured JqlQuery is adopted by the seeded Jira view") {
        std::remove(ConfigManager::GetViewsPath().c_str());
        TrackerConfig cfg2;
        cfg2.TrackerType = "Jira";
        cfg2.JqlQuery = "project = CUST";
        ViewsStore store2 = ConfigManager::LoadViewsOrBootstrap(cfg2);
        REQUIRE(store2.Views.size() == 1);
        CHECK(store2.Views[0].Jql == "project = CUST");
    }
}

TEST_CASE("ConfigManager::LoadViewsOrBootstrap seeds the Plane default field-set (empty Jql by design)") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    TrackerConfig cfg;
    cfg.TrackerType = "Plane";
    cfg.JqlQuery = "project = IGNORED";

    ViewsStore store = ConfigManager::LoadViewsOrBootstrap(cfg);
    REQUIRE(store.Views.size() == 1);
    const ViewDefinition& v = store.Views[0];
    CHECK(store.ActiveViewId == "plane_default_view");
    CHECK(v.Id == "plane_default_view");
    CHECK(v.Name == "Default Plane View");
    // characterizes existing behaviour — the Plane default view ignores
    // cfg.JqlQuery entirely (unlike Jira / GitHub, which adopt it).
    CHECK(v.Jql == "");
    const std::vector<std::string> expectedFields = {"summary", "status",  "priority", "assignee",
                                                     "labels",  "created", "updated"};
    CHECK(v.Fields == expectedFields);
    REQUIRE(v.ColumnOrder.size() == expectedFields.size() + 1);
    CHECK(v.ColumnOrder[0] == "id");
    CHECK(HasColumn(v, "field:labels"));
    REQUIRE(v.ColumnWidths.count("id") == 1);
    CHECK(v.ColumnWidths.at("id") == doctest::Approx(90.0f));
}

TEST_CASE("ConfigManager::LoadViewsOrBootstrap seeds the GitHub default field-set") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    TrackerConfig cfg;
    cfg.TrackerType = "github"; // lowercase, like GitHubClient reports
    cfg.JqlQuery = "repo:o/r is:open";

    ViewsStore store = ConfigManager::LoadViewsOrBootstrap(cfg);
    REQUIRE(store.Views.size() == 1);
    const ViewDefinition& v = store.Views[0];
    CHECK(store.ActiveViewId == "github_default_view");
    CHECK(v.Id == "github_default_view");
    CHECK(v.Name == "Default GitHub View");
    CHECK(v.Jql == "repo:o/r is:open");
    const std::vector<std::string> expectedFields = {"summary", "description", "status",  "assignee",
                                                     "labels",  "author",      "created", "updated"};
    CHECK(v.Fields == expectedFields);
    CHECK(HasColumn(v, "field:author"));
    // GitHub seeds no `priority` / `issuetype` columns (absent from its catalog).
    CHECK_FALSE(HasColumn(v, "field:priority"));
    CHECK_FALSE(HasColumn(v, "field:issuetype"));
}

TEST_CASE("ConfigManager v2 backends map survives a save -> load round-trip") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    PersistentViewsFile disk;
    disk.Version = 2;

    ViewWorkspaceState jiraWs;
    jiraWs.ActiveViewId = "jv2";
    ViewDefinition jv1;
    jv1.Id = "jv1";
    jv1.Name = "Jira One";
    jv1.Jql = "project = A";
    jv1.Fields = {"summary", "status"};
    jv1.ColumnOrder = {"id", "field:summary", "field:status"};
    jv1.ColumnWidths["id"] = 75.0f;
    jv1.ColumnWidths["field:summary"] = 320.5f;
    ViewSortSpec ascending;
    ascending.ColumnKey = "field:status";
    ascending.Direction = 1;
    ViewSortSpec noneDirection;
    noneDirection.ColumnKey = "field:summary";
    noneDirection.Direction = 0; // dropped on serialize — characterized below
    jv1.SortSpecs.push_back(ascending);
    jv1.SortSpecs.push_back(noneDirection);
    ViewDefinition jv2;
    jv2.Id = "jv2";
    jv2.Name = "Jira Two";
    jv2.Jql = "project = B";
    jv2.Fields = {"summary"};
    jv2.ColumnOrder = {"id", "field:summary"};
    jiraWs.Views.push_back(jv1);
    jiraWs.Views.push_back(jv2);
    disk.Backends["Jira"] = jiraWs;

    ViewWorkspaceState planeWs;
    planeWs.ActiveViewId = "pv1";
    ViewDefinition pv1;
    pv1.Id = "pv1";
    pv1.Name = "Plane One";
    pv1.Jql = "";
    pv1.Fields = {"summary", "labels"};
    pv1.ColumnOrder = {"id", "field:summary", "field:labels"};
    planeWs.Views.push_back(pv1);
    disk.Backends["Plane"] = planeWs;

    ConfigManager::SavePersistentViewsToDisk(disk);
    PersistentViewsFile loaded = ConfigManager::LoadPersistentViewsFromDisk();

    CHECK(loaded.Version == 2);
    REQUIRE(loaded.Backends.size() == 2);
    REQUIRE(loaded.Backends.count("Jira") == 1);
    REQUIRE(loaded.Backends.count("Plane") == 1);

    const ViewWorkspaceState& jLoaded = loaded.Backends["Jira"];
    CHECK(jLoaded.ActiveViewId == "jv2");
    REQUIRE(jLoaded.Views.size() == 2);
    CHECK(jLoaded.Views[0].Id == "jv1");
    CHECK(jLoaded.Views[0].Name == "Jira One");
    CHECK(jLoaded.Views[0].Jql == "project = A");
    CHECK(jLoaded.Views[0].Fields == jv1.Fields);
    CHECK(jLoaded.Views[0].ColumnOrder == jv1.ColumnOrder);
    REQUIRE(jLoaded.Views[0].ColumnWidths.count("field:summary") == 1);
    CHECK(jLoaded.Views[0].ColumnWidths.at("field:summary") == doctest::Approx(320.5f));
    // Direction==0 sort specs are dropped on serialize; only the ascending spec survives.
    REQUIRE(jLoaded.Views[0].SortSpecs.size() == 1);
    CHECK(jLoaded.Views[0].SortSpecs[0].ColumnKey == "field:status");
    CHECK(jLoaded.Views[0].SortSpecs[0].Direction == 1);
    CHECK(jLoaded.Views[1].Id == "jv2");

    const ViewWorkspaceState& pLoaded = loaded.Backends["Plane"];
    CHECK(pLoaded.ActiveViewId == "pv1");
    REQUIRE(pLoaded.Views.size() == 1);
    CHECK(pLoaded.Views[0].Id == "pv1");
    CHECK(pLoaded.Views[0].Fields == pv1.Fields);
}

// ---------------------------------------------------------------------------
// THE core multi-grid invariant. Bootstrapping / writing the Plane bucket must
// leave the Jira bucket byte-for-byte intact (and vice versa). The multi-pane
// refactor (multi-grid-tabs.md) builds directly on this guarantee — if this
// test breaks, per-pane view state can silently clobber a sibling pane's.
// ---------------------------------------------------------------------------
TEST_CASE("MULTI-BUCKET COEXISTENCE: bootstrapping the Plane bucket must NOT clobber Jira's customized bucket" *
          doctest::test_suite("[high-risk]")) {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    // 1. Bootstrap + customize the Jira bucket.
    TrackerConfig jiraCfg;
    jiraCfg.TrackerType = "Jira";
    jiraCfg.JqlQuery = "project = CUST";
    ViewsStore jiraStore = ConfigManager::LoadViewsOrBootstrap(jiraCfg);
    REQUIRE(jiraStore.Views.size() == 1);

    PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
    REQUIRE(disk.Backends.count("Jira") == 1);
    disk.Backends["Jira"].Views[0].Name = "My Custom Jira View";
    disk.Backends["Jira"].Views[0].ColumnWidths["field:summary"] = 250.0f;
    ConfigManager::SavePersistentViewsToDisk(disk);

    // 2. Bootstrap the Plane bucket (dirty -> rewrites the whole file).
    TrackerConfig planeCfg;
    planeCfg.TrackerType = "Plane";
    ViewsStore planeStore = ConfigManager::LoadViewsOrBootstrap(planeCfg);
    REQUIRE(planeStore.Views.size() == 1);
    CHECK(planeStore.Views[0].Id == "plane_default_view");

    // 3. Reload from disk — BOTH buckets present, Jira's customization intact.
    PersistentViewsFile after = ConfigManager::LoadPersistentViewsFromDisk();
    REQUIRE(after.Backends.count("Jira") == 1);
    REQUIRE(after.Backends.count("Plane") == 1);
    const ViewWorkspaceState& jiraAfter = after.Backends["Jira"];
    REQUIRE(jiraAfter.Views.size() == 1);
    CHECK(jiraAfter.Views[0].Name == "My Custom Jira View");
    CHECK(jiraAfter.Views[0].Jql == "project = CUST");
    REQUIRE(jiraAfter.Views[0].ColumnWidths.count("field:summary") == 1);
    CHECK(jiraAfter.Views[0].ColumnWidths.at("field:summary") == doctest::Approx(250.0f));

    // 4. Symmetric direction: re-loading Jira after Plane was bootstrapped
    //    must hand back the customized Jira slice, and must not dirty / mutate
    //    the Plane bucket.
    ViewsStore jiraReloaded = ConfigManager::LoadViewsOrBootstrap(jiraCfg);
    REQUIRE(jiraReloaded.Views.size() == 1);
    CHECK(jiraReloaded.Views[0].Name == "My Custom Jira View");
    PersistentViewsFile final = ConfigManager::LoadPersistentViewsFromDisk();
    REQUIRE(final.Backends.count("Plane") == 1);
    CHECK(final.Backends["Plane"].Views.size() == 1);
    CHECK(final.Backends["Plane"].Views[0].Id == "plane_default_view");
}

TEST_CASE("ConfigManager legacy v1 views file migrates into the Jira bucket on load") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    WriteViewsFileRaw(R"({
        "active_view_id": "legacy_view",
        "views": [
            {"id": "legacy_view", "name": "Legacy", "jql": "project = L",
             "fields": ["summary", "status"],
             "column_order": ["id", "field:summary", "field:status"],
             "column_widths": {"id": 80.0}},
            {"id": "second_view", "name": "Second", "jql": "project = M",
             "fields": ["summary"]}
        ]
    })");

    PersistentViewsFile disk = ConfigManager::LoadPersistentViewsFromDisk();
    REQUIRE(disk.Backends.size() == 1);
    REQUIRE(disk.Backends.count("Jira") == 1);
    const ViewWorkspaceState& ws = disk.Backends["Jira"];
    CHECK(ws.ActiveViewId == "legacy_view");
    REQUIRE(ws.Views.size() == 2);
    CHECK(ws.Views[0].Id == "legacy_view");
    CHECK(ws.Views[0].Jql == "project = L");
    CHECK(ws.Views[1].Id == "second_view");

    // LoadViewsOrBootstrap on a Jira config returns the migrated legacy views,
    // not a fresh bootstrap default.
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    ViewsStore store = ConfigManager::LoadViewsOrBootstrap(cfg);
    REQUIRE(store.Views.size() == 2);
    CHECK(store.ActiveViewId == "legacy_view");
    CHECK(store.Views[0].Name == "Legacy");

    SUBCASE("legacy v1 file without active_view_id promotes the first view") {
        WriteViewsFileRaw(R"({"views": [{"id": "only", "name": "Only", "fields": ["summary"]}]})");
        PersistentViewsFile d2 = ConfigManager::LoadPersistentViewsFromDisk();
        REQUIRE(d2.Backends.count("Jira") == 1);
        CHECK(d2.Backends["Jira"].ActiveViewId == "only");
    }

    SUBCASE("malformed JSON yields an empty backends map (graceful degradation)") {
        WriteViewsFileRaw("{not json!!");
        PersistentViewsFile d3 = ConfigManager::LoadPersistentViewsFromDisk();
        CHECK(d3.Backends.empty());
        CHECK(d3.Version == 2);
    }
}

TEST_CASE("ConfigManager::EnsureViewBucketBootstrapped is idempotent and repairs an empty active id") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.JqlQuery = "";

    PersistentViewsFile disk;
    bool dirty = false;
    ConfigManager::EnsureViewBucketBootstrapped(disk, "Jira", cfg, dirty);
    CHECK(dirty == true);
    REQUIRE(disk.Backends.count("Jira") == 1);
    REQUIRE(disk.Backends["Jira"].Views.size() == 1);
    CHECK(disk.Backends["Jira"].ActiveViewId == "default_view");

    // Second call: bucket already populated — no dirty flip, no content change.
    bool dirty2 = false;
    ConfigManager::EnsureViewBucketBootstrapped(disk, "Jira", cfg, dirty2);
    CHECK(dirty2 == false);
    CHECK(disk.Backends["Jira"].Views.size() == 1);
    CHECK(disk.Backends["Jira"].ActiveViewId == "default_view");

    // A populated bucket whose ActiveViewId went empty gets repaired to the front view.
    disk.Backends["Jira"].ActiveViewId.clear();
    bool dirty3 = false;
    ConfigManager::EnsureViewBucketBootstrapped(disk, "Jira", cfg, dirty3);
    CHECK(dirty3 == true);
    CHECK(disk.Backends["Jira"].ActiveViewId == "default_view");
    CHECK(disk.Backends["Jira"].Views.size() == 1);

    // Bootstrapping a SECOND bucket leaves the first untouched.
    bool dirty4 = false;
    ConfigManager::EnsureViewBucketBootstrapped(disk, "Plane", cfg, dirty4);
    CHECK(dirty4 == true);
    REQUIRE(disk.Backends.count("Plane") == 1);
    CHECK(disk.Backends["Plane"].Views.size() == 1);
    CHECK(disk.Backends["Jira"].Views.size() == 1);
    CHECK(disk.Backends["Jira"].ActiveViewId == "default_view");
}
