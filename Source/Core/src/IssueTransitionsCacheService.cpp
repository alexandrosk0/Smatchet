#include "IssueTransitionsCacheService.h"

#include "IEditMetaDeps.h"
#include "ILookupCache.h"
#include "ITrackerFieldCatalog.h"
#include "LearnedWorkflowPure.h"
#include "Logger.h"
#include "OfflineFirstPure.h"
#include "ProjectResolver.h"
#include "StringUtil.h"

IssueTransitionsCacheService::IssueTransitionsCacheService(IEditMetaDeps& deps) : deps_(deps) {}

std::string IssueTransitionsCacheService::LiveKey(const std::string& backendKey, const std::string& issueId) {
    return backendKey + "|" + issueId;
}

TransitionsLookup IssueTransitionsCacheService::GetAvailableTransitions(const TransitionsQuery& q) const {
    TransitionsLookup lookup;

    auto backend = deps_.BackendShared();
    auto catalog = backend ? backend->FieldCatalog() : nullptr;
    if (!backend || !catalog || !catalog->SupportsIssueTransitions()) {
        lookup.applicable = false;
        return lookup;
    }

    lookup.applicable = true;
    const std::string backendKey = deps_.CacheBackendKey();
    const std::string liveKey = LiveKey(backendKey, q.IssueId);

    // Check if live entry has Fresh value.
    if (live_.HasValue(liveKey)) {
        const auto value = live_.GetValue(liveKey);
        if (value) {
            lookup.options = *value;
            lookup.freshness = live_.Freshness(liveKey, smatchet::offline::ClassifyFreshnessInput{
                                                  .HasCache = !lookup.options.empty(),
                                                  .Live = true,
                                                  .InFlight = live_.InFlight(liveKey),
                                                  .LastAttemptFailed = live_.LastAttemptFailed(liveKey),
                                                  .Connectivity = deps_.TrackerConnectivity()});
            lookup.fromLearned = false;
            return lookup;
        }
    }

    // No live value; check learned.
    const std::string learnedKey = backendKey + "|" +
                                    smatchet::workflow::BuildLearnedTransitionsKey(q.ProjectKey, q.IssueTypeKey, q.FromStatusKey);
    {
        std::lock_guard<std::mutex> lock(learnedMutex_);
        const auto it = learned_.find(learnedKey);
        if (it != learned_.end()) {
            lookup.options = it->second;
            lookup.fromLearned = true;
        }
    }

    // Compute freshness: live=false, options come from cache (if any).
    lookup.freshness = smatchet::offline::ClassifyFreshness({
        .HasCache = !lookup.options.empty(),
        .Live = false,
        .InFlight = live_.InFlight(liveKey),
        .LastAttemptFailed = live_.LastAttemptFailed(liveKey),
        .Connectivity = deps_.TrackerConnectivity()});

    return lookup;
}

void IssueTransitionsCacheService::EnsureIssueTransitionsLoaded(const TransitionsQuery& q,
                                                                 const TrackerConfig* configSnapshot) {
    auto backend = deps_.BackendShared();
    auto catalog = backend ? backend->FieldCatalog() : nullptr;
    if (!backend || !catalog || !catalog->SupportsIssueTransitions()) {
        return; // Not applicable — no fetch, no log.
    }

    const std::string backendKey = deps_.CacheBackendKey();
    EnsureLearnedLoaded(backendKey);

    const std::string liveKey = LiveKey(backendKey, q.IssueId);
    smatchet::offline::KeyedLookupCache<std::vector<TrackerFieldOption>>::TicketHandle ticket;
    if (!live_.TryBeginFetch(liveKey, deps_.TrackerConnectivity(), smatchet::Clock::now(), ticket)) {
        return; // Already in flight or recently failed — no new fetch.
    }

    // Capture dependencies for the background worker.
    deps_.LaunchBackgroundTask([backend, catalog, ticket, q, backendKey, this]() {
        const TrackerConfig* useCfg = nullptr;
        std::unique_ptr<TrackerConfig> loadedCfg;
        if (!useCfg) {
            loadedCfg = std::make_unique<TrackerConfig>(ConfigManager::Load());
            useCfg = loadedCfg.get();
        }
        auto store = deps_.LookupCacheShared();
        smatchet::offline::RunKeyedFetch(
            live_, ticket, [&]() {
                auto r = catalog->FetchIssueTransitions(*useCfg, q.IssueId);
                if (r.has_value()) {
                    RememberLearned(backendKey, q, r.value(), store);
                } else {
                    LOG_DEBUG("IssueTransitionsCacheService: transitions fetch failed issue=%s kind=%s", q.IssueId.c_str(),
                              ToString(r.error().Kind).c_str());
                }
                return r;
            });
    });
}

void IssueTransitionsCacheService::InvalidateIssueTransitions(const std::string& issueId) {
    const std::string backendKey = deps_.CacheBackendKey();
    const std::string liveKey = LiveKey(backendKey, issueId);
    live_.Invalidate(liveKey);
}

void IssueTransitionsCacheService::OnConnectivityRecovered() {
    live_.OnConnectivityRecovered();
}

void IssueTransitionsCacheService::EnsureLearnedLoaded(const std::string& backendKey) {
    std::lock_guard<std::mutex> lock(learnedMutex_);
    if (learnedLoadedBackends_.count(backendKey)) {
        return; // Already loaded.
    }
    learnedLoadedBackends_.insert(backendKey);

    auto store = deps_.LookupCacheShared();
    if (!store) {
        return; // Cache not available.
    }

    deps_.LaunchBackgroundTask([backendKey, store, this]() {
        const auto rows = store->LoadLookups(backendKey, smatchet::workflow::kLearnedTransitionsKind);
        std::lock_guard<std::mutex> lock(learnedMutex_);
        for (const auto& row : rows) {
            std::vector<TrackerFieldOption> opts;
            if (smatchet::workflow::ParseTransitionTargets(row.PayloadJson, opts)) {
                learned_.emplace(backendKey + "|" + row.CacheKey, opts);
            }
        }
    });
}

void IssueTransitionsCacheService::RememberLearned(const std::string& backendKey, const TransitionsQuery& q,
                                                    const std::vector<TrackerFieldOption>& options,
                                                    const std::shared_ptr<ILookupCache>& store) {
    if (q.ProjectKey.empty() || q.IssueTypeKey.empty() || q.FromStatusKey.empty() || options.empty()) {
        return; // Any missing part prevents learning.
    }

    // Don't learn if any option's Id equals the from-status (server moved; avoid wrong edge).
    for (const auto& opt : options) {
        if (!opt.Id.empty() && opt.Id == q.FromStatusKey) {
            return;
        }
    }

    const std::string learnedKey =
        smatchet::workflow::BuildLearnedTransitionsKey(q.ProjectKey, q.IssueTypeKey, q.FromStatusKey);
    const std::string storeKey = backendKey + "|" + learnedKey;

    // Store in-memory.
    {
        std::lock_guard<std::mutex> lock(learnedMutex_);
        learned_[storeKey] = options;
    }

    // Persist to SQLite.
    if (store) {
        store->UpsertLookup(backendKey, smatchet::workflow::kLearnedTransitionsKind, learnedKey,
                            smatchet::workflow::SerializeTransitionTargets(options));
    }
}
