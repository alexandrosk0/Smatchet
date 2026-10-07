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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

IssueTransitionsCacheService::IssueTransitionsCacheService(IEditMetaDeps& deps) : deps_(deps) {}

// Composite keys escape each free-text part (EscapeKeyPart) so two backends or issues never collide.
std::string IssueTransitionsCacheService::LiveKey(const std::string& backendKey, const std::string& issueId) {
    return smatchet::workflow::BuildScopedKey(backendKey, issueId);
}

std::string IssueTransitionsCacheService::LearnedMemoryKey(const std::string& backendKey,
                                                           const std::string& learnedKey) {
    return smatchet::workflow::EscapeKeyPart(backendKey) + "|" + learnedKey; // learnedKey is already escaped
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
        lookup.fromLive = true;
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
            // Everything that can throw, the config load included, runs inside RunKeyedFetch: its guard
            // records a failure on every exit, so a throw can never leave the ticket in flight.
            smatchet::offline::RunKeyedFetch(live_, ticket, [&]() {
                const TrackerConfig useCfg = haveCfg ? cfg : ConfigManager::Load();
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
    InvalidateIssueTransitions(deps_.CacheBackendKey(), issueId);
}

void IssueTransitionsCacheService::InvalidateIssueTransitions(const std::string& backendKey,
                                                              const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    live_.Invalidate(LiveKey(backendKey, issueId));
}

void IssueTransitionsCacheService::OnConnectivityRecovered() {
    live_.OnConnectivityRecovered();
    learnedLoad_.ClearBackoff();
}

void IssueTransitionsCacheService::EnsureLearnedLoaded(const std::string& backendKey) {
    // SQLite read on a worker; never hold learnedMutex_ across it or across the launch.
    smatchet::offline::LoadSavedRowsOnce(
        deps_, learnedLoad_, backendKey, smatchet::workflow::kLearnedTransitionsKind, "the saved workflow",
        [this](const std::string& key, const std::vector<LookupCacheRow>& rows) {
            std::vector<std::pair<std::string, std::vector<TrackerFieldOption>>> parsed;
            parsed.reserve(rows.size());
            for (const LookupCacheRow& row : rows) {
                std::vector<TrackerFieldOption> opts;
                if (smatchet::workflow::ParseTransitionTargets(row.PayloadJson, opts) && !opts.empty()) {
                    parsed.emplace_back(LearnedMemoryKey(key, row.CacheKey), std::move(opts));
                }
            }
            std::lock_guard<std::mutex> lock(learnedMutex_);
            for (auto& kv : parsed) {
                learned_.emplace(std::move(kv.first), std::move(kv.second)); // never overwrite a newer edge
            }
        });
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
    // The from-status itself among the targets means the issue moved on the server (these targets
    // belong to another from-status) — unless a global or looped transition offers it, which Jira lists
    // from every status (every status of a team-managed workflow allows all). Such a self-target is
    // dropped from what is remembered; the combo always shows the current status anyway.
    std::vector<TrackerFieldOption> remembered;
    remembered.reserve(options.size());
    for (const TrackerFieldOption& opt : options) {
        if (opt.Id == q.FromStatusKey) {
            if (!opt.ReachableFromAnyStatus) {
                return;
            }
            continue;
        }
        remembered.push_back(opt);
    }
    if (remembered.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(learnedMutex_);
        learned_[LearnedMemoryKey(backendKey, learnedKey)] = remembered;
    }
    if (store) {
        store->UpsertLookup(backendKey, smatchet::workflow::kLearnedTransitionsKind, learnedKey,
                            smatchet::workflow::SerializeTransitionTargets(remembered));
    }
}
