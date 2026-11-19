// AppController::RefreshFieldCatalog runs on a worker and writes the catalog of the pane it latched.
// A pane that switches tracker while the fetch is in flight must not receive the old tracker's
// catalog. Linked into SmatchetCommandsTests: it needs the real (headless) AppController, which
// only the posix-core-check archive provides.

#include <doctest/doctest.h>

#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "AppController.h"
#include "ComponentOptionsPickPure.h"
#include "Config/CacheBackendKeyPure.h"
#include "ConfigManager.h"
#include "GridContextDepsAdapter.h"
#include "GridContextFieldCatalogOps.h"
#include "GridLiveContext.h"
#include "Tracker/FieldCatalogCache.h"
#include "Tracker/JqlProjectScope.h"
#include "Tracker/TrackerFieldSchema.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

/// SnapshotHasField once the app's queued snapshot saves (they run on a worker) have reached the file.
bool SnapshotHasField(AppController& app, const TrackerConfig& cfg, const std::string& project, const std::string& id) {
    REQUIRE(app.FlushFieldCatalogSnapshotIo());
    return SnapshotHasField(cfg, project, id);
}

/// A UI-thread SetFieldCatalog failure with nothing in memory reads the saved catalog on a worker and installs it
/// from the main-thread queue: wait for the read, then run the install.
void SettleCatalogRestore(AppController& app) {
    REQUIRE(app.FlushFieldCatalogSnapshotIo());
    app.mainThreadDispatcher.Drain();
}

/// The grid's catalog apply as production runs it: latch the focused pane as the fetch starts, then apply the
/// result for `project`. True when a catalog was applied.
bool ApplyGridCatalog(AppController& app, const TrackerConfig& cfg, const std::string& project,
                      std::vector<TrackerField> fields, const std::string& error = std::string(),
                      bool errorTransient = false) {
    return app.ApplyGridFieldCatalogResult(app.LatchGridCatalogFetch(), cfg, project, std::move(fields), {}, {}, error,
                                           errorTransient);
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
    CHECK(HasField(*app.GetAvailableFields(), "customfield_10001"));
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
    CHECK(app.GetAvailableFields()->empty());
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
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "customfield_10001"));
}

TEST_CASE("AppController::RefreshFieldCatalog drops the result when ClearFieldCatalogInMemory runs mid-fetch") {
    // A site or backend switch clears through ClearFieldCatalogInMemory; like SetFieldCatalog's explicit
    // clear, it must supersede the fetch already in flight.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    raw->OnFetch = [&app]() { app.ClearFieldCatalogInMemory(); };

    CHECK_FALSE(app.RefreshFieldCatalog(ConfigManager::Load()));
    CHECK(raw->FetchFieldCatalogCalls() == 1u);
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "customfield_10001"));
}

TEST_CASE("AppController::ClearFieldCatalogInMemory drops the project pin and the user roster too") {
    // Both clears empty the catalog and forget its project; only the site-switch clear also drops the
    // previous site's user roster, which must not autocomplete under the next site.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    const TrackerConfig cfg = ConfigManager::Load();
    std::vector<TrackerUser> users(1);
    users[0].AccountId = "acct-1";
    users[0].DisplayName = "Ann";
    const auto fooCatalog = []() {
        std::vector<TrackerField> foo(1);
        foo[0].Id = "customfield_foo";
        foo[0].Name = "Foo field";
        return foo;
    };

    REQUIRE(ApplyGridCatalog(app, cfg, "FOO", fooCatalog()));
    app.SetAvailableUsers(users);
    REQUIRE(app.IsFieldCatalogScopedToProject("FOO"));
    app.SetFieldCatalog({}, {}, {}, std::string());
    CHECK(app.GetAvailableFields()->empty());
    CHECK_FALSE(app.IsFieldCatalogScopedToProject("FOO"));
    CHECK(app.GetAvailableUsers().size() == 1u);

    REQUIRE(ApplyGridCatalog(app, cfg, "FOO", fooCatalog()));
    REQUIRE(app.IsFieldCatalogScopedToProject("FOO"));
    app.ClearFieldCatalogInMemory();
    CHECK(app.GetAvailableFields()->empty());
    CHECK_FALSE(app.IsFieldCatalogScopedToProject("FOO"));
    CHECK(app.GetAvailableUsers().empty());
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
    CHECK(app.GetAvailableFields()->empty());
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
    CHECK(HasField(*app.GetAvailableFields(), "customfield_10001"));
    // The offline snapshot is the pane's tracker's, never the configured one's.
    TrackerConfig planeCfg = cfg;
    planeCfg.TrackerType = "Plane";
    CHECK(SnapshotHasField(app, planeCfg, std::string(), "customfield_10001"));
    CHECK_FALSE(SnapshotHasField(app, cfg, std::string(), "customfield_10001"));
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
    CHECK(app.GetAvailableFields()->empty());
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
    CHECK(app.GetAvailableFields()->empty());
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
    CHECK(HasField(*app.GetAvailableFields(), "customfield_10001"));
}

TEST_CASE("AppController::ApplyGridFieldCatalogResult falls back to the unscoped snapshot when the project has none") {
    // As the startup restore does: a project fetched for the first time offline (or evicted from the
    // snapshot cache) still gets the saved unscoped catalog, which claims no project.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    const TrackerConfig cfg = ConfigManager::Load();
    REQUIRE(ApplyGridCatalog(app, cfg, std::string(), CatalogWithField("customfield_any").Fields));
    REQUIRE(SnapshotHasField(app, cfg, std::string(), "customfield_any"));
    REQUIRE_FALSE(SnapshotHasField(app, cfg, "FOO", "customfield_any"));

    app.SetFieldCatalog({}, {}, {}, std::string());
    CHECK_FALSE(ApplyGridCatalog(app, cfg, "FOO", {}, "tracker unreachable", true));
    SettleCatalogRestore(app);
    CHECK(HasField(*app.GetAvailableFields(), "customfield_any"));
    CHECK_FALSE(app.IsFieldCatalogScopedToProject("FOO"));
    CHECK(app.GetFieldCatalogError().empty());
}

TEST_CASE("AppController::ApplyGridFieldCatalogResult prefers the project's snapshot to the unscoped one") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    const TrackerConfig cfg = ConfigManager::Load();
    REQUIRE(ApplyGridCatalog(app, cfg, std::string(), CatalogWithField("customfield_any").Fields));
    REQUIRE(ApplyGridCatalog(app, cfg, "FOO", CatalogWithField("customfield_foo").Fields));
    REQUIRE(SnapshotHasField(app, cfg, std::string(), "customfield_any"));
    REQUIRE(SnapshotHasField(app, cfg, "FOO", "customfield_foo"));

    app.SetFieldCatalog({}, {}, {}, std::string());
    CHECK_FALSE(ApplyGridCatalog(app, cfg, "FOO", {}, "tracker unreachable", true));
    SettleCatalogRestore(app);
    CHECK(HasField(*app.GetAvailableFields(), "customfield_foo"));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "customfield_any"));
    CHECK(app.IsFieldCatalogScopedToProject("FOO"));
}

TEST_CASE("AppController::RefreshFieldCatalog falls back to the unscoped snapshot for a failed project fetch") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(CatalogWithField("customfield_any"));
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();
    REQUIRE(app.RefreshFieldCatalog(cfg));
    REQUIRE(SnapshotHasField(app, cfg, std::string(), "customfield_any"));

    app.SetFieldCatalog({}, {}, {}, std::string());
    raw->Fail = true;
    CHECK_FALSE(app.RefreshFieldCatalog(cfg, "FOO"));
    CHECK(HasField(*app.GetAvailableFields(), "customfield_any"));
    CHECK_FALSE(app.IsFieldCatalogScopedToProject("FOO"));
}

TEST_CASE("AppController::RefreshFieldCatalog never restores another tracker's snapshot for a failure") {
    // A GitHub pane beside a Jira configuration refreshes for GitHub. Its catalog is saved under GitHub's
    // own key: a Jira project that then fails offline must not restore it.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TrackerConfig jira = ConfigManager::Load();
    jira.TrackerType = "Jira";
    jira.Domain = "https://acme.atlassian.net";
    jira.Email = "dev@example.com";
    jira.GitHubOwner = "octo";
    jira.GitHubRepo = "repo";
    TrackerConfig github = jira;
    github.TrackerType = "GitHub";
    CHECK(FieldCatalogCache::BuildFieldCatalogCacheKey(github, std::string()) !=
          FieldCatalogCache::BuildFieldCatalogCacheKey(jira, std::string()));
    {
        AppController githubApp;
        GridContextDepsAdapter githubAdapter(githubApp);
        auto backend = std::make_unique<SwapDuringFetchBackend>();
        backend->SetFieldCatalogResult(CatalogWithField("pr.head"));
        githubAdapter.SetBackend(std::move(backend));
        githubAdapter.SetCacheBackendKey(smatchet::cache_keys::TrackerCacheBackendKey(github));
        REQUIRE(githubApp.RefreshFieldCatalog(jira)); // unscoped: runs for the pane's own tracker
        REQUIRE(HasField(*githubApp.GetAvailableFields(), "pr.head"));
    }
    CHECK(SnapshotHasField(github, std::string(), "pr.head"));
    CHECK_FALSE(SnapshotHasField(jira, std::string(), "pr.head"));
    // Preferences lists the snapshot under GitHub and its repository, not under the Jira site.
    const FieldCatalogCache::FieldCatalogIndexIdentity githubIndex =
        FieldCatalogCache::BuildFieldCatalogIndexIdentity(github);
    CHECK(githubIndex.backend == "GitHub");
    bool listed = false;
    for (const FieldCatalogCache::CachedProjectEntry& e : FieldCatalogCache::ListCachedProjects()) {
        listed = listed || (e.backend == githubIndex.backend && e.endpoint == githubIndex.endpoint);
    }
    CHECK(listed);

    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->Fail = true;
    adapter.SetBackend(std::move(backend));
    adapter.SetCacheBackendKey(smatchet::cache_keys::TrackerCacheBackendKey(jira));
    CHECK_FALSE(app.RefreshFieldCatalog(jira, "FOO"));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "pr.head"));
}

TEST_CASE("AppController::RefreshFieldCatalog never falls back to a snapshot an older build saved") {
    // Older builds saved a GitHub pane's catalog under the Jira site's unscoped key: that snapshot may be
    // another tracker's, so a failed Jira project fetch does not restore it.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TrackerConfig jira = ConfigManager::Load();
    jira.TrackerType = "Jira";
    jira.Domain = "https://acme.atlassian.net";
    const std::string unscopedKey = FieldCatalogCache::BuildFieldCatalogCacheKey(jira, std::string());
    {
        std::ofstream out(ConfigManager::GetUserDataDirectory() + "smatchet_field_catalog_cache.json",
                          std::ios::binary | std::ios::trunc);
        out << "{\"schema_version\":3,\"entries\":[{\"cacheKey\":\"" << unscopedKey
            << "\",\"projectKey\":\"\",\"backend\":\"Jira\",\"endpoint\":\"" << jira.Domain
            << "\",\"lastUsedUnix\":100}],\"" << unscopedKey
            << "\":{\"fields\":[{\"id\":\"pr.head\",\"name\":\"PR Head Branch\"}]}}";
    }
    REQUIRE_FALSE(SnapshotHasField(jira, std::string(), "pr.head")); // the cache refuses it to every reader

    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->Fail = true;
    adapter.SetBackend(std::move(backend));
    CHECK_FALSE(app.RefreshFieldCatalog(jira, "FOO"));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "pr.head"));
}

TEST_CASE("AppController::RefreshFieldCatalog never restores an older build's unscoped Jira snapshot") {
    // The unscoped refresh restores the unscoped key directly: the same older-build snapshot is refused.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    TrackerConfig jira = ConfigManager::Load();
    jira.TrackerType = "Jira";
    jira.Domain = "https://acme.atlassian.net";
    const std::string unscopedKey = FieldCatalogCache::BuildFieldCatalogCacheKey(jira, std::string());
    {
        std::ofstream out(ConfigManager::GetUserDataDirectory() + "smatchet_field_catalog_cache.json",
                          std::ios::binary | std::ios::trunc);
        out << "{\"schema_version\":3,\"entries\":[],\"" << unscopedKey
            << "\":{\"fields\":[{\"id\":\"pr.head\",\"name\":\"PR Head Branch\"}]}}";
    }

    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->Fail = true;
    adapter.SetBackend(std::move(backend));
    CHECK_FALSE(app.RefreshFieldCatalog(jira));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "pr.head"));
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
    // The grid's own fetch applies its catalog on the UI thread; that is no newer refresh, so the draft's
    // project catalog still lands, scoped to the draft's project.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();
    raw->OnFetch = [&app, &cfg]() {
        CHECK(ApplyGridCatalog(app, cfg, "GRID", CatalogWithField("customfield_grid").Fields));
    };

    CHECK(app.RefreshFieldCatalog(cfg, "PROJA"));
    CHECK(HasField(*app.GetAvailableFields(), "customfield_10001"));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "customfield_grid"));
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
    CHECK(HasField(*app.GetAvailableFields(), "customfield_b"));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "customfield_a"));
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
    CHECK(HasField(*app.GetAvailableFields(), "customfield_b"));
    CHECK(app.GetFieldCatalogError().empty());
}

namespace {

/// Installs a backend in the pane, saves a catalog with one field under project FOO's snapshot, then empties the
/// pane: the state in which a failed fetch for FOO restores the saved catalog (on a worker, landing later on the UI
/// thread).
void SaveFooSnapshotAndClear(AppController& app, GridContextDepsAdapter& adapter, const TrackerConfig& cfg) {
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    REQUIRE(ApplyGridCatalog(app, cfg, "FOO", CatalogWithField("customfield_foo").Fields));
    REQUIRE(SnapshotHasField(app, cfg, "FOO", "customfield_foo"));
    app.SetFieldCatalog({}, {}, {}, std::string());
    REQUIRE(app.GetAvailableFields()->empty());
}

/// The grid's offline failure for FOO (the fetch's project), restoring FOO's saved catalog.
void FailFooFetchOffline(AppController& app, const TrackerConfig& cfg) {
    CHECK_FALSE(ApplyGridCatalog(app, cfg, "FOO", {}, "tracker unreachable", true));
}

} // namespace

TEST_CASE("AppController::ApplyGridFieldCatalogResult drops a failed fetch's restore when the pane switched tracker "
          "meanwhile") {
    // The restore reads the saved catalog on a worker and installs it from the main-thread queue. If the pane's
    // backend was swapped in between, the saved catalog is the OLD tracker's: it must not land in the new one's.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    const TrackerConfig cfg = ConfigManager::Load();
    SaveFooSnapshotAndClear(app, adapter, cfg);

    FailFooFetchOffline(app, cfg);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>()); // the pane switches tracker
    const std::uint64_t revisionBefore = app.GetFieldCatalogRevision();
    SettleCatalogRestore(app);

    CHECK(app.GetAvailableFields()->empty());
    CHECK(app.GetFieldCatalogError().empty()); // no banner either: the failure was the old tracker's
    CHECK(app.GetFieldCatalogRevision() == revisionBefore);
}

TEST_CASE("AppController::ApplyGridFieldCatalogResult drops a failed fetch's restore when the pane switched site "
          "meanwhile") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    const TrackerConfig cfg = ConfigManager::Load();
    SaveFooSnapshotAndClear(app, adapter, cfg);

    FailFooFetchOffline(app, cfg);
    adapter.SetCacheBackendKey("Jira@another-site.example"); // same tracker kind, another site: the key moved
    SettleCatalogRestore(app);

    CHECK(app.GetAvailableFields()->empty());
    CHECK(app.GetFieldCatalogError().empty());
}

TEST_CASE("AppController::HandleFieldCatalogError drops a restore when the pane switched tracker meanwhile") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    const TrackerConfig cfg = ConfigManager::Load();
    SaveFooSnapshotAndClear(app, adapter, cfg);

    app.HandleFieldCatalogError("tracker unreachable", true, FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, "FOO"),
                                "Jira");
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    SettleCatalogRestore(app);

    CHECK(app.GetAvailableFields()->empty());
    CHECK(app.GetFieldCatalogError().empty());
}

TEST_CASE("AppController::IsFieldCatalogRestoring is per pane: a restore for one pane does not show on another") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    const TrackerConfig cfg = ConfigManager::Load();
    GridLiveContext* const other = app.EnsurePaneContextLive("other-pane", "Jira");
    REQUIRE(other != nullptr);
    CHECK_FALSE(app.IsFieldCatalogRestoring());
    CHECK_FALSE(app.IsFieldCatalogRestoring("other-pane"));
    SaveFooSnapshotAndClear(app, adapter, cfg);

    FailFooFetchOffline(app, cfg);
    CHECK(app.IsFieldCatalogRestoring());                   // the focused pane is reading its saved catalog
    CHECK_FALSE(app.IsFieldCatalogRestoring("other-pane")); // the other pane is not
    SettleCatalogRestore(app);
    CHECK_FALSE(app.IsFieldCatalogRestoring());
    CHECK(HasField(*app.GetAvailableFields(), "customfield_foo"));
}

TEST_CASE("AppController::HandleFieldCatalogError still restores when the pane did not move") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    const TrackerConfig cfg = ConfigManager::Load();
    SaveFooSnapshotAndClear(app, adapter, cfg);

    app.HandleFieldCatalogError("tracker unreachable", true, FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, "FOO"),
                                "Jira");
    SettleCatalogRestore(app);

    CHECK(HasField(*app.GetAvailableFields(), "customfield_foo"));
}

TEST_CASE("AppController::SetFieldCatalog clear empties the pane without publishing synthetic columns") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    adapter.SetBackend(std::move(backend));
    REQUIRE(app.RefreshFieldCatalog(ConfigManager::Load()));
    REQUIRE(HasField(*app.GetAvailableFields(), "customfield_10001"));

    const std::uint64_t revisionBefore = app.GetFieldCatalogRevision();
    app.SetFieldCatalog({}, {}, {}, std::string());
    CHECK(app.GetAvailableFields()->empty()); // no synthetic history/comments for a tracker not loaded yet
    CHECK(app.GetFieldCatalogRevision() != revisionBefore);
}

TEST_CASE("ClearCatalogInMemoryInto drops a restored snapshot's state with the catalog") {
    // A pane showing a restored snapshot reads it while fieldCatalogRestored_ is set; once the catalog is
    // cleared the flag must go too, or the pane keeps reading an empty catalog as populated.
    for (const bool clearUsers : {false, true}) {
        GridContextFieldCatalog cat;
        cat.AvailableFields = std::make_shared<std::vector<TrackerField>>(1);
        cat.AvailableUsers.resize(1);
        cat.fieldCatalogRestored_ = true;
        cat.currentCatalogProjectKey_ = "FOO";
        cat.LastTrackerFieldCatalogWarning = "Offline: restored";
        const std::uint64_t epochBefore = cat.CatalogEpoch.load();
        const std::uint64_t revisionBefore = cat.TrackerFieldCatalogRevision.load();

        smatchet::catalogops::ClearCatalogInMemoryInto(cat, clearUsers);
        CHECK(cat.AvailableFields->empty());
        CHECK_FALSE(cat.fieldCatalogRestored_);
        CHECK_FALSE(cat.fieldCatalogEverLoaded_);
        CHECK(cat.currentCatalogProjectKey_.empty());
        CHECK(cat.LastTrackerFieldCatalogWarning.empty());
        CHECK(cat.AvailableUsers.empty() == clearUsers);
        CHECK(cat.CatalogEpoch.load() != epochBefore);
        CHECK(cat.TrackerFieldCatalogRevision.load() != revisionBefore);
    }
}

TEST_CASE("WriteCatalogFailureBannerLocked: only the no-catalog error drops a restored snapshot's state") {
    using smatchet::catalogoffline::CatalogFailureBanner;
    GridContextFieldCatalog cat;
    cat.fieldCatalogRestored_ = true;
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        smatchet::catalogops::WriteCatalogFailureBannerLocked(cat, CatalogFailureBanner::ErrorKeepCatalog, "Jira",
                                                              "401", false);
    }
    CHECK(cat.fieldCatalogRestored_); // the catalog is kept, so its source is too
    CHECK(cat.LastTrackerFieldCatalogError == "401");
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        smatchet::catalogops::WriteCatalogFailureBannerLocked(cat, CatalogFailureBanner::ErrorNoCatalog, "Jira",
                                                              "offline", true);
    }
    CHECK_FALSE(cat.fieldCatalogRestored_);
    CHECK_FALSE(cat.fieldCatalogEverLoaded_);
    CHECK(cat.LastTrackerFieldCatalogErrorTransient);
}

// ---- The grid's own catalog fetch: applied to the pane and under the configuration it latched (#2328) ----

TEST_CASE("AppController::ApplyGridFieldCatalogResult keys the snapshot and fixups by the fetch's configuration") {
    // SMATCHET_BACKEND_TYPE (or a Preferences save landing mid-fetch) can make the configuration read when
    // the result lands name Jira while a Plane pane fetched: the Plane catalog must not get the Jira-only
    // synthetic columns, and must be saved as the Plane snapshot, never over the Jira one.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    adapter.SetCacheBackendKey("Plane");

    TrackerConfig jiraCfg = ConfigManager::Load();
    jiraCfg.TrackerType = "Jira";
    TrackerConfig planeCfg = jiraCfg;
    planeCfg.TrackerType = "Plane";

    const GridCatalogFetchLatch latch = app.LatchGridCatalogFetch();
    CHECK(app.IsGridCatalogFetchForFocusedPane(latch));
    CHECK(app.ApplyGridFieldCatalogResult(latch, planeCfg, "PROJ", OneFieldCatalog().Fields, {}, {}, std::string(),
                                          false));
    CHECK(HasField(*app.GetAvailableFields(), "customfield_10001"));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "history"));  // Jira-only synthetic column
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "comments")); // Jira-only synthetic column
    CHECK(app.IsFieldCatalogScopedToProject("PROJ"));
    CHECK(SnapshotHasField(app, planeCfg, "PROJ", "customfield_10001"));
    CHECK_FALSE(SnapshotHasField(app, jiraCfg, "PROJ", "customfield_10001"));
}

TEST_CASE("AppController::ApplyGridFieldCatalogResult drops a result whose pane switched tracker or was cleared") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    const TrackerConfig cfg = ConfigManager::Load();

    SUBCASE("a backend switch after the latch") {
        const GridCatalogFetchLatch latch = app.LatchGridCatalogFetch();
        adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
        CHECK_FALSE(app.ApplyGridFieldCatalogResult(latch, cfg, std::string(), OneFieldCatalog().Fields, {}, {},
                                                    std::string(), false));
        CHECK(app.GetAvailableFields()->empty());
        CHECK_FALSE(SnapshotHasField(app, cfg, std::string(), "customfield_10001"));
    }
    SUBCASE("a catalog clear after the latch (a tracker switch starts with one)") {
        const GridCatalogFetchLatch latch = app.LatchGridCatalogFetch();
        app.ClearFieldCatalogInMemory();
        CHECK_FALSE(app.ApplyGridFieldCatalogResult(latch, cfg, std::string(), OneFieldCatalog().Fields, {}, {},
                                                    std::string(), false));
        CHECK(app.GetAvailableFields()->empty());
    }
    SUBCASE("a failure for a superseded pane changes nothing") {
        const GridCatalogFetchLatch latch = app.LatchGridCatalogFetch();
        adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
        CHECK_FALSE(
            app.ApplyGridFieldCatalogResult(latch, cfg, std::string(), {}, {}, {}, "tracker unreachable", true));
        SettleCatalogRestore(app);
        CHECK(app.GetFieldCatalogError().empty());
        CHECK(app.GetFieldCatalogWarning().empty());
    }
}

TEST_CASE("AppController::ApplyGridFieldCatalogResult restores the fetch project's snapshot on a failure") {
    // A pane switch clears the catalog and its project. The grid's next fetch fails offline and names the
    // project it resolved: that project's saved catalog comes back, pinned to it, not the unscoped one.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    const TrackerConfig cfg = ConfigManager::Load();

    REQUIRE(app.ApplyGridFieldCatalogResult(app.LatchGridCatalogFetch(), cfg, "FOO",
                                            CatalogWithField("customfield_foo").Fields, {}, {}, std::string(), false));
    REQUIRE(SnapshotHasField(app, cfg, "FOO", "customfield_foo"));
    app.ClearFieldCatalogInMemory();
    REQUIRE(app.GetAvailableFields()->empty());
    CHECK_FALSE(app.IsFieldCatalogScopedToProject("FOO")); // an empty catalog is no project's

    CHECK_FALSE(app.ApplyGridFieldCatalogResult(app.LatchGridCatalogFetch(), cfg, "FOO", {}, {}, {},
                                                "tracker unreachable", true));
    SettleCatalogRestore(app);
    CHECK(HasField(*app.GetAvailableFields(), "customfield_foo"));
    CHECK(app.IsFieldCatalogScopedToProject("FOO"));
    CHECK(app.GetFieldCatalogError().empty()); // a restored catalog shows a warning, not an error
}

TEST_CASE("AppController::ApplyGridFieldCatalogResult lands in the latched pane, not the focused one") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    const TrackerConfig cfg = ConfigManager::Load();
    REQUIRE(app.EnsurePaneContextLive("paneB", ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType)) != nullptr);

    const GridCatalogFetchLatch latch = app.LatchGridCatalogFetch(); // "main" is focused
    app.SetFocusedPane("paneB");
    CHECK_FALSE(app.IsGridCatalogFetchForFocusedPane(latch));
    CHECK(app.ApplyGridFieldCatalogResult(latch, cfg, std::string(), OneFieldCatalog().Fields, {}, {}, std::string(),
                                          false));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "customfield_10001")); // the focused pane is untouched
    app.SetFocusedPane(std::string("main"));
    CHECK(HasField(*app.GetAvailableFields(), "customfield_10001"));
}

TEST_CASE("AppController::ApplyGridFieldCatalogResult drops a fetch configured for another tracker than its pane") {
    // A Preferences switch to Plane whose backend swap waits for a running sync: the grid fetched with the Plane
    // settings from the still-Jira pane. Its result is no catalog of either and must not become the Plane snapshot.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    adapter.SetCacheBackendKey("Jira");
    TrackerConfig planeCfg = ConfigManager::Load();
    planeCfg.TrackerType = "Plane";

    CHECK_FALSE(app.ApplyGridFieldCatalogResult(app.LatchGridCatalogFetch(), planeCfg, std::string(),
                                                OneFieldCatalog().Fields, {}, {}, std::string(), false));
    CHECK(app.GetAvailableFields()->empty());
    CHECK_FALSE(SnapshotHasField(app, planeCfg, std::string(), "customfield_10001"));
}

TEST_CASE("AppController::ApplyGridFieldCatalogResult yields to a RefreshFieldCatalog started after it") {
    // A draft's project catalog refresh that starts while the grid fetch runs is the newer answer for the pane.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    const TrackerConfig cfg = ConfigManager::Load();

    const GridCatalogFetchLatch latch = app.LatchGridCatalogFetch();
    app.RefreshFieldCatalog(cfg); // the newer refresh, whatever it fetched
    SettleCatalogRestore(app);
    CHECK_FALSE(app.ApplyGridFieldCatalogResult(latch, cfg, std::string(), CatalogWithField("customfield_grid").Fields,
                                                {}, {}, std::string(), false));
    CHECK_FALSE(HasField(*app.GetAvailableFields(), "customfield_grid"));
}

TEST_CASE("AppController::GetAvailableFields snapshots stay whole while a worker replaces the catalog") {
    // GetAvailableFields used to return a reference into the focused catalog while RefreshFieldCatalog (a worker)
    // move-assigned a new vector over it, so a caller iterating the reference could read freed or half-written
    // memory. It now returns an immutable snapshot the caller keeps alive. Here the test thread plays the UI
    // thread, iterating snapshots with no lock while a worker refreshes the catalog between two generations; the
    // same holds for the offline queue's read (GridContextDepsAdapter::AvailableFields). This TU builds only in the
    // Linux posix-core-check lane (SmatchetCommandsTests); TSan covers the same contract at the GridContextFieldCatalog
    // seam in AppControllerFieldCatalogLock.test.cpp.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    SwapDuringFetchBackend* const backendRaw = backend.get();
    const auto generation = [](char marker) {
        TrackerFieldCatalogResult result;
        const std::size_t count = marker == 'a' ? 4 : 9;
        for (std::size_t i = 0; i < count; ++i) {
            TrackerField field;
            field.Id = std::string("gen") + marker + "_" + std::to_string(i);
            field.Name = std::string("gen-") + marker;
            result.Fields.push_back(field);
        }
        return result;
    };
    char nextMarker = 'a';
    backendRaw->OnFetch = [&]() { // runs on the worker, inside the fetch
        backendRaw->SetFieldCatalogResult(generation(nextMarker));
        nextMarker = nextMarker == 'a' ? 'b' : 'a';
    };
    adapter.SetBackend(std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();

    // One generation's own fields: every gen-prefixed id shares one marker and the count matches it. The synthetic
    // columns the Jira fixups add (history, comments) are not part of a generation.
    const auto isOneGeneration = [](const std::vector<TrackerField>& fields) {
        char marker = 0;
        std::size_t count = 0;
        for (const TrackerField& f : fields) {
            if (f.Id.compare(0, 3, "gen") != 0) {
                continue;
            }
            const char m = f.Id.size() > 3 ? f.Id[3] : '?';
            if ((marker != 0 && m != marker) || f.Name != std::string("gen-") + m) {
                return false;
            }
            marker = m;
            ++count;
        }
        return marker == 0 || count == (marker == 'a' ? 4u : 9u);
    };

    std::atomic<bool> writerDone{false};
    std::atomic<int> applied{0};
    std::thread worker([&] {
        for (int i = 0; i < 200; ++i) {
            if (app.RefreshFieldCatalog(cfg)) {
                applied.fetch_add(1);
            }
        }
        writerDone.store(true);
    });

    // No REQUIRE in this loop: it throws, and unwinding past the joinable worker would std::terminate.
    bool sawNull = false;
    bool sawInconsistent = false;
    int reads = 0;
    while (!writerDone.load() || reads < 200) {
        ++reads;
        const std::shared_ptr<const std::vector<TrackerField>> fields = app.GetAvailableFields();
        const std::shared_ptr<const std::vector<TrackerField>> depsFields = adapter.AvailableFields();
        if (!fields || !depsFields) {
            sawNull = true;
            continue;
        }
        sawInconsistent = sawInconsistent || !isOneGeneration(*fields) || !isOneGeneration(*depsFields);
        std::this_thread::yield(); // let a refresh land while these snapshots are still held
        sawInconsistent = sawInconsistent || !isOneGeneration(*fields) || !isOneGeneration(*depsFields);
    }
    worker.join();
    SettleCatalogRestore(app);

    CHECK_FALSE(sawNull);
    CHECK_FALSE(sawInconsistent);
    CHECK(applied.load() == 200);
    CHECK(isOneGeneration(*app.GetAvailableFields()));
    CHECK(HasField(*app.GetAvailableFields(), "genb_0")); // the 200th refresh fetched generation b
}

namespace {

// A grid pane's OWN catalog (AppController::populatePaneCatalogAfterSync_, kicked by the pane's first sync)
// is scoped to the single project the view the kick resolved for the pane names. Before, the pane always
// fetched unscoped: Jira skipped createmeta, logged "field catalog fetched WITHOUT project scope", and the
// pane's component / version dropdowns stayed empty.

const char* const kCatalogPane = "catalog-pane";

/// Parses a view's JQL as JiraClient does, records the project every catalog fetch was scoped to, and fails
/// every fetch when `Fail` is set (set it before the kick: the fetch reads it on the kick's worker).
class ScopedCatalogBackend : public smatchet_tests::FakeTrackerClient {
  public:
    bool Fail = false;

    std::string ExtractProjectFromQuery(const std::string& query) const override {
        return JqlProjectScope::ExtractSingleProject(query);
    }

    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& cfg,
                                                                      const std::string& projectKey) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fetchedProjects_.push_back(projectKey);
        }
        if (Fail) {
            return Result<TrackerFieldCatalogResult, TrackerError>::Err(
                TrackerErrorInvalidRequest("the catalog is unavailable"));
        }
        return FakeTrackerClient::FetchFieldCatalog(cfg, projectKey);
    }

    std::vector<std::string> FetchedProjects() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return fetchedProjects_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> fetchedProjects_;
};

/// A catalog whose components field carries the project's options, as a createmeta-enriched Jira fetch does.
TrackerFieldCatalogResult CatalogWithComponents() {
    TrackerFieldCatalogResult result;
    TrackerField components;
    components.Id = "components";
    components.Name = "Components";
    TrackerFieldOption option;
    option.Id = "10001";
    option.Value = "Backend";
    components.AllowedValueOptions.push_back(option);
    result.Fields.push_back(components);
    return result;
}

/// Pumps the main-thread queue until `done` holds, for up to 20 s. Returns whether it held.
template <typename Pred> bool PumpUntil(AppController& app, Pred done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        app.mainThreadDispatcher.Drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// An unfocused pane on the configured tracker with its own (still empty) catalog and `backend`. Created
/// before anything fills the focused catalog, so the same-backend seed copies nothing into it.
ScopedCatalogBackend* AddCatalogPane(AppController& app, std::unique_ptr<ScopedCatalogBackend> backend) {
    const TrackerConfig cfg = ConfigManager::Load();
    GridLiveContext* const pane =
        app.EnsurePaneContextLive(kCatalogPane, ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType));
    REQUIRE(pane != nullptr);
    REQUIRE_FALSE(app.IsPaneFieldCatalogPopulated(kCatalogPane));
    ScopedCatalogBackend* const raw = backend.get();
    GridContextDepsAdapter paneAdapter(app, *pane);
    paneAdapter.SetBackend(std::move(backend));
    return raw;
}

/// Kicks the pane's catalog step for a view whose query is `viewQuery` and waits for its catalog to land.
void KickAndSettle(AppController& app, const TrackerConfig& cfg, const std::string& viewQuery) {
    app.KickPaneCatalogFetchForTest(kCatalogPane, cfg, viewQuery);
    REQUIRE(PumpUntil(app, [&app]() { return app.IsPaneFieldCatalogPopulated(kCatalogPane); }));
}

/// Whether pane `paneId`'s own catalog was fetched for `project`: the comparison the grid's components cell makes
/// against GetPaneFieldCatalogProjectKey (resolved once per pane per frame).
bool PaneCatalogScopedTo(const AppController& app, const std::string& paneId, const std::string& project) {
    return smatchet::components::CatalogIsRowProject(app.GetPaneFieldCatalogProjectKey(paneId), project);
}

const TrackerField* FindField(const std::vector<TrackerField>& fields, const std::string& id) {
    for (const TrackerField& f : fields) {
        if (f.Id == id) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Pane catalog: a view naming one project scopes the pane's own fetch and fills its components") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    auto backend = std::make_unique<ScopedCatalogBackend>();
    backend->SetFieldCatalogResult(CatalogWithComponents());
    ScopedCatalogBackend* const raw = AddCatalogPane(app, std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();

    KickAndSettle(app, cfg, "project = KEY AND status = Open ORDER BY updated DESC");

    // The fetch was scoped, so Jira runs createmeta and logs no unscoped-catalog warning for the pane.
    CHECK(raw->FetchedProjects() == std::vector<std::string>{"KEY"});
    // The pane's own catalog is pinned to KEY; the focused pane's is not touched.
    CHECK(PaneCatalogScopedTo(app, kCatalogPane, "KEY"));
    CHECK(PaneCatalogScopedTo(app, kCatalogPane, "key")); // project keys compare without case
    CHECK_FALSE(PaneCatalogScopedTo(app, kCatalogPane, "OTHER"));
    CHECK_FALSE(app.IsFieldCatalogScopedToProject("KEY"));
    CHECK_FALSE(PaneCatalogScopedTo(app, "main", "KEY"));

    // The components dropdown of a KEY row in this pane uses the pane catalog's list (no per-project list cached).
    const std::shared_ptr<const std::vector<TrackerField>> fields = app.GetPaneAvailableFields(kCatalogPane);
    REQUIRE(fields);
    const TrackerField* const components = FindField(*fields, "components");
    REQUIRE(components != nullptr);
    const smatchet::components::ComponentOptionsPick pick = smatchet::components::PickComponentOptions(
        ProjectComponentsLookup(), PaneCatalogScopedTo(app, kCatalogPane, "KEY"), components->AllowedValueOptions,
        app.GetLastTrackerConnectivityState());
    CHECK(pick.FromCatalog);
    REQUIRE(pick.Options->size() == 1u);
    CHECK(pick.Options->front().Value == "Backend");
}

TEST_CASE("Pane catalog: a view naming several projects, or none, keeps the unscoped fallback") {
    const char* const queries[] = {"project in (A, B)", "assignee = currentUser()", ""};
    for (const char* const query : queries) {
        CAPTURE(query);
        smatchet_tests::OfflineQueueTestEnvGuard env;
        AppController app;
        GridContextDepsAdapter adapter(app);
        adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
        auto backend = std::make_unique<ScopedCatalogBackend>();
        backend->SetFieldCatalogResult(CatalogWithComponents());
        ScopedCatalogBackend* const raw = AddCatalogPane(app, std::move(backend));

        KickAndSettle(app, ConfigManager::Load(), query);

        CHECK(raw->FetchedProjects() == std::vector<std::string>{std::string()});
        // An unscoped catalog is no project's: its components never stand in for a row's project list.
        CHECK_FALSE(PaneCatalogScopedTo(app, kCatalogPane, "A"));
        const smatchet::components::ComponentOptionsPick pick = smatchet::components::PickComponentOptions(
            ProjectComponentsLookup(), PaneCatalogScopedTo(app, kCatalogPane, "A"),
            FindField(*app.GetPaneAvailableFields(kCatalogPane), "components")->AllowedValueOptions,
            app.GetLastTrackerConnectivityState());
        CHECK_FALSE(pick.FromCatalog);
        CHECK(pick.Options->empty());
    }
}

TEST_CASE("Pane catalog: a failed scoped fetch restores the snapshot saved for the same project first") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    auto backend = std::make_unique<ScopedCatalogBackend>();
    backend->Fail = true;
    ScopedCatalogBackend* const raw = AddCatalogPane(app, std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();
    // Both snapshots exist: the unscoped one and KEY's own (saved by the focused grid's fetches).
    REQUIRE(ApplyGridCatalog(app, cfg, std::string(), CatalogWithField("customfield_any").Fields));
    REQUIRE(ApplyGridCatalog(app, cfg, "KEY", CatalogWithField("customfield_key").Fields));
    REQUIRE(SnapshotHasField(app, cfg, std::string(), "customfield_any"));
    REQUIRE(SnapshotHasField(app, cfg, "KEY", "customfield_key"));

    KickAndSettle(app, cfg, "project = KEY");

    CHECK(raw->FetchedProjects() == std::vector<std::string>{"KEY"});
    const std::shared_ptr<const std::vector<TrackerField>> fields = app.GetPaneAvailableFields(kCatalogPane);
    REQUIRE(fields);
    CHECK(HasField(*fields, "customfield_key"));
    CHECK_FALSE(HasField(*fields, "customfield_any"));
    CHECK(PaneCatalogScopedTo(app, kCatalogPane, "KEY"));
}

TEST_CASE("Pane catalog: the unscoped snapshot standing in for a project restores as no project's") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    auto backend = std::make_unique<ScopedCatalogBackend>();
    backend->Fail = true;
    ScopedCatalogBackend* const raw = AddCatalogPane(app, std::move(backend));
    const TrackerConfig cfg = ConfigManager::Load();
    REQUIRE(ApplyGridCatalog(app, cfg, std::string(), CatalogWithField("customfield_any").Fields));
    REQUIRE(SnapshotHasField(app, cfg, std::string(), "customfield_any"));
    REQUIRE_FALSE(SnapshotHasField(app, cfg, "NEW", "customfield_any"));

    KickAndSettle(app, cfg, "project = NEW");

    CHECK(raw->FetchedProjects() == std::vector<std::string>{"NEW"});
    CHECK(HasField(*app.GetPaneAvailableFields(kCatalogPane), "customfield_any"));
    CHECK_FALSE(PaneCatalogScopedTo(app, kCatalogPane, "NEW"));
}

namespace {

/// Puts the pane in the state the same-backend seed leaves it in: a loaded catalog copied from the focused one,
/// pinned to `seedProject` ("" = unscoped).
void SeedPaneCatalog(AppController& app, const std::string& seedProject) {
    GridLiveContext* const pane = app.EnsurePaneContextLive(kCatalogPane, std::string());
    REQUIRE(pane != nullptr);
    GridContextFieldCatalog& cat = pane->fieldCatalog;
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    cat.AvailableFields = std::make_shared<std::vector<TrackerField>>(CatalogWithField("customfield_seed").Fields);
    cat.fieldCatalogEverLoaded_ = true;
    cat.currentCatalogProjectKey_ = seedProject;
}

/// Answers each fetch with a catalog naming its project (`customfield_<project>`), and holds the FIRST fetch until
/// `Release()`: the window in which the pane's view changes and a second fetch starts and lands.
class HeldFirstFetchBackend : public ScopedCatalogBackend {
  public:
    HeldFirstFetchBackend() : gate_(release_.get_future().share()) {}
    ~HeldFirstFetchBackend() override { Release(); }

    void Release() {
        if (!released_.exchange(true)) {
            release_.set_value();
        }
    }
    bool FirstStarted() const { return firstStarted_.load(); }
    bool FirstReturned() const { return firstReturned_.load(); }

    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& /*cfg*/,
                                                                      const std::string& projectKey) override {
        const bool first = calls_.fetch_add(1) == 0;
        if (first) {
            firstStarted_.store(true);
            gate_.wait();
        }
        Result<TrackerFieldCatalogResult, TrackerError> result =
            Result<TrackerFieldCatalogResult, TrackerError>::Ok(CatalogWithField("customfield_" + projectKey));
        if (first) {
            firstReturned_.store(true);
        }
        return result;
    }

  private:
    std::promise<void> release_;
    std::shared_future<void> gate_;
    std::atomic<bool> released_{false};
    std::atomic<int> calls_{0};
    std::atomic<bool> firstStarted_{false};
    std::atomic<bool> firstReturned_{false};
};

/// Releases a held fetch when the test leaves scope, before the AppController joins its workers (declare it after
/// the AppController), so a failed REQUIRE never leaves a worker blocked in the destructor's join.
struct ReleaseHeldFetchOnExit {
    HeldFirstFetchBackend* Backend;
    ~ReleaseHeldFetchOnExit() { Backend->Release(); }
};

/// Pumps the main-thread queue for `duration`: lets a result that should be dropped arrive and be dropped.
void PumpFor(AppController& app, std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
        app.mainThreadDispatcher.Drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

TEST_CASE("Pane catalog: a seeded catalog of another scope does not stop the view's project fetch") {
    // A same-backend pane starts with the focused catalog's copy. When that copy is unscoped (or another project's),
    // it must not count as the pane's KEY catalog: KEY's components and versions would never load.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    auto backend = std::make_unique<ScopedCatalogBackend>();
    backend->SetFieldCatalogResult(CatalogWithComponents());
    ScopedCatalogBackend* const raw = AddCatalogPane(app, std::move(backend));
    SeedPaneCatalog(app, std::string());
    const TrackerConfig cfg = ConfigManager::Load();

    app.KickPaneCatalogFetchForTest(kCatalogPane, cfg, "project = KEY");
    REQUIRE(PumpUntil(app, [&app]() { return PaneCatalogScopedTo(app, kCatalogPane, "KEY"); }));

    CHECK(raw->FetchedProjects() == std::vector<std::string>{"KEY"});
    CHECK(FindField(*app.GetPaneAvailableFields(kCatalogPane), "components") != nullptr);
    CHECK_FALSE(HasField(*app.GetPaneAvailableFields(kCatalogPane), "customfield_seed"));
}

TEST_CASE("Pane catalog: a catalog already loaded for the view's scope is not fetched again") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    auto backend = std::make_unique<ScopedCatalogBackend>();
    backend->SetFieldCatalogResult(CatalogWithComponents());
    ScopedCatalogBackend* const raw = AddCatalogPane(app, std::move(backend));
    SeedPaneCatalog(app, "KEY");
    const TrackerConfig cfg = ConfigManager::Load();

    // The skip is decided on the UI thread before any worker starts, so nothing is launched to wait for.
    app.KickPaneCatalogFetchForTest(kCatalogPane, cfg, "project = key");            // project keys compare without case
    app.KickPaneCatalogFetchForTest(kCatalogPane, cfg, "assignee = currentUser()"); // unscoped: KEY's is a superset
    PumpFor(app, std::chrono::milliseconds(50));

    CHECK(raw->FetchedProjects().empty());
    CHECK(PaneCatalogScopedTo(app, kCatalogPane, "KEY"));
    CHECK(HasField(*app.GetPaneAvailableFields(kCatalogPane), "customfield_seed"));
}

TEST_CASE("Pane catalog: a fetch for the pane's previous view that finishes last is dropped") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    auto backend = std::make_unique<HeldFirstFetchBackend>();
    HeldFirstFetchBackend* const held = backend.get();
    AddCatalogPane(app, std::move(backend));
    ReleaseHeldFetchOnExit releaseOnExit{held};
    const TrackerConfig cfg = ConfigManager::Load();

    app.KickPaneCatalogFetchForTest(kCatalogPane, cfg, "project = OLD");
    REQUIRE(PumpUntil(app, [held]() { return held->FirstStarted(); }));
    // The view changes to NEW while OLD's fetch is still out: NEW's fetch starts and lands first.
    app.KickPaneCatalogFetchForTest(kCatalogPane, cfg, "project = NEW");
    REQUIRE(PumpUntil(app, [&app]() { return PaneCatalogScopedTo(app, kCatalogPane, "NEW"); }));
    REQUIRE(HasField(*app.GetPaneAvailableFields(kCatalogPane), "customfield_NEW"));

    held->Release();
    REQUIRE(PumpUntil(app, [held]() { return held->FirstReturned(); }));
    PumpFor(app, std::chrono::milliseconds(300)); // OLD's result is posted right after it returns

    CHECK(PaneCatalogScopedTo(app, kCatalogPane, "NEW"));
    CHECK(HasField(*app.GetPaneAvailableFields(kCatalogPane), "customfield_NEW"));
    CHECK_FALSE(HasField(*app.GetPaneAvailableFields(kCatalogPane), "customfield_OLD"));
}

TEST_CASE("Pane catalog: a pane fetch gets the Jira catalog corrections the focused catalog gets") {
    // Once a scoped fetch may replace a seeded (already corrected) catalog, the pane's own result must be corrected
    // the same way: non-editable timetracking stays read-only, the legacy comment blob is dropped, and the
    // synthetic History / Comments columns are present.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    auto backend = std::make_unique<ScopedCatalogBackend>();
    TrackerFieldCatalogResult raw = CatalogWithField("timespent");
    TrackerField legacyComment;
    legacyComment.Id = "comment";
    legacyComment.Name = "Comment";
    raw.Fields.push_back(legacyComment);
    backend->SetFieldCatalogResult(raw);
    AddCatalogPane(app, std::move(backend));
    SeedPaneCatalog(app, std::string());

    app.KickPaneCatalogFetchForTest(kCatalogPane, ConfigManager::Load(), "project = KEY");
    REQUIRE(PumpUntil(app, [&app]() { return PaneCatalogScopedTo(app, kCatalogPane, "KEY"); }));

    const std::shared_ptr<const std::vector<TrackerField>> fields = app.GetPaneAvailableFields(kCatalogPane);
    REQUIRE(fields);
    const TrackerField* const timespent = FindField(*fields, "timespent");
    REQUIRE(timespent != nullptr);
    CHECK(timespent->ReadOnly);
    CHECK_FALSE(HasField(*fields, "comment"));
    CHECK(HasField(*fields, "history"));
    CHECK(HasField(*fields, "comments"));
}

TEST_CASE("Pane catalog: switching back to the loaded view drops a fetch still out for the view in between") {
    // Catalog A is loaded; the view switches to B (fetch held), then back to A, which needs no fetch. B's result,
    // landing afterwards, must not replace A's catalog.
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>());
    auto backend = std::make_unique<HeldFirstFetchBackend>();
    HeldFirstFetchBackend* const held = backend.get();
    AddCatalogPane(app, std::move(backend));
    ReleaseHeldFetchOnExit releaseOnExit{held};
    SeedPaneCatalog(app, "A");
    const TrackerConfig cfg = ConfigManager::Load();

    app.KickPaneCatalogFetchForTest(kCatalogPane, cfg, "project = B");
    REQUIRE(PumpUntil(app, [held]() { return held->FirstStarted(); }));
    app.KickPaneCatalogFetchForTest(kCatalogPane, cfg, "project = A"); // A is loaded: no fetch

    held->Release();
    REQUIRE(PumpUntil(app, [held]() { return held->FirstReturned(); }));
    PumpFor(app, std::chrono::milliseconds(300)); // B's result is posted right after it returns

    CHECK(PaneCatalogScopedTo(app, kCatalogPane, "A"));
    CHECK(HasField(*app.GetPaneAvailableFields(kCatalogPane), "customfield_seed"));
    CHECK_FALSE(HasField(*app.GetPaneAvailableFields(kCatalogPane), "customfield_B"));
}
