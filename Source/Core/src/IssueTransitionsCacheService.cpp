#include "IssueTransitionsCacheService.h"

#include "Config/ConfigManager.h"
#include "IEditMetaDeps.h"
#include "ILookupCache.h"
#include "ITrackerBackend.h"
#include "ITrackerFieldCatalog.h"
#include "LearnedWorkflowPure.h"
#include "Logger.h"
#include "OfflineFirstPure.h"
#include "Tracker/TrackerError.h"

#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

IssueTransitionsCacheService::IssueTransitionsCacheService(IEditMetaDeps& deps) : deps_(deps) {}

std::string IssueTransitionsCacheService::LiveKey(const std::string& backendKey, const std::string& issueId) {
    return backendKey + "|" + issueId;
}

std::string IssueTransitionsCacheService::LearnedMemoryKey(const std::string& backendKey,
                                                           const std::string& learnedKey) {
    return backendKey + "|" + learnedKey;
}

bool IssueTransitionsCacheService::BackendSupportsTransitions() const {
    const std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    const ITrackerFieldCatalog* catalog = backend ? backend->FieldCatalog() : nullptr;
    return catalog != nullptr && catalog->SupportsIssueTransitions();
}

TransitionsLookup IssueTransitionsCacheService::GetAvailableTransitions(const TransitionsQuery& q) const {
    TransitionsLookup lookup;
    if (q.IssueId.empty() || !BackendSupportsTransitions()) {
        return lookup;
    }
    lookup.applicable = true;
    const std::string backendKey = deps_.CacheBackendKey();
    const std::string liveKey = LiveKey(backendKey, q.IssueId);
    const TrackerConnectivityState connectivity = deps_.TrackerConnectivity();
    const auto entry = live_.Get(liveKey);
    if (entry.HasValue && entry.Live) {
        lookup.options = entry.Payload;
        lookup.freshness = live_.Freshness(liveKey, connectivity);
        return lookup;
    }
    const std::string learnedKey =
        smatchet::workflow::BuildLearnedTransitionsKey(q.ProjectKey, q.IssueTypeKey, q.FromStatusKey);
    if (!learnedKey.empty()) {
        std::lock_guard<std::mutex> lock(learnedMutex_);
        const auto it = learned_.find(LearnedMemoryKey(backendKey, learnedKey));
        if (it != learned_.end()) {
            lookup.options = it->second;
            lookup.fromLearned = true;
        }
    }
    smatchet::offline::FreshnessInputs in;
    in.HasCache = !lookup.options.empty();
    in.Live = false;
    in.InFlight = entry.InFlight;
    in.LastAttemptFailed = entry.LastAttemptFailed;
    in.Connectivity = connectivity;
    lookup.freshness = smatchet::offline::ClassifyFreshness(in);
    return lookup;
}

void IssueTransitionsCacheService::EnsureIssueTransitionsLoaded(const TransitionsQuery& q,
                                                                const TrackerConfig* configSnapshot) {
    if (q.IssueId.empty()) {
        return;
    }
    const std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    ITrackerFieldCatalog* catalog = backend ? backend->FieldCatalog() : nullptr;
    if (catalog == nullptr || !catalog->SupportsIssueTransitions()) {
        return; // not applicable: no fetch, no log
    }
    const std::string backendKey = deps_.CacheBackendKey();
    EnsureLearnedLoaded(backendKey);

    using TransitionsCache = smatchet::offline::KeyedLookupCache<std::vector<TrackerFieldOption>>;
    TransitionsCache::Ticket ticket;
    if (!live_.TryBeginFetch(LiveKey(backendKey, q.IssueId), deps_.TrackerConnectivity(),
                             smatchet::offline::Clock::now(), ticket)) {
        return; // in flight, already live, offline, or inside the failure backoff
    }
    const std::shared_ptr<ILookupCache> store = deps_.LookupCacheShared();
    const bool haveCfg = configSnapshot != nullptr;
    TrackerConfig cfg = haveCfg ? *configSnapshot : TrackerConfig();
    try {
        // `backend` keeps `catalog` alive for the whole fetch (ADR-0012 latched handle).
        deps_.LaunchBackgroundTask([this, backend, catalog, ticket, q, backendKey, store, haveCfg, cfg]() {
            const TrackerConfig useCfg = haveCfg ? cfg : ConfigManager::Load();
            smatchet::offline::RunKeyedFetch(live_, ticket, [&]() {
                auto r = catalog->FetchIssueTransitions(useCfg, q.IssueId);
                if (r.has_value()) {
                    RememberLearned(backendKey, q, r.value(), store);
                } else {
                    LOG_DEBUG("IssueTransitionsCacheService: transitions fetch failed issue=%s kind=%s",
                              q.IssueId.c_str(), ToString(r.error().Kind));
                }
                return r;
            });
        });
    } catch (const std::exception& ex) {
        // The launch itself failed (or, with an inline runner, the fetch threw): record a failure so
        // the entry backs off and retries instead of staying in flight forever.
        LOG_WARN("IssueTransitionsCacheService: transitions fetch for %s did not complete: %s", q.IssueId.c_str(),
                 ex.what());
        live_.CompleteFailure(ticket, TrackerErrorUnknown("transitions fetch did not complete"),
                              smatchet::offline::Clock::now());
    }
}

void IssueTransitionsCacheService::InvalidateIssueTransitions(const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    live_.Invalidate(LiveKey(deps_.CacheBackendKey(), issueId));
}

void IssueTransitionsCacheService::OnConnectivityRecovered() { live_.OnConnectivityRecovered(); }

void IssueTransitionsCacheService::EnsureLearnedLoaded(const std::string& backendKey) {
    const std::shared_ptr<ILookupCache> store = deps_.LookupCacheShared();
    if (!store) {
        return; // no local store yet; try again on the next call
    }
    {
        std::lock_guard<std::mutex> lock(learnedMutex_);
        if (!learnedLoadedBackends_.insert(backendKey).second) {
            return;
        }
    }
    try {
        // SQLite read on a worker; never hold learnedMutex_ across it or across the launch.
        deps_.LaunchBackgroundTask([this, backendKey, store]() {
            const std::vector<LookupCacheRow> rows =
                store->LoadLookups(backendKey, smatchet::workflow::kLearnedTransitionsKind);
            std::vector<std::pair<std::string, std::vector<TrackerFieldOption>>> parsed;
            parsed.reserve(rows.size());
            for (const LookupCacheRow& row : rows) {
                std::vector<TrackerFieldOption> opts;
                if (smatchet::workflow::ParseTransitionTargets(row.PayloadJson, opts) && !opts.empty()) {
                    parsed.emplace_back(LearnedMemoryKey(backendKey, row.CacheKey), std::move(opts));
                }
            }
            std::lock_guard<std::mutex> lock(learnedMutex_);
            for (auto& kv : parsed) {
                learned_.emplace(std::move(kv.first), std::move(kv.second)); // never overwrite a newer edge
            }
        });
    } catch (const std::exception& ex) {
        LOG_WARN("IssueTransitionsCacheService: loading the saved workflow did not complete: %s", ex.what());
        std::lock_guard<std::mutex> lock(learnedMutex_);
        learnedLoadedBackends_.erase(backendKey);
    }
}

void IssueTransitionsCacheService::RememberLearned(const std::string& backendKey, const TransitionsQuery& q,
                                                   const std::vector<TrackerFieldOption>& options,
                                                   const std::shared_ptr<ILookupCache>& store) {
    if (options.empty()) {
        return;
    }
    const std::string learnedKey =
        smatchet::workflow::BuildLearnedTransitionsKey(q.ProjectKey, q.IssueTypeKey, q.FromStatusKey);
    if (learnedKey.empty()) {
        return;
    }
    for (const TrackerFieldOption& opt : options) {
        if (opt.Id == q.FromStatusKey) {
            return; // the issue moved on the server; these targets belong to another from-status
        }
    }
    {
        std::lock_guard<std::mutex> lock(learnedMutex_);
        learned_[LearnedMemoryKey(backendKey, learnedKey)] = options;
    }
    if (store) {
        store->UpsertLookup(backendKey, smatchet::workflow::kLearnedTransitionsKind, learnedKey,
                            smatchet::workflow::SerializeTransitionTargets(options));
    }
}
