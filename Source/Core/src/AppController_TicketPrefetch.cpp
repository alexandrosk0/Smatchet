// AppController_TicketPrefetch.cpp — bulk-import ticket-prefetch cluster extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/appcontroller-clusters-followup.md). Method DECLARATIONS stay in
// AppController.h; only the definitions moved, so linkage and behavior are identical.
// The cluster dedupes requested keys against the in-flight set, launches a background
// worker, fetches and caches the tickets off the UI thread, then clears the in-flight
// set. Includes are curated from what the moved bodies actually use; this TU never
// touches the pImpl, so it does not need the companion-TU subsystem superset.
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining the AppController ticket-prefetch methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on

#include "ConfigManager.h"
#include "LocalCacheManager.h" // direct: AppController.h fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls Cache-> methods.
#include "Logger.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

void AppController::PrefetchIssueTicketsForKeys(const std::vector<std::string>& issueKeys, bool includeAlreadyActive) {
    // Latch the focused pane once, here: the worker fetches from this pane's backend and saves into its
    // cache namespace even if focus moves before it runs, so one tracker's tickets never land under
    // another's key. Nothing is held across the fetch; the old pane may be retired meanwhile (ADR-0012).
    const GridLiveContext& pane = focusedContext();
    std::vector<std::string> keys;
    if (includeAlreadyActive) {
        keys = issueKeys;
    } else {
        const auto snap = GetActiveTicketsSnapshot();
        std::unordered_set<std::string> have;
        if (snap) {
            for (const auto& t : *snap) {
                have.insert(t.id);
            }
        }
        for (const auto& k : issueKeys) {
            if (have.count(k) == 0) {
                keys.push_back(k);
            }
        }
    }
    PrefetchIssueTicketsFrom(std::atomic_load(&pane.Backend), pane.CacheBackendKeyCopy(), keys);
}

void AppController::PrefetchIssueTicketsFrom(const std::shared_ptr<ITrackerBackend>& backend,
                                             const std::string& cacheBackendKey,
                                             const std::vector<std::string>& issueKeys) {
    if (!Cache || !backend) {
        return;
    }
    std::vector<std::string> toFetch;
    {
        std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
        for (const auto& k : issueKeys) {
            if (k.empty() || bulkImportPrefetchKeysInFlight_.count(k) > 0) {
                continue;
            }
            bulkImportPrefetchKeysInFlight_.insert(k);
            toFetch.push_back(k);
        }
    }
    if (toFetch.empty()) {
        return;
    }
    LaunchBackgroundTask([this, toFetch, backend, cacheBackendKey]() {
        FetchAndCachePrefetchedTickets(toFetch, backend, cacheBackendKey);
    });
}

void AppController::FetchAndCachePrefetchedTickets(const std::vector<std::string>& toFetch,
                                                   const std::shared_ptr<ITrackerBackend>& backend,
                                                   const std::string& cacheBackendKey) {
    // Safety net for the in-flight keys that PrefetchIssueTicketsFrom inserted: a key left in
    // the set blocks every future prefetch for it until restart, so it must be cleared on every
    // exit. The success path clears the keys inline below and disarms this guard; the guard exists
    // only to cover the exception path that would otherwise leak.
    struct InFlightClearGuard {
        AppController* self;
        const std::vector<std::string>& keys;
        bool armed = true;
        ~InFlightClearGuard() {
            if (!armed) {
                return;
            }
            std::lock_guard<std::mutex> lock(self->bulkImportPrefetchKeysMutex_);
            for (const auto& k : keys) {
                self->bulkImportPrefetchKeysInFlight_.erase(k);
            }
        }
    } inFlightClearGuard{this, toFetch};

    TrackerConfig cfg = ConfigManager::Load();

    ViewsStore views = ConfigManager::LoadViewsOrBootstrap(cfg);

    TrackerError err;

    std::vector<CachedTicket> tickets;

    auto fetchResult = backend->Reader().FetchIssuesForKeys(cfg, toFetch, views);
    const bool ok = static_cast<bool>(fetchResult);
    if (ok) {
        tickets = std::move(fetchResult.value());
    } else {
        err = fetchResult.error();
    }

    {

        std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);

        for (const auto& k : toFetch) {

            bulkImportPrefetchKeysInFlight_.erase(k);
        }
    }
    inFlightClearGuard.armed = false;

    if (!ok) {

        // N12 item 13: the structured kind from FetchIssuesForKeys is authoritative — no
        // re-classification of the flattened text.
        if (err.IsRetryable()) {

            LOG_INFO("AppController::PrefetchIssueTicketsForKeys skipped (transport): %s", err.Detail.c_str());

        } else {

            LOG_WARN("AppController::PrefetchIssueTicketsForKeys failed: %s", err.Detail.c_str());
        }

        return;
    }

    requestDeferredLiveTrackerBackendSuccessNotify_();

    if (!Cache) {

        return;
    }

    for (const auto& t : tickets) {

        Cache->SaveTicket(cacheBackendKey, t);
    }

    RefreshLocalData();
}

bool AppController::IsBulkImportPrefetchInFlight(const std::string& issueKey) const {

    if (issueKey.empty()) {

        return false;
    }

    std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);

    return bulkImportPrefetchKeysInFlight_.find(issueKey) != bulkImportPrefetchKeysInFlight_.end();
}
