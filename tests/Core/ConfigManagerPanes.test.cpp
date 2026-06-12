// smatchet_panes.json persistence + GridPane runtime isolation — Slice 2 of
// docs/plans/multi-grid-tabs.md (plan items 7 + 13, ADR-0018).
//
// Covers:
//   * GridPaneDescriptor serialize → restore round-trip (order + focused id kept).
//   * Default bootstrap (zero-migration): no panes file → exactly ONE pane derived
//     from cfg.TrackerType + that backend's active view, persisted to disk.
//   * Bootstrap idempotence: a second LoadPanesOrBootstrap re-reads the same pane.
//   * Malformed rows (missing id) skipped, the rest kept; missing focused id
//     repaired to the first pane.
//   * PER-PANE SORT-CACHE ISOLATION — two GridPane instances hold fully
//     independent sort/filter caches (the plan's "two panes, different sorts, no
//     thrash" invariant at the data-structure level) + FindGridPaneById lookup.
//
// Every case runs under TestEnvGuard so smatchet_panes.json / smatchet_views.json
// live in a private temp dir; PanesFilesCleanup removes the files the guard
// itself doesn't own.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"
#include "GridPane.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

/// Removes the panes + views files while the TestEnvGuard redirect is still
/// active. Declare AFTER the guard so this dtor runs first.
struct PanesFilesCleanup {
    ~PanesFilesCleanup() {
        std::remove(ConfigManager::GetPanesPath().c_str());
        std::remove(ConfigManager::GetViewsPath().c_str());
    }
};

void WritePanesFileRaw(const std::string& content) {
    std::ofstream f(ConfigManager::GetPanesPath(), std::ios::binary | std::ios::trunc);
    f << content;
}

bool FileExistsAt(const std::string& path) {
    std::ifstream f(path);
    return f.is_open();
}

} // namespace

TEST_CASE("ConfigManager panes: save -> load round-trips ordered panes + focused id") {
    smatchet_tests::TestEnvGuard env;
    PanesFilesCleanup cleanup;

    PersistentPanesFile out;
    GridPaneDescriptor a;
    a.Id = "main";
    a.Title = "Default View";
    a.BackendKey = "Jira";
    a.ViewId = "default_view";
    GridPaneDescriptor b;
    b.Id = "pane-2";
    b.Title = "Plane Bugs";
    b.BackendKey = "Plane";
    b.ViewId = "plane_default_view";
    out.Panes.push_back(a);
    out.Panes.push_back(b);
    out.FocusedPaneId = "pane-2";
    ConfigManager::SavePanesToDisk(out);

    const PersistentPanesFile in = ConfigManager::LoadPanesFromDisk();
    REQUIRE(in.Panes.size() == 2);
    CHECK(in.FocusedPaneId == "pane-2");
    CHECK(in.Panes[0].Id == "main");
    CHECK(in.Panes[0].Title == "Default View");
    CHECK(in.Panes[0].BackendKey == "Jira");
    CHECK(in.Panes[0].ViewId == "default_view");
    CHECK(in.Panes[1].Id == "pane-2");
    CHECK(in.Panes[1].Title == "Plane Bugs");
    CHECK(in.Panes[1].BackendKey == "Plane");
    CHECK(in.Panes[1].ViewId == "plane_default_view");
}

TEST_CASE("ConfigManager panes: fresh install default-bootstraps ONE pane from cfg + active view (zero-migration)") {
    smatchet_tests::TestEnvGuard env;
    PanesFilesCleanup cleanup;

    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.JqlQuery = "";

    const PersistentPanesFile disk = ConfigManager::LoadPanesOrBootstrap(cfg);
    REQUIRE(disk.Panes.size() == 1);
    CHECK(disk.Panes[0].Id == "main");
    CHECK(disk.Panes[0].BackendKey == "Jira");
    // Jira default views bucket activates "default_view" named "Default View".
    CHECK(disk.Panes[0].ViewId == "default_view");
    CHECK(disk.Panes[0].Title == "Default View");
    CHECK(disk.FocusedPaneId == "main");
    // Bootstrap persists — the file now exists on disk.
    CHECK(FileExistsAt(ConfigManager::GetPanesPath()));

    // Idempotence: a second call re-reads the SAME pane (no second bootstrap).
    const PersistentPanesFile again = ConfigManager::LoadPanesOrBootstrap(cfg);
    REQUIRE(again.Panes.size() == 1);
    CHECK(again.Panes[0].Id == "main");
    CHECK(again.FocusedPaneId == "main");
}

TEST_CASE("ConfigManager panes: Plane config bootstraps the Plane bucket's active view") {
    smatchet_tests::TestEnvGuard env;
    PanesFilesCleanup cleanup;

    TrackerConfig cfg;
    cfg.TrackerType = "plane"; // case-insensitive normalization

    const PersistentPanesFile disk = ConfigManager::LoadPanesOrBootstrap(cfg);
    REQUIRE(disk.Panes.size() == 1);
    CHECK(disk.Panes[0].BackendKey == "Plane");
    CHECK(disk.Panes[0].ViewId == "plane_default_view");
    CHECK(disk.Panes[0].Title == "Default Plane View");
}

TEST_CASE("ConfigManager panes: malformed rows skipped, missing focused id repaired to first pane") {
    smatchet_tests::TestEnvGuard env;
    PanesFilesCleanup cleanup;

    WritePanesFileRaw(R"({
        "version": 1,
        "focused_pane_id": "ghost",
        "panes": [
            {"title": "no id - dropped", "backend_key": "Jira", "view_id": "v"},
            {"id": "p1", "title": "Kept", "backend_key": "Jira", "view_id": "default_view"},
            "not-an-object",
            {"id": "p2", "title": "Also kept", "backend_key": "GitHub", "view_id": "github_default_view"}
        ]
    })");

    const PersistentPanesFile in = ConfigManager::LoadPanesFromDisk();
    REQUIRE(in.Panes.size() == 2);
    CHECK(in.Panes[0].Id == "p1");
    CHECK(in.Panes[1].Id == "p2");
    // "ghost" survives the parse (repair happens at the UI bootstrap layer), but an
    // EMPTY focused id is repaired to the first pane:
    CHECK(in.FocusedPaneId == "ghost");

    WritePanesFileRaw(R"({"version":1,"panes":[{"id":"p9","title":"t","backend_key":"Jira","view_id":"v"}]})");
    const PersistentPanesFile repaired = ConfigManager::LoadPanesFromDisk();
    REQUIRE(repaired.Panes.size() == 1);
    CHECK(repaired.FocusedPaneId == "p9");
}

TEST_CASE("ConfigManager panes: corrupt JSON falls back to empty file (bootstrap then reseeds)") {
    smatchet_tests::TestEnvGuard env;
    PanesFilesCleanup cleanup;

    WritePanesFileRaw("{ this is not json");
    const PersistentPanesFile in = ConfigManager::LoadPanesFromDisk();
    CHECK(in.Panes.empty());

    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    const PersistentPanesFile disk = ConfigManager::LoadPanesOrBootstrap(cfg);
    REQUIRE(disk.Panes.size() == 1); // never an empty pane list (Pillar 3)
    CHECK(disk.Panes[0].Id == "main");
}

TEST_CASE("GridPane: per-pane sort/filter caches are fully isolated (two panes, different sorts, no thrash)") {
    GridPane a;
    a.id = "main";
    GridPane b;
    b.id = "pane-2";

    // Pane A sorts by one fingerprint over revision 7; pane B by another over 9.
    a.cachedSortFingerprint = "0:1:0|";
    a.cachedSortTicketsRevision = 7;
    a.cachedSortValid = true;
    a.cachedSortedIndices = {2, 0, 1};
    a.filteredIndices = {2, 0};
    std::snprintf(a.gridFilterBuf, sizeof(a.gridFilterBuf), "%s", "alpha");

    b.cachedSortFingerprint = "1:2:0|";
    b.cachedSortTicketsRevision = 9;
    b.cachedSortValid = true;
    b.cachedSortedIndices = {0, 1, 2};
    b.filteredIndices = {0, 1, 2};

    // Mutating A further must not disturb B (the old singleton fields would have).
    a.cachedSortValid = false;
    a.cachedSortedIndices.clear();
    a.gridState.ActiveIssueId = "SMAT-1";
    a.gridState.RectSel.Rows.insert(1);

    CHECK(b.cachedSortValid == true);
    CHECK(b.cachedSortFingerprint == "1:2:0|");
    CHECK(b.cachedSortTicketsRevision == 9);
    CHECK(b.cachedSortedIndices.size() == 3);
    CHECK(b.filteredIndices.size() == 3);
    CHECK(b.gridState.ActiveIssueId.empty());
    CHECK(b.gridState.RectSel.Rows.empty());
    CHECK(b.gridFilterBuf[0] == '\0');

    // Lookup helper resolves by id (and misses cleanly).
    std::vector<GridPane> panes;
    panes.push_back(GridPane());
    panes.back().id = "main";
    panes.push_back(GridPane());
    panes.back().id = "pane-2";
    CHECK(FindGridPaneById(panes, "pane-2") == &panes[1]);
    CHECK(FindGridPaneById(panes, "absent") == nullptr);
}
