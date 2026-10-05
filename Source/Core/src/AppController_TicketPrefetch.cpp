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
#include "OfflineFirstPure.h" // kLookupRetryAfterSeconds — the failed-prefetch backoff

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// In-flight prefetches are tracked per (cache backend key, issue key): the same issue key on two
// trackers names two different tickets, and each must be re-read.
std::string PrefetchInFlightKey(const std::string& cacheBackendKey, const std::string& issueKey) {
    return cacheBackendKey + '\x1f' + issueKey;
}

// Erases keys built when they were inserted, so this allocates nothing: it also runs in an exit guard's destructor.
void ErasePrefetchInFlight(std::mutex& mutex, std::unordered_set<std::string>& inFlight,
                           const std::vector<std::string>& inFlightKeys) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& k : inFlightKeys) {
        inFlight.erase(k);
    }
}

} // namespace

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
    if (IsTrackerOffline()) {
        // Pillar 6: best-effort, so offline it is skipped rather than spending a worker's whole retry
        // window on a request that cannot succeed; the cached tickets stay as they are.
        LOG_DEBUG("AppController::PrefetchIssueTicketsFrom: tracker offline, skipped %zu key(s)", issueKeys.size());
        return;
    }
    std::vector<std::string> toFetch;
    std::vector<std::string> inFlightKeys;
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
        for (const auto& k : issueKeys) {
            if (k.empty()) {
                continue;
            }
            std::string inFlightKey = PrefetchInFlightKey(cacheBackendKey, k);
            const auto backoff = bulkImportPrefetchRetryAfter_.find(inFlightKey);
            if (backoff != bulkImportPrefetchRetryAfter_.end()) {
                if (now < backoff->second) {
                    continue; // failed recently: wait out the backoff instead of resending every frame
                }
                bulkImportPrefetchRetryAfter_.erase(backoff);
            }
            if (bulkImportPrefetchKeysInFlight_.insert(inFlightKey).second) {
                toFetch.push_back(k);
                inFlightKeys.push_back(std::move(inFlightKey));
            }
        }
    }
    if (toFetch.empty()) {
        return;
    }
    try {
        LaunchBackgroundTask([this, toFetch, inFlightKeys, backend, cacheBackendKey]() {
            FetchAndCachePrefetchedTickets(toFetch, inFlightKeys, backend, cacheBackendKey);
        });
    } catch (...) {
        // Nothing else would clear these, and a key left in flight blocks every later prefetch of it.
        ErasePrefetchInFlight(bulkImportPrefetchKeysMutex_, bulkImportPrefetchKeysInFlight_, inFlightKeys);
        throw;
    }
}

void AppController::FetchAndCachePrefetchedTickets(const std::vector<std::string>& toFetch,
                                                   const std::vector<std::string>& inFlightKeys,
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
            if (armed) {
                ErasePrefetchInFlight(self->bulkImportPrefetchKeysMutex_, self->bulkImportPrefetchKeysInFlight_, keys);
            }
        }
    } inFlightClearGuard{this, inFlightKeys};

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

    if (ok) {
        ErasePrefetchInFlight(bulkImportPrefetchKeysMutex_, bulkImportPrefetchKeysInFlight_, inFlightKeys);
    } else {
        // Pillar 6: release the keys and start their backoff under one lock, so an ask in between cannot
        // resend at once. A connectivity recovery clears the backoff early.
        const std::chrono::steady_clock::time_point retryAt =
            std::chrono::steady_clock::now() + std::chrono::seconds(smatchet::offline::kLookupRetryAfterSeconds);
        std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
        for (const auto& k : inFlightKeys) {
            bulkImportPrefetchRetryAfter_[k] = retryAt;
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

    {
        std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
        if (bulkImportPrefetchKeysInFlight_.empty()) {
            return false; // the common case: no key copy per row per frame
        }
    }
    // The bulk-import rows belong to the focused pane. Its key is read outside the set's lock, so the
    // two mutexes are never held together.
    const std::string key = PrefetchInFlightKey(focusedContext().CacheBackendKeyCopy(), issueKey);
    std::lock_guard<std::mutex> lock(bulkImportPrefetchKeysMutex_);
    return bulkImportPrefetchKeysInFlight_.count(key) > 0;
}
