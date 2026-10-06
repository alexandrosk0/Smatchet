// AppController::RefreshFieldCatalog runs on a worker and writes the catalog of the pane it latched.
// A pane that switches tracker while the fetch is in flight must not receive the old tracker's
// catalog. Linked into SmatchetCommandsTests: it needs the real (headless) AppController, which
// only the posix-core-check archive provides.

#include <doctest/doctest.h>

#include "../support/FakeTrackerClient.h"
#include "../support/OfflineQueueTestEnv.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "GridContextDepsAdapter.h"
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

    Result<TrackerFieldCatalogResult, TrackerError> FetchFieldCatalog(const TrackerConfig& cfg,
                                                                      const std::string& projectKey) override {
        if (OnFetch) {
            OnFetch();
        }
        return FakeTrackerClient::FetchFieldCatalog(cfg, projectKey);
    }
};

TrackerFieldCatalogResult OneFieldCatalog() {
    TrackerFieldCatalogResult result;
    TrackerField field;
    field.Id = "customfield_10001";
    field.Name = "Old tracker field";
    result.Fields.push_back(field);
    return result;
}

bool HasField(const std::vector<TrackerField>& fields, const std::string& id) {
    for (const TrackerField& f : fields) {
        if (f.Id == id) {
            return true;
        }
    }
    return false;
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

TEST_CASE("AppController::RefreshFieldCatalog skips a fetch for another tracker kind than the pane runs") {
    // A deferred Save & Sync leaves the old backend installed while the config already names the new
    // tracker: a fetch then would pair the two, so it must not run at all.
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
    CHECK_FALSE(app.RefreshFieldCatalog(cfg));
    CHECK(raw->FetchFieldCatalogCalls() == 0u);
    CHECK(app.GetAvailableFields().empty());
}

TEST_CASE("AppController::RefreshFieldCatalog drops a result once a newer refresh pins another project") {
    smatchet_tests::OfflineQueueTestEnvGuard env;
    AppController app;
    GridContextDepsAdapter adapter(app);
    auto backend = std::make_unique<SwapDuringFetchBackend>();
    backend->SetFieldCatalogResult(OneFieldCatalog());
    SwapDuringFetchBackend* const raw = backend.get();
    adapter.SetBackend(std::move(backend));
    raw->OnFetch = [&app]() { app.SetCurrentCatalogProject("PROJB"); };

    CHECK_FALSE(app.RefreshFieldCatalog(ConfigManager::Load(), "PROJA"));
    CHECK_FALSE(HasField(app.GetAvailableFields(), "customfield_10001"));
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
