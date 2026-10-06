// AppController::RefreshFieldCatalog runs on a worker and writes the catalog of the pane it latched.
// A pane that switches tracker while the fetch is in flight must not receive the old tracker's
// catalog. Linked into SmatchetCommandsTests: it needs the real (headless) AppController, which
// only the posix-core-check archive provides.

#include <doctest/doctest.h>

#include "../support/FakeTrackerClient.h"

#include "AppController.h"
#include "GridContextDepsAdapter.h"
#include "Tracker/TrackerFieldSchema.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

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

} // namespace

TEST_CASE("AppController::RefreshFieldCatalog drops the result when the pane switches tracker mid-fetch") {
    AppController app;
    GridContextDepsAdapter adapter(app);

    auto first = std::make_unique<SwapDuringFetchBackend>();
    TrackerFieldCatalogResult result;
    TrackerField field;
    field.Id = "customfield_10001";
    field.Name = "Old tracker field";
    result.Fields.push_back(field);
    first->SetFieldCatalogResult(result);
    SwapDuringFetchBackend* const firstRaw = first.get();
    adapter.SetBackend(std::move(first));
    firstRaw->OnFetch = [&adapter]() { adapter.SetBackend(std::make_unique<smatchet_tests::FakeTrackerClient>()); };

    const std::uint64_t revisionBefore = app.GetFieldCatalogRevision();
    CHECK_FALSE(app.RefreshFieldCatalog(TrackerConfig{}));
    // The retired backend stays alive in the graveyard, so its call count is still readable.
    CHECK(firstRaw->FetchFieldCatalogCalls() == 1u);
    CHECK(app.GetAvailableFields().empty());
    CHECK(app.GetFieldCatalogRevision() == revisionBefore);
}
