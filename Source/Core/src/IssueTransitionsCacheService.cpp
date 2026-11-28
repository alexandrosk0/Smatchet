#include "IssueTransitionsCacheService.h"

#include "Config/ConfigManager.h"
#include "IEditMetaDeps.h"
#include "ILookupCache.h"
#include "ITrackerBackend.h"
#include "ITrackerFieldCatalog.h"
#include "LearnedWorkflowPure.h"
#include "Logger.h"
#include "OfflineFirstPure.h"
#include "StringUtil.h"
#include "TrackerError.h"

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
    // Live only for the from-status it was fetched for, and only while the issue was still there.
    if (entry.HasValue && entry.Live && !entry.Payload.ServerElsewhere &&
        entry.Payload.FromStatusKey == q.FromStatusKey) {
        lookup.options = entry.Payload.Options;
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

    using TransitionsCache = smatchet::offline::KeyedLookupCache<LiveTransitions>;
    TransitionsCache::Ticket ticket;
    // The combo asks every frame it is open, so this must stay a cheap refusal: a live set is admitted again only
    // for another from-status or past its max age (the predicate runs under the cache's mutex).
    if (!live_.TryBeginFetchIf(LiveKey(backendKey, q.IssueId), deps_.TrackerConnectivity(), deps_.SteadyNow(), ticket,
                               smatchet::offline::LiveMaxAgeAdmission(/*revalidate=*/false),
                               [&q](const LiveTransitions& live) { return live.FromStatusKey != q.FromStatusKey; })) {
        return; // in flight, current, offline, or inside the failure backoff
    }
    const std::shared_ptr<ILookupCache> store = deps_.LookupCacheShared();
    const bool haveCfg = configSnapshot != nullptr;
    TrackerConfig cfg = haveCfg ? *configSnapshot : TrackerConfig();
    try {
        // `backend` keeps `catalog` alive for the whole fetch (ADR-0012 latched handle).
        deps_.LaunchBackgroundTask([this, backend, catalog, ticket, q, backendKey, store, haveCfg, cfg]() {
            // Everything that can throw, the config load included, runs inside RunKeyedFetch: its guard
            // records a failure on every exit, so a throw can never leave the ticket in flight.
            using FetchResult = Result<LiveTransitions, TrackerError>;
            const auto fetchBody = [&]() {
                const TrackerConfig useCfg = haveCfg ? cfg : ConfigManager::Load();
                auto r = catalog->FetchIssueTransitions(useCfg, q.IssueId);
                if (!r.has_value()) {
                    LOG_DEBUG("IssueTransitionsCacheService: transitions fetch failed issue=%s kind=%s",
                              q.IssueId.c_str(), ToString(r.error().Kind));
                    return FetchResult::Err(r.error());
                }
                LiveTransitions live;
                live.IssueId = q.IssueId;
                live.FromStatusKey = q.FromStatusKey;
                live.ServerElsewhere =
                    RememberLearned(backendKey, q, r.value(), store) == LearnOutcome::ServerElsewhere;
                live.Options = std::move(r.value());
                return FetchResult::Ok(std::move(live));
            };
            smatchet::offline::RunKeyedFetch(live_, ticket, fetchBody, [this]() { return deps_.SteadyNow(); });
        });
    } catch (const std::exception& ex) {
        // The launch itself failed (or, with an inline runner, the fetch threw): record a failure so
        // the entry backs off and retries instead of staying in flight forever.
        LOG_WARN("IssueTransitionsCacheService: transitions fetch for %s did not complete: %s", q.IssueId.c_str(),
                 ex.what());
        live_.CompleteFailure(ticket, TrackerErrorUnknown("transitions fetch did not complete"), deps_.SteadyNow());
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
                if (learnedErased_.count(kv.first) != 0) {
                    continue; // erased after this load read its row: the stored copy is obsolete
                }
                learned_.emplace(std::move(kv.first), std::move(kv.second)); // never overwrite a newer edge
            }
        });
}

IssueTransitionsCacheService::LearnOutcome
IssueTransitionsCacheService::RememberLearned(const std::string& backendKey, const TransitionsQuery& q,
                                              const std::vector<TrackerFieldOption>& options,
                                              const std::shared_ptr<ILookupCache>& store) {
    if (options.empty()) {
        return LearnOutcome::Skipped;
    }
    const std::string learnedKey =
        smatchet::workflow::BuildLearnedTransitionsKey(q.ProjectKey, q.IssueTypeKey, q.FromStatusKey);
    if (learnedKey.empty()) {
        return LearnOutcome::Skipped;
    }
    // The from-status itself among the targets means the issue moved on the server (these targets
    // belong to another from-status) — unless a global or looped transition offers it, which Jira lists
    // from every status (every status of a team-managed workflow allows all). Such a self-target is
    // dropped from what is remembered; the combo always shows the current status anyway.
    std::vector<TrackerFieldOption> remembered;
    remembered.reserve(options.size());
    for (const TrackerFieldOption& opt : options) {
        const bool isFromStatus =
            opt.Id == q.FromStatusKey || (!q.FromStatusName.empty() && !opt.Value.empty() &&
                                          ToLowerAsciiCopy(opt.Value) == ToLowerAsciiCopy(q.FromStatusName));
        if (isFromStatus) {
            if (!opt.ReachableFromAnyStatus) {
                return LearnOutcome::ServerElsewhere;
            }
            continue;
        }
        remembered.push_back(opt);
    }
    if (remembered.empty()) {
        // Every option was a global or looped self-target, so this status has no other move now. An
        // edge learned from an earlier fetch is obsolete and must not be offered offline.
        {
            const std::string memoryKey = LearnedMemoryKey(backendKey, learnedKey);
            std::lock_guard<std::mutex> lock(learnedMutex_);
            learned_.erase(memoryKey);
            learnedErased_.insert(memoryKey);
        }
        if (store) {
            store->DeleteLookup(backendKey, smatchet::workflow::kLearnedTransitionsKind, learnedKey);
        }
        return LearnOutcome::Erased;
    }
    {
        const std::string memoryKey = LearnedMemoryKey(backendKey, learnedKey);
        std::lock_guard<std::mutex> lock(learnedMutex_);
        learned_[memoryKey] = remembered;
        learnedErased_.erase(memoryKey);
    }
    if (store) {
        store->UpsertLookup(backendKey, smatchet::workflow::kLearnedTransitionsKind, learnedKey,
                            smatchet::workflow::SerializeTransitionTargets(remembered));
    }
    return LearnOutcome::Remembered;
}
