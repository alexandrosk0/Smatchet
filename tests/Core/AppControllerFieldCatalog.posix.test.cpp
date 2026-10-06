// AppController::RefreshFieldCatalog runs on a worker and writes the catalog of the pane it latched.
// A pane that switches tracker while the fetch is in flight must not receive the old tracker's
// catalog. Linked into SmatchetCommandsTests: it needs the real (headless) AppController, which
// only the posix-core-check archive provides.

#include <doctest/doctest.h>

#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "AppController.h"
#include "Config/CacheBackendKeyPure.h"
#include "ConfigManager.h"
#include "GridContextDepsAdapter.h"
#include "Tracker/FieldCatalogCache.h"
#include "Tracker/TrackerFieldSchema.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/// Runs `OnFetch` inside the catalog fetch, before answering it: the window in which a live
/// tracker switch lands on the UI thread in production.
class SwapDuringFetchBackend : public smatchet_tests::FakeTrackerClient {
  public:
    std::function<void()> OnFetch;
    bool Fail = false; ///< answer with a non-retryable error (read after OnFetch, so OnFetch may set it)

    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& cfg,
                                                                      const std::string& projectKey) override {
        if (OnFetch) {
            OnFetch();
        }
        if (Fail) {
            return Result<TrackerFieldCatalogResult, TrackerError>::Err(
                TrackerErrorInvalidRequest("the catalog is unavailable"));
        }
        return FakeTrackerClient::FetchFieldCatalog(cfg, projectKey);
    }
};

TrackerFieldCatalogResult CatalogWithField(const std::string& id) {
    TrackerFieldCatalogResult result;
    TrackerField field;
    field.Id = id;
    field.Name = "Field " + id;
    result.Fields.push_back(field);
    return result;
}

TrackerFieldCatalogResult OneFieldCatalog() { return CatalogWithField("customfield_10001"); }

bool HasField(const std::vector<TrackerField>& fields, const std::string& id) {
    for (const TrackerField& f : fields) {
        if (f.Id == id) {
            return true;
        }
    }
    return false;
}

/// True when the offline snapshot saved for `cfg` and `project` holds field `id`.
bool SnapshotHasField(const TrackerConfig& cfg, const std::string& project, const std::string& id) {
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    return FieldCatalogCache::TryLoadFieldCatalogSnapshot(FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, project),
                                                          fields, components, issueTypeMeta, error) &&
           HasField(fields, id);
}

} // namespace

TEST_CASE("AppController::RefreshFieldCatalog applies a fetched catalog to the pane it fetched for") {
    // Control for the two drop cases below: with nothing moving mid-fetch the result lands.
    smatchet_tests::OfflineQueueTestEnvGuard env; // snapshot + config writes go to a temp dir
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    adapter.SetBackend(std::move(backend));

    const std::uint64_t revisionBefore = app.GetFieldCatalogRevision();
    CHECK(app.RefreshFieldCatalog(ConfigManager::Load()));
    CHECK(HasField(app.GetAvailableFields(), "customfield_10001"));
    CHECK(app.GetFieldCatalogRevision() != revisionBefore);
}

TEST_CASE("AppController::RefreshFieldCatalog drops the result when the pane switches tracker mid-fetch") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);

    auto first = std::make_unique<SwapDuringFetchBackend>();
    first->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const firstRaw = first.get();
    adapter.SetBackend(std::move(first));
    firstRaw->OnFetch = [&adapter]() { adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>()); };

    const std::uint64_t revisionBefore = app.GetFieldCatalogRevision();
    CHECK_FALSE(app.RefreshFieldCatalog(ConfigManager::Load()));
    // The retired backend stays alive in the graveyard, so its call count is still readable.
    CHECK(firstRaw->FetchFieldCatalogCalls() == 1u);
    CHECK(app.GetAvailableFields().empty());
    CHECK(app.GetFieldCatalogRevision() == revisionBefore);
}

TEST_CASE("AppController::RefreshFieldCatalog drops the result when the catalog is cleared mid-fetch") {
    // Preferences' Save & Sync clears the catalog before the new backend is installed, so the pane's
    // generation has not moved yet; the clear alone must supersede the in-flight fetch.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    raw->OnFetch = [&app]() { app.SetFieldCatalog({}, {}, {}, std::string()); };

    CHECK_FALSE(app.RefreshFieldCatalog(ConfigManager::Load()));
    CHECK(raw->FetchFieldCatalogCalls() == 1u);
    CHECK_FALSE(HasField(app.GetAvailableFields(), "customfield_10001"));
}

TEST_CASE("AppController::RefreshFieldCatalog skips a project fetch for another tracker kind than the pane runs") {
    // A deferred Save & Sync leaves the old backend installed while the config already names the new
    // tracker: a project of the new tracker cannot be fetched through the old backend, so it does not run.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    adapter.SetCacheBackendKey("Plane"); // the pane still runs Plane

    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "Jira"; // the configuration already names Jira
    CHECK_FALSE(app.RefreshFieldCatalog(cfg, "PROJ"));
    CHECK(raw->FetchFieldCatalogCalls() == 0u);
    CHECK(app.GetAvailableFields().empty());
}

TEST_CASE("AppController::RefreshFieldCatalog refreshes the tracker the pane runs when the config names another") {
    // fields.refresh_catalog passes the saved configuration, which a backend override or a fixture backend
    // can leave naming another tracker than the pane runs: the unscoped refresh is for the pane's tracker.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    adapter.SetCacheBackendKey("Plane"); // an unconfigured Plane keeps the bare kind as its key

    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "Jira";
    CHECK(app.RefreshFieldCatalog(cfg));
    CHECK(raw->FetchFieldCatalogCalls() == 1u);
    CHECK(HasField(app.GetAvailableFields(), "customfield_10001"));
    // The offline snapshot is the pane's tracker's, never the configured one's.
    TrackerConfig planeCfg = cfg;
    planeCfg.TrackerType = "Plane";
    CHECK(SnapshotHasField(planeCfg, std::string(), "customfield_10001"));
    CHECK_FALSE(SnapshotHasField(cfg, std::string(), "customfield_10001"));
}

TEST_CASE("AppController::RefreshFieldCatalog skips an unscoped fetch whose site is not the pane's") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    adapter.SetCacheBackendKey("Plane@plane.other.example/ws"); // a site the configuration does not name

    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "Jira";
    CHECK_FALSE(app.RefreshFieldCatalog(cfg));
    CHECK(raw->FetchFieldCatalogCalls() == 0u);
    CHECK(app.GetAvailableFields().empty());
}

TEST_CASE("AppController::RefreshFieldCatalog skips a fetch for another site of the tracker the pane runs") {
    // Same tracker kind is not enough: a pane on another Jira site must not receive this site's catalog.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    adapter.SetCacheBackendKey("Jira@jira.other.example#0123456789ab");

    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "Jira";
    CHECK_FALSE(app.RefreshFieldCatalog(cfg));
    CHECK(raw->FetchFieldCatalogCalls() == 0u);
    CHECK(app.GetAvailableFields().empty());
}

TEST_CASE("AppController::RefreshFieldCatalog runs for a pane keyed by the configured site") {
    // The production norm: a configured tracker stamps its pane with a site key, and a refresh for that
    // same site runs.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));

    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "Jira";
    cfg.Domain = "https://acme.atlassian.net";
    cfg.Email = "dev@example.com";
    const std::string siteKey = smatchet::cache_keys::TrackerCacheBackendKey(cfg);
    REQUIRE(siteKey != "Jira"); // the configuration names a site
    adapter.SetCacheBackendKey(siteKey);
    CHECK(app.RefreshFieldCatalog(cfg));
    CHECK(raw->FetchFieldCatalogCalls() == 1u);
    CHECK(HasField(app.GetAvailableFields(), "customfield_10001"));
}

TEST_CASE("AppController::SetFieldCatalog restores a project's snapshot for a failure after a clear") {
    // A pane switch clears the catalog and its project. The grid's next fetch fails offline: naming
    // the fetch's project for that failure restores the project's saved catalog, not the unscoped one.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    const TrackerConfig cfg = ConfigManager::Load();
    app.SetCurrentCatalogProject("FOO");
    std::vector<TrackerField> foo(1);
    foo[0].Id = "customfield_foo";
    foo[0].Name = "Foo field";
    app.SetFieldCatalog(std::move(foo), std::vector<TrackerComponent>(), std::string(), false);
    REQUIRE(SnapshotHasField(cfg, "FOO", "customfield_foo"));

    app.SetFieldCatalog({}, {}, {}, std::string());
    CHECK(app.GetAvailableFields().empty());
    CHECK_FALSE(app.IsFieldCatalogScopedToProject("FOO")); // an empty catalog is no project's

    app.SetCurrentCatalogProject("FOO");
    app.SetFieldCatalog(std::vector<TrackerField>(), std::vector<TrackerComponent>(), "tracker unreachable", true);
    CHECK(HasField(app.GetAvailableFields(), "customfield_foo"));
    CHECK(app.IsFieldCatalogScopedToProject("FOO"));
    CHECK(app.GetFieldCatalogError().empty()); // a restored catalog shows a warning, not an error
}

TEST_CASE("AppController::SetFieldCatalog files the grid's catalog under the grid's project when a refresh lands "
          "in between") {
    // The grid applies its fetch in two calls (pin the project, then apply). A draft refresh for another
    // project landing between them must not lend the grid's catalog its project, in memory or on disk.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(CatalogWithField("customfield_b"));
    adapter.SetBackend(std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();

    app.SetCurrentCatalogProject("GRID");
    REQUIRE(app.RefreshFieldCatalog(cfg, "PROJB"));
    CHECK(app.IsFieldCatalogScopedToProject("PROJB"));
    std::vector<TrackerField> grid(1);
    grid[0].Id = "customfield_grid";
    grid[0].Name = "Grid field";
    app.SetFieldCatalog(std::move(grid), std::vector<TrackerComponent>(), std::string(), false);

    CHECK(HasField(app.GetAvailableFields(), "customfield_grid"));
    CHECK(app.IsFieldCatalogScopedToProject("GRID"));
    CHECK_FALSE(app.IsFieldCatalogScopedToProject("PROJB"));
    CHECK(SnapshotHasField(cfg, "GRID", "customfield_grid"));
    CHECK_FALSE(SnapshotHasField(cfg, "PROJB", "customfield_grid"));
}

TEST_CASE("AppController::RefreshFieldCatalog pins its project only with the catalog it fetched") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    bool scopedDuringFetch = true;
    raw->OnFetch = [&]() { scopedDuringFetch = app.IsFieldCatalogScopedToProject("PROJA"); };

    CHECK(app.RefreshFieldCatalog(ConfigManager::Load(), "PROJA"));
    CHECK_FALSE(scopedDuringFetch); // the catalog in memory was not PROJA's yet
    CHECK(app.IsFieldCatalogScopedToProject("PROJA"));
}

TEST_CASE("AppController::RefreshFieldCatalog keeps a project result when the grid's catalog lands mid-fetch") {
    // The grid's own fetch pins its project and applies its catalog on the UI thread; that is no newer
    // refresh, so the draft's project catalog still lands, scoped to the draft's project.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    raw->OnFetch = [&app]() {
        app.SetCurrentCatalogProject("GRID");
        std::vector<TrackerField> grid(1);
        grid[0].Id = "customfield_grid";
        grid[0].Name = "Grid field";
        app.SetFieldCatalog(std::move(grid), std::vector<TrackerComponent>(), std::string(), false);
    };

    CHECK(app.RefreshFieldCatalog(ConfigManager::Load(), "PROJA"));
    CHECK(HasField(app.GetAvailableFields(), "customfield_10001"));
    CHECK_FALSE(HasField(app.GetAvailableFields(), "customfield_grid"));
    CHECK(app.IsFieldCatalogScopedToProject("PROJA"));
}

TEST_CASE("AppController::RefreshFieldCatalog drops an older refresh's result once a newer one started") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();
    int fetches = 0;
    raw->OnFetch = [&]() {
        if (++fetches != 1) {
            return;
        }
        // The user picked project B while A's fetch was still running; B's answer lands first.
        raw->SetFieldCatalogResult(CatalogWithField("customfield_b"));
        CHECK(app.RefreshFieldCatalog(cfg, "PROJB"));
        raw->SetFieldCatalogResult(CatalogWithField("customfield_a"));
    };

    CHECK_FALSE(app.RefreshFieldCatalog(cfg, "PROJA"));
    CHECK(HasField(app.GetAvailableFields(), "customfield_b"));
    CHECK_FALSE(HasField(app.GetAvailableFields(), "customfield_a"));
    CHECK(app.IsFieldCatalogScopedToProject("PROJB"));
}

TEST_CASE("AppController::RefreshFieldCatalog drops a superseded failure without setting the error banner") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();
    int fetches = 0;
    raw->OnFetch = [&]() {
        if (++fetches != 1) {
            return;
        }
        raw->SetFieldCatalogResult(CatalogWithField("customfield_b"));
        CHECK(app.RefreshFieldCatalog(cfg, "PROJB"));
        raw->Fail = true; // the older fetch then fails
    };

    CHECK_FALSE(app.RefreshFieldCatalog(cfg, "PROJA"));
    CHECK(HasField(app.GetAvailableFields(), "customfield_b"));
    CHECK(app.GetFieldCatalogError().empty());
}

TEST_CASE("AppController::SetFieldCatalog clear empties the pane without publishing synthetic columns") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    adapter.SetBackend(std::move(backend));
    REQUIRE(app.RefreshFieldCatalog(ConfigManager::Load()));
    REQUIRE(HasField(app.GetAvailableFields(), "customfield_10001"));

    const std::uint64_t revisionBefore = app.GetFieldCatalogRevision();
    app.SetFieldCatalog({}, {}, {}, std::string());
    CHECK(app.GetAvailableFields().empty()); // no synthetic history/comments for a tracker not loaded yet
    CHECK(app.GetFieldCatalogRevision() != revisionBefore);
}
