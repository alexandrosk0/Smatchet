#include "ProjectComponentsCacheService.h"

#include "CachedTicketTypes.h"
#include "FieldOptionsJsonPure.h"
#include "IEditMetaDeps.h"
#include "ILookupCache.h"
#include "ITrackerBackend.h"
#include "ITrackerFieldCatalog.h"
#include "LearnedWorkflowPure.h"
#include "Logger.h"
#include "LookupPayloadsPure.h"
#include "OfflineFirstPure.h"
#include "Sync/LookupFreshnessPure.h"
#include "Tracker/ProjectResolver.h"
#include "Tracker/TrackerError.h"

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using OptionsList = std::vector<TrackerFieldOption>;

// One cache entry per (tracker, project); the parts are escaped, so two pairs never collide.
std::string EntryKey(const std::string& backendKey, const std::string& projectKey) {
    return smatchet::workflow::BuildScopedKey(backendKey, projectKey);
}

} // namespace

ProjectComponentsCacheService::ProjectComponentsCacheService(IEditMetaDeps& deps) : deps_(deps) {}

ProjectComponentsLookup ProjectComponentsCacheService::GetComponentOptions(const std::string& projectKey) const {
    ProjectComponentsLookup lookup;
    if (projectKey.empty()) {
        return lookup;
    }
    const OptionsCache::Entry entry = cache_.Get(EntryKey(deps_.CacheBackendKey(), projectKey));
    if (entry.HasValue && entry.Payload) {
        lookup.options = entry.Payload;
    }
    smatchet::offline::FreshnessInputs in;
    in.HasCache = lookup.options != nullptr;
    in.Live = entry.Live;
    in.InFlight = entry.InFlight;
    in.LastAttemptFailed = entry.LastAttemptFailed;
    in.Connectivity = deps_.TrackerConnectivity();
    lookup.freshness = smatchet::offline::ClassifyFreshness(in);
    lookup.inFlight = entry.InFlight;
    lookup.lastAttemptFailed = entry.LastAttemptFailed;
    lookup.lastError = entry.LastError;
    return lookup;
}

bool ProjectComponentsCacheService::EnsureComponentsLoaded(const std::string& projectKey) {
    return BeginFetch(projectKey, /*bypassSaved=*/false);
}

bool ProjectComponentsCacheService::RefreshComponents(const std::string& projectKey) {
    if (projectKey.empty()) {
        return false;
    }
    cache_.ClearBackoff(EntryKey(deps_.CacheBackendKey(), projectKey));
    return BeginFetch(projectKey, /*bypassSaved=*/true);
}

bool ProjectComponentsCacheService::BeginFetch(const std::string& projectKey, bool bypassSaved) {
    if (projectKey.empty()) {
        return false;
    }
    const std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend || backend->FieldCatalog() == nullptr) {
        return false;
    }
    const std::string backendKey = deps_.CacheBackendKey();
    EnsureSavedLoaded(backendKey);

    OptionsCache::Ticket ticket;
    if (!cache_.TryBeginFetch(EntryKey(backendKey, projectKey), deps_.TrackerConnectivity(),
                              smatchet::offline::Clock::now(), ticket)) {
        return false; // live, in flight, offline, inside the failure backoff, or a saved list still in its lifetime
    }
    const std::shared_ptr<ILookupCache> store = deps_.LookupCacheShared();
    try {
        // `backend` keeps its field catalog alive for the whole fetch (ADR-0012 latched handle).
        deps_.LaunchBackgroundTask([this, backend, ticket, backendKey, projectKey, store, bypassSaved]() {
            FetchOne(backend, ticket, backendKey, projectKey, nullptr, store, bypassSaved);
        });
    } catch (const std::exception& ex) {
        // The launch failed (or, with an inline runner, the fetch threw): record a failure so the entry
        // backs off and retries instead of staying in flight.
        LOG_WARN("ProjectComponentsCacheService: component fetch for %s did not complete: %s", projectKey.c_str(),
                 ex.what());
        cache_.CompleteFailure(ticket, TrackerErrorUnknown("component fetch did not complete"),
                               smatchet::offline::Clock::now());
    }
    return true;
}

void ProjectComponentsCacheService::WarmForActiveTicketsAsync(TrackerConfig trackerCfgForWorker) {
    const std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend || backend->FieldCatalog() == nullptr) {
        return;
    }
    const std::string backendKey = deps_.CacheBackendKey();
    EnsureSavedLoaded(backendKey);

    if (smatchet::offline::IsOfflineState(deps_.TrackerConnectivity())) {
        return; // the saved rows (loading above) are all there is until the tracker is back
    }
    // Distinct Jira project keys (issue-key prefixes) across the active tickets, so each row's editor
    // offers its own project's components. Projects already live or in flight need no worker.
    const auto ticketsSnap = deps_.GetActiveTicketsSnapshot();
    std::vector<std::string> projectKeys;
    std::unordered_set<std::string> seen;
    for (const CachedTicket& ticket : *ticketsSnap) {
        const std::string projectKey = smatchet::ExtractIssueKeyPrefix(ticket.id);
        if (projectKey.empty() || !seen.insert(projectKey).second) {
            continue;
        }
        const OptionsCache::Entry entry = cache_.Get(EntryKey(backendKey, projectKey));
        if (!entry.InFlight && !(entry.Live && !entry.LastAttemptFailed)) {
            projectKeys.push_back(projectKey);
        }
    }
    if (projectKeys.empty()) {
        return;
    }
    const std::shared_ptr<ILookupCache> store = deps_.LookupCacheShared();
    try {
        deps_.LaunchBackgroundTask(
            [this, backend, projectKeys, backendKey, store, cfg = std::move(trackerCfgForWorker)]() {
                for (const std::string& projectKey : projectKeys) {
                    if (deps_.IsShuttingDown()) {
                        break;
                    }
                    // Take each ticket only when its turn comes, so a lazy open of a later project is never
                    // held behind this warm (and vice versa: a project already in flight is skipped).
                    OptionsCache::Ticket ticket;
                    if (cache_.TryBeginFetch(EntryKey(backendKey, projectKey), deps_.TrackerConnectivity(),
                                             smatchet::offline::Clock::now(), ticket)) {
                        FetchOne(backend, ticket, backendKey, projectKey, &cfg, store);
                    }
                }
            });
    } catch (const std::exception& ex) {
        LOG_WARN("ProjectComponentsCacheService: component warm did not start: %s", ex.what());
    }
}

void ProjectComponentsCacheService::OnConnectivityRecovered() {
    cache_.OnConnectivityRecovered();
    savedLoad_.ClearBackoff();
}

void ProjectComponentsCacheService::FetchOne(const std::shared_ptr<ITrackerBackend>& backend,
                                             const OptionsCache::Ticket& ticket, const std::string& backendKey,
                                             const std::string& projectKey, const TrackerConfig* cfgSnapshot,
                                             const std::shared_ptr<ILookupCache>& store, bool bypassSaved) {
    using FetchResult = Result<OptionsPtr, TrackerError>;
    OptionsPtr fetched;
    bool servedFromSaved = false;
    // Everything that can throw, the config load included, runs inside RunKeyedFetch: its guard records
    // a failure on every exit, so a throw can never leave the ticket in flight.
    smatchet::offline::RunKeyedFetch(cache_, ticket, [&]() {
        if (deps_.IsShuttingDown()) {
            return FetchResult::Err(TrackerErrorCancelled("shutting down"));
        }
        ITrackerFieldCatalog* catalog = backend ? backend->FieldCatalog() : nullptr;
        if (catalog == nullptr) {
            return FetchResult::Err(TrackerErrorInvalidRequest("The tracker has no field catalog."));
        }
        const TrackerConfig cfg = cfgSnapshot ? *cfgSnapshot : ConfigManager::Load();
        // A saved list younger than its lifetime stands in for the live fetch (and keeps its save time, so it
        // does expire). It stays a SAVED copy: shown under the "saved" cue, not refetched before it expires, and
        // replaced at once by a user's Refresh. An empty saved list never stands in: "no components" must come
        // from the tracker, not from a copy.
        if (store && cfg.LookupCacheTtlSec > 0 && !bypassSaved) {
            try {
                LookupCacheRow row;
                std::vector<TrackerFieldOption> saved;
                const std::int64_t nowSec =
                    static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                  std::chrono::system_clock::now().time_since_epoch())
                                                  .count());
                if (store->TryGetLookup(backendKey, smatchet::lookup::kProjectComponentsKind, projectKey, row) &&
                    smatchet::sync::IsLookupRowFresh(row.UpdatedAtEpochSec, nowSec, cfg.LookupCacheTtlSec) &&
                    smatchet::fieldoptions::ParseFieldOptions(row.PayloadJson, saved) && !saved.empty()) {
                    fetched = std::make_shared<const OptionsList>(std::move(saved));
                    servedFromSaved = true;
                    const auto untilStale = std::chrono::seconds(
                        smatchet::sync::SecondsUntilLookupStale(row.UpdatedAtEpochSec, nowSec, cfg.LookupCacheTtlSec));
                    cache_.CompleteFromSaved(ticket, fetched, smatchet::offline::Clock::now() + untilStale);
                    return FetchResult::Ok(fetched); // the spent ticket makes RunKeyedFetch's success a no-op
                }
            } catch (const std::exception& ex) {
                LOG_WARN("ProjectComponentsCacheService: reading saved components for %s failed: %s",
                         projectKey.c_str(), ex.what());
            }
        }
        auto result = catalog->FetchProjectComponents(cfg, projectKey);
        if (!result) {
            LOG_DEBUG("ProjectComponentsCacheService: component fetch failed project=%s kind=%s err=%s",
                      projectKey.c_str(), ToString(result.error().Kind), result.error().Detail.c_str());
            return FetchResult::Err(result.error());
        }
        // Presence means loaded: a project with no components settles to an empty list, not "Loading".
        fetched = std::make_shared<const OptionsList>(std::move(result.value().Options));
        return FetchResult::Ok(fetched);
    });
    if (!fetched || !store || servedFromSaved) {
        return;
    }
    // Save after the live value is recorded, so a store failure can never cost the user the live list.
    try {
        store->UpsertLookup(backendKey, smatchet::lookup::kProjectComponentsKind, projectKey,
                            smatchet::fieldoptions::SerializeFieldOptions(*fetched));
    } catch (const std::exception& ex) {
        LOG_WARN("ProjectComponentsCacheService: saving components for %s failed: %s", projectKey.c_str(), ex.what());
    }
}

void ProjectComponentsCacheService::EnsureSavedLoaded(const std::string& backendKey) {
    // SQLite read and JSON parse on a worker; SeedFromStore never overrides a list fetched live meanwhile.
    smatchet::offline::LoadSavedRowsOnce(
        deps_, savedLoad_, backendKey, smatchet::lookup::kProjectComponentsKind, "saved components",
        [this](const std::string& key, const std::vector<LookupCacheRow>& rows) {
            for (const LookupCacheRow& row : rows) {
                std::vector<TrackerFieldOption> options;
                if (!row.CacheKey.empty() && smatchet::fieldoptions::ParseFieldOptions(row.PayloadJson, options)) {
                    cache_.SeedFromStore(EntryKey(key, row.CacheKey),
                                         std::make_shared<const OptionsList>(std::move(options)));
                }
            }
        });
}
