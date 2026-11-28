#include "EditMetaCacheService.h"

#include "TrackerFieldPayloadPure.h"

#include "EditMetaScopePure.h"
#include "IEditMetaDeps.h"
#include "LaunchClaim.h"
#include "ILookupCache.h"
#include "ITrackerBackend.h"
#include "ITrackerFieldCatalog.h" // full def: FetchIssueEditMeta, IssueEditMetaVariesPerIssue
#include "LearnedWorkflowPure.h"  // BuildScopedKey (in-flight / scope keys)
#include "LookupPayloadsPure.h"   // the editmeta kinds + the permissions payload codec
#include "Config/ConfigManager.h"
#include "TrackerFieldValueUtils.h"
#include "TrackerFieldSchema.h"
#include "CachedTicketTypes.h"

#include "Logger.h"
#include "OfflineFirstPure.h" // IsOfflineState, kLookupRetryAfterSeconds (Pillar 6 gate + backoff)
#include "ScopeExit.h"
#include "StringUtil.h"
#include "Sync/LookupFreshnessPure.h" // IsLookupRowFresh: the 30-day ceiling on a restored scope
#include "TimeNowPure.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId) {
    return TrackerFieldValueUtils::IsEditableTimetrackingEstimateFieldId(fieldId);
}

std::string IssueTypeKeyOfTicket(const CachedTicket& ticket) {
    return ToLowerAsciiCopy(TrimCopy(ticket.GetFieldValue("issuetype")));
}

} // namespace

EditMetaCacheService::EditMetaCacheService(IEditMetaDeps& deps)
    : deps_(deps), scopeSaveIo_([this](std::function<void()> task) { deps_.LaunchBackgroundTask(std::move(task)); }) {}

std::string EditMetaCacheService::ResolveIssueTypeKeyForIssue(const std::string& issueId) const {
    if (issueId.empty()) {
        return std::string();
    }
    const auto ticketsSnap = deps_.GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    const auto it =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (it == tickets.end()) {
        return std::string();
    }
    return IssueTypeKeyOfTicket(*it);
}

const EditMetaCacheService::IssueEditMetaCache*
EditMetaCacheService::FindLoaded(const EditMetaByBackend& maps, const std::string& backendKey, const std::string& id) {
    const auto backendIt = maps.find(backendKey);
    if (backendIt == maps.end()) {
        return nullptr;
    }
    const auto it = backendIt->second.find(id);
    return (it != backendIt->second.end() && it->second.loaded) ? &it->second : nullptr;
}

const EditMetaCacheService::IssueEditMetaCache* EditMetaCacheService::FindScopeLoaded(const ScopeByBackend& maps,
                                                                                      const std::string& backendKey,
                                                                                      const std::string& project,
                                                                                      const std::string& issueTypeKey) {
    const auto backendIt = maps.find(backendKey);
    if (backendIt == maps.end()) {
        return nullptr;
    }
    const auto projectIt = backendIt->second.find(project);
    if (projectIt == backendIt->second.end()) {
        return nullptr;
    }
    const auto typeIt = projectIt->second.find(issueTypeKey);
    return (typeIt != projectIt->second.end() && typeIt->second.loaded) ? &typeIt->second : nullptr;
}

bool EditMetaCacheService::ScopeProjectFor(const std::string& issueId, const std::string& issueTypeKey,
                                           bool variesPerIssue, std::string& project) {
    if (issueTypeKey.empty()) {
        return false;
    }
    project = smatchet::editmeta::ProjectOfIssue(issueId);
    // A tracker whose answer varies per issue needs the project to say whose answer it is: a local draft or a
    // key of another shape stays with its own entry. One whose answer does not vary has one scope per type.
    return !project.empty() || !variesPerIssue;
}

std::string EditMetaCacheService::InFlightKey(const std::string& backendKey, const std::string& issueId) {
    return smatchet::workflow::BuildScopedKey(backendKey, issueId);
}

void EditMetaCacheService::WarmIssueTypeEditMetaAtStartAsync(TrackerConfig trackerCfgForWorker) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend) {
        return;
    }
    const std::string backendKey = deps_.CacheBackendKey();
    EnsureSavedTypesLoaded(backendKey);
    const ITrackerFieldCatalog* const catalog = backend->FieldCatalog();
    const auto ticketsSnap = deps_.GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;

    // One entry per scope without a live answer, in roster order, capped; each carries up to
    // kMaxCandidatesPerScope tickets so a non-representative first ticket (a Done issue) can be passed over.
    constexpr std::size_t kSkipScope = static_cast<std::size_t>(-1);
    std::vector<ScopeCandidates> scopes;
    std::unordered_map<std::string, std::size_t> indexByScope;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        for (const CachedTicket& ticket : tickets) {
            if (ticket.id.empty()) {
                continue;
            }
            const std::string typeKey = IssueTypeKeyOfTicket(ticket);
            const bool varies = catalog != nullptr && catalog->IssueEditMetaVariesPerIssue(ticket.id);
            std::string project;
            if (!ScopeProjectFor(ticket.id, typeKey, varies, project)) {
                continue;
            }
            const std::string scopeKey = smatchet::editmeta::BuildScopeStoreKey(project, typeKey);
            const auto found = indexByScope.find(scopeKey);
            if (found == indexByScope.end()) {
                if (scopes.size() >= smatchet::editmeta::kMaxScopesPerWarm) {
                    continue; // the rest wait for the next warm
                }
                // A restored (not live) entry still gets one live fetch per session.
                const IssueEditMetaCache* scopeMeta = FindScopeLoaded(scopeEditMeta_, backendKey, project, typeKey);
                const auto backoff = scopeNoRepresentativeUntil_.find(InFlightKey(backendKey, scopeKey));
                const bool skip = (scopeMeta != nullptr && scopeMeta->live) ||
                                  (backoff != scopeNoRepresentativeUntil_.end() && now < backoff->second);
                if (skip) {
                    indexByScope.emplace(scopeKey, kSkipScope);
                    continue;
                }
                indexByScope.emplace(scopeKey, scopes.size());
                ScopeCandidates entry;
                entry.Project = project;
                entry.IssueTypeKey = typeKey;
                entry.IssueIds.push_back(ticket.id);
                scopes.push_back(std::move(entry));
            } else if (found->second != kSkipScope &&
                       scopes[found->second].IssueIds.size() < smatchet::editmeta::kMaxCandidatesPerScope) {
                scopes[found->second].IssueIds.push_back(ticket.id);
            }
        }
    }
    if (scopes.empty()) {
        return;
    }
    try {
        deps_.LaunchBackgroundTask([this, scopes, backend, backendKey, cfg = std::move(trackerCfgForWorker)]() {
            WarmIssueTypeEditMetaWorker(scopes, backend, backendKey, cfg);
        });
    } catch (const std::exception& ex) {
        LOG_WARN("EditMetaCacheService: issue-type editmeta warm did not start: %s", ex.what());
    }
}

void EditMetaCacheService::WarmIssueTypeEditMetaWorker(const std::vector<ScopeCandidates>& scopes,
                                                       const std::shared_ptr<ITrackerBackend>& backend,
                                                       const std::string& backendKey,
                                                       const TrackerConfig& trackerCfgForWorker) {
    std::size_t extraFetches = smatchet::editmeta::kMaxExtraCandidateFetchesPerWarm;
    for (const ScopeCandidates& scope : scopes) {
        bool promoted = false;
        bool triedAll = true;
        bool failedAny = false;
        for (std::size_t i = 0; i < scope.IssueIds.size(); ++i) {
            if (deps_.IsShuttingDown()) {
                return;
            }
            if (i > 0) {
                if (extraFetches == 0) {
                    triedAll = false;
                    break;
                }
                --extraFetches;
            }
            // Fire-and-forget warmup: a failed fetch is not surfaced and the issue stays optimistic. A tracker that
            // went offline ends the whole warm; any other failure (a deleted or restricted issue) only skips that
            // candidate, so one bad ticket keeps neither its siblings nor the scopes after it from being warmed.
            bool refused = false;
            const VoidResult loaded =
                LoadIssueEditMeta(backend, backendKey, scope.IssueIds[i], scope.IssueTypeKey, &trackerCfgForWorker,
                                  LoadMode::ScopeSuffices, nullptr, &refused);
            if (refused) {
                failedAny = true; // no request was made for this candidate: not evidence of "no representative result"
                continue;
            }
            if (!loaded) {
                if (smatchet::offline::IsOfflineState(deps_.TrackerConnectivity())) {
                    return;
                }
                failedAny = true; // not "no representative result": no backoff, the next warm tries again
                continue;
            }
            std::lock_guard<std::mutex> lock(editMetaMutex_);
            if (FindScopeLoaded(scopeEditMeta_, backendKey, scope.Project, scope.IssueTypeKey) != nullptr) {
                promoted = true;
                break;
            }
        }
        if (!promoted && triedAll && !failedAny) {
            // Every candidate answered, none with an editable field: ask again in half an hour, not every warm.
            const std::string scopeKey = smatchet::editmeta::BuildScopeStoreKey(scope.Project, scope.IssueTypeKey);
            std::lock_guard<std::mutex> lock(editMetaMutex_);
            scopeNoRepresentativeUntil_[InFlightKey(backendKey, scopeKey)] =
                std::chrono::steady_clock::now() +
                std::chrono::seconds(smatchet::editmeta::kNoRepresentativeBackoffSec);
        }
    }
}

void EditMetaCacheService::EnsureSavedTypesLoaded(const std::string& backendKey) {
    // SQLite read and JSON parse on a worker; editMetaMutex_ is taken only to apply the parsed rows.
    smatchet::offline::LoadSavedRowsOnce(
        deps_, savedTypesLoad_, backendKey, smatchet::lookup::kEditMetaProjectTypeKind, "saved edit permissions",
        [this](const std::string& key, const std::vector<LookupCacheRow>& rows) {
            const std::int64_t nowSec = TimeNowPure::NowUnixSeconds();
            struct Restored {
                std::string Project;
                std::string IssueTypeKey;
                IssueEditMetaCache Entry;
            };
            std::vector<Restored> restored;
            restored.reserve(rows.size());
            for (const LookupCacheRow& row : rows) {
                Restored item;
                // Older than the ceiling (or of unknown age): permissions drift, so the copy is not restored.
                if (!smatchet::sync::IsLookupRowFresh(row.UpdatedAtEpochSec, nowSec,
                                                      smatchet::editmeta::kSavedEditPermissionsMaxAgeSec) ||
                    !smatchet::editmeta::SplitScopeStoreKey(row.CacheKey, item.Project, item.IssueTypeKey) ||
                    !smatchet::lookup::ParseEditPermissions(row.PayloadJson, item.Entry.fieldCanEdit)) {
                    continue;
                }
                item.Entry.loaded = true; // live stays false: a saved copy
                restored.push_back(std::move(item));
            }
            {
                std::lock_guard<std::mutex> lock(editMetaMutex_);
                ScopeByProject& byProject = scopeEditMeta_[key];
                for (Restored& item : restored) {
                    // emplace never replaces an entry fetched live meanwhile.
                    byProject[item.Project].emplace(item.IssueTypeKey, std::move(item.Entry));
                }
            }
            PurgeLegacyTypeRows(key);
        });
}

void EditMetaCacheService::PurgeLegacyTypeRows(const std::string& backendKey) {
    const std::shared_ptr<ILookupCache> store = deps_.LookupCacheShared();
    if (!store) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        if (!legacyPurged_.insert(backendKey).second) {
            return;
        }
    }
    bool purged = false;
    try {
        // Per-type rows pooled every project of a type, so one uneditable issue narrowed them all: they are
        // never read again, and removed so an older build cannot be fed them back after a downgrade either.
        for (const LookupCacheRow& row : store->LoadLookups(backendKey, smatchet::lookup::kEditMetaTypeKind)) {
            store->DeleteLookup(backendKey, smatchet::lookup::kEditMetaTypeKind, row.CacheKey);
        }
        purged = true;
    } catch (const std::exception& ex) {
        LOG_WARN("EditMetaCacheService: deleting the legacy per-type edit permissions failed: %s", ex.what());
    }
    if (!purged) {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        legacyPurged_.erase(backendKey); // the next saved-rows load (a later session at the latest) tries again
    }
}

void EditMetaCacheService::PostScopeSave(const std::string& backendKey, const std::string& project,
                                         const std::string& issueTypeKey,
                                         const std::unordered_map<std::string, bool>& fieldCanEdit) {
    const std::shared_ptr<ILookupCache> store = deps_.LookupCacheShared();
    const std::string storeKey = smatchet::editmeta::BuildScopeStoreKey(project, issueTypeKey);
    if (!store || storeKey.empty()) {
        return;
    }
    const std::string seqKey = InFlightKey(backendKey, storeKey);
    std::uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        seq = ++nextScopeSeq_;
        latestScopeSeq_[seqKey] = seq;
    }
    std::string payload = smatchet::lookup::SerializeEditPermissions(fieldCanEdit);
    const std::string launchError =
        scopeSaveIo_.Post([this, store, backendKey, storeKey, seqKey, seq, payload = std::move(payload)]() {
            {
                std::lock_guard<std::mutex> lock(editMetaMutex_);
                // Only the task holding the newest number writes (and erases the entry). A missing entry means a
                // newer save already ran, which can happen when two posts reach the queue out of number order.
                const auto it = latestScopeSeq_.find(seqKey);
                if (it == latestScopeSeq_.end() || it->second != seq) {
                    return;
                }
                latestScopeSeq_.erase(it);
            }
            try {
                store->UpsertLookup(backendKey, smatchet::lookup::kEditMetaProjectTypeKind, storeKey, payload);
            } catch (const std::exception& ex) {
                LOG_WARN("EditMetaCacheService: saving edit permissions for scope %s failed: %s", storeKey.c_str(),
                         ex.what());
            }
        });
    if (!launchError.empty()) {
        // The save stays queued and runs with the next post (or the shutdown flush).
        LOG_WARN("EditMetaCacheService: the edit-permissions save worker did not start: %s", launchError.c_str());
    }
}

bool EditMetaCacheService::FlushScopeSaves(std::chrono::milliseconds timeout) { return scopeSaveIo_.Flush(timeout); }

bool EditMetaCacheService::CanEditFieldForIssue(const std::string& issueId, const std::string& fieldId,
                                                const TrackerField* fieldMeta,
                                                const std::string* issueTypeKeyOverride) const {
    if (!deps_.BackendShared()) {
        return true;
    }
    const bool haveOverride = issueTypeKeyOverride != nullptr && !issueTypeKeyOverride->empty();
    return CanEditFieldImpl(deps_.CacheBackendKey(), issueId, fieldId, fieldMeta,
                            haveOverride ? issueTypeKeyOverride : nullptr);
}

bool EditMetaCacheService::CanEditFieldForIssueWithType(const std::string& backendKey, const std::string& issueId,
                                                        const std::string& fieldId, const TrackerField* fieldMeta,
                                                        const std::string& issueTypeKey) const {
    return CanEditFieldImpl(backendKey, issueId, fieldId, fieldMeta, &issueTypeKey);
}

bool EditMetaCacheService::CanEditFieldImpl(const std::string& backendKey, const std::string& issueId,
                                            const std::string& fieldId, const TrackerField* fieldMeta,
                                            const std::string* explicitTypeKey) const {
    if (issueId.empty() || fieldId.empty()) {
        return true;
    }
    if (IsEditableTimetrackingEstimateFieldId(fieldId)) {
        return true;
    }
    const std::shared_ptr<const TrackerField> found =
        fieldMeta ? std::shared_ptr<const TrackerField>() : deps_.FindFieldSnapshotById(fieldId);
    const TrackerField* meta = fieldMeta ? fieldMeta : found.get();
    if (meta && TrackerFieldPayloadPure::IsSprintField(*meta)) {
        return true;
    }
    const std::string fieldKey = ToLowerAsciiCopy(fieldId);
    // Edit metadata usually does not include a plain "set" for status; Jira applies changes via
    // POST /issue/{key}/transitions (see JiraClient::UpdateIssueFields).
    if (fieldKey == "status") {
        return true;
    }
    const std::string issueTypeKey = explicitTypeKey ? *explicitTypeKey : ResolveIssueTypeKeyForIssue(issueId);
    // The scope key needs no backend capability here: no backend mixes ids that carry a project with ids that do
    // not under one issue type, so the project the write side derived is the one this derives.
    const std::string project = smatchet::editmeta::ProjectOfIssue(issueId);
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    // The issue's own editmeta, else its scope's (live or restored), else optimistic.
    const IssueEditMetaCache* loaded = FindLoaded(issueEditMeta_, backendKey, issueId);
    if (loaded == nullptr && !issueTypeKey.empty()) {
        loaded = FindScopeLoaded(scopeEditMeta_, backendKey, project, issueTypeKey);
    }
    if (loaded == nullptr) {
        return true;
    }
    const auto fieldIt = loaded->fieldCanEdit.find(fieldKey);
    if (fieldIt == loaded->fieldCanEdit.end()) {
        // Jira often omits `priority` (e.g. Epic) and `components` (cross-project / filter-id views)
        // from editmeta while PUT still accepts them; force-editable carve-out. See
        // AppController::CanEditFieldForIssue doc comment.
        return fieldKey == "priority" || fieldKey == "components";
    }
    return fieldIt->second;
}

VoidResult EditMetaCacheService::EnsureIssueEditMetaLoaded(const std::string& issueId,
                                                           const std::string* issueTypeKeyOverride,
                                                           const TrackerConfig* configSnapshot) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend || issueId.empty()) {
        return VoidOk();
    }
    const std::string backendKey = deps_.CacheBackendKey();
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        if (FindLoaded(issueEditMeta_, backendKey, issueId) != nullptr) {
            return VoidOk(); // skip the issue-type scan below
        }
    }
    const std::string issueTypeKey = (issueTypeKeyOverride && !issueTypeKeyOverride->empty())
                                         ? *issueTypeKeyOverride
                                         : ResolveIssueTypeKeyForIssue(issueId);
    return EnsureIssueEditMetaLoadedFor(backend, backendKey, issueId, issueTypeKey, configSnapshot);
}

VoidResult EditMetaCacheService::EnsureIssueEditMetaLoadedFor(const std::shared_ptr<ITrackerBackend>& backend,
                                                              const std::string& backendKey, const std::string& issueId,
                                                              const std::string& issueTypeKey,
                                                              const TrackerConfig* configSnapshot) {
    return LoadIssueEditMeta(backend, backendKey, issueId, issueTypeKey, configSnapshot, LoadMode::ScopeSuffices);
}

bool EditMetaCacheService::AnsweredFromCache(const std::string& backendKey, const std::string& issueId,
                                             const std::string* project, const std::string& issueTypeKey,
                                             bool variesPerIssue, LoadMode mode) const {
    if (mode == LoadMode::Refresh) {
        return false; // a refresh always asks the tracker; the entry it replaces keeps answering meanwhile
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    if (FindLoaded(issueEditMeta_, backendKey, issueId) != nullptr) {
        return true;
    }
    // A scope entry fetched this session stands in for the issue (the field-edit path), and for a backend whose
    // answer does not vary per issue even on the focused-row warm. A restored one does not: it keeps answering
    // CanEdit* offline, but a reachable tracker is asked.
    const IssueEditMetaCache* scopeMeta =
        project != nullptr ? FindScopeLoaded(scopeEditMeta_, backendKey, *project, issueTypeKey) : nullptr;
    const bool scopeStandsIn = mode == LoadMode::ScopeSuffices || (mode == LoadMode::IssueOwn && !variesPerIssue);
    return scopeMeta != nullptr && scopeMeta->live && scopeStandsIn;
}

bool EditMetaCacheService::TryBeginIssueFetchLocked(const std::string& key, LoadMode mode, FetchTicket& out) {
    if (mode != LoadMode::Refresh && issueFetchInFlight_.find(key) != issueFetchInFlight_.end()) {
        return false;
    }
    out.Key = key;
    out.Gen = ++nextFetchGen_;
    issueFetchInFlight_[key] = out.Gen; // a Refresh replaces the running fetch's generation: that fetch is superseded
    return true;
}

bool EditMetaCacheService::IssueFetchCurrentLocked(const FetchTicket& ticket) const {
    const auto it = issueFetchInFlight_.find(ticket.Key);
    return it != issueFetchInFlight_.end() && it->second == ticket.Gen;
}

void EditMetaCacheService::ReleaseIssueFetch(const FetchTicket& ticket) {
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    const auto it = issueFetchInFlight_.find(ticket.Key);
    if (it != issueFetchInFlight_.end() && it->second == ticket.Gen) {
        issueFetchInFlight_.erase(it);
    }
}

EditMetaCacheService::FetchResult EditMetaCacheService::FetchEditMeta(ITrackerFieldCatalog* catalog,
                                                                      const TrackerConfig& cfg,
                                                                      const std::string& issueId) {
    FetchResult result;
    if (catalog != nullptr) {
        auto metaResult = catalog->FetchIssueEditMeta(cfg, issueId);
        result.Ok = static_cast<bool>(metaResult);
        if (result.Ok) {
            result.Cache.fieldCanEdit = std::move(metaResult.value());
        } else {
            result.Error = metaResult.error().Detail;
        }
    }
    // Only mark loaded after a successful fetch; on failure an empty map with loaded=true made
    // CanEditFieldForIssue deny every field (missing keys) instead of staying optimistic offline.
    result.Cache.loaded = result.Ok;
    result.Cache.live = result.Ok;
    if (!result.Ok) {
        result.Cache.retryAfter =
            std::chrono::steady_clock::now() + std::chrono::seconds(smatchet::offline::kLookupRetryAfterSeconds);
    }
    return result;
}

bool EditMetaCacheService::StoreFetchResult(const FetchTicket& ticket, const std::string& backendKey,
                                            const std::string& issueId, const std::string* project,
                                            const std::string& issueTypeKey, bool variesPerIssue,
                                            const FetchResult& fetched, bool promote, bool& scopeUnchanged) {
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    if (!IssueFetchCurrentLocked(ticket)) {
        // A newer fetch (Refresh) or an Invalidate retired this one: its answer is not stored, so it can neither
        // overwrite the newer answer nor bring back an entry the caller just dropped.
        return true;
    }
    issueFetchInFlight_.erase(ticket.Key);
    if (!fetched.Ok) {
        // A failed fetch never replaces what is already loaded (Pillar 6); it only records the backoff.
        IssueEditMetaCache& slot = issueEditMeta_[backendKey][issueId];
        if (!slot.loaded) {
            slot = fetched.Cache;
        }
        return false;
    }
    // Where the answer cannot vary per issue the scope entry serves every issue, so none is kept per row (and an
    // older one, from before the scope was filled, goes: the read side prefers an own entry).
    if (promote && !variesPerIssue) {
        const auto backendIt = issueEditMeta_.find(backendKey);
        if (backendIt != issueEditMeta_.end()) {
            backendIt->second.erase(issueId);
        }
    } else {
        issueEditMeta_[backendKey][issueId] = fetched.Cache;
    }
    if (promote && project != nullptr) {
        IssueEditMetaCache& scopeEntry = scopeEditMeta_[backendKey][*project][issueTypeKey];
        // A live entry that already says the same needs no new save (arrowing through rows of one scope).
        scopeUnchanged = scopeEntry.loaded && scopeEntry.live && scopeEntry.fieldCanEdit == fetched.Cache.fieldCanEdit;
        scopeEntry = fetched.Cache;
    }
    return false;
}

VoidResult EditMetaCacheService::LoadIssueEditMeta(const std::shared_ptr<ITrackerBackend>& backend,
                                                   const std::string& backendKey, const std::string& issueId,
                                                   const std::string& issueTypeKey, const TrackerConfig* configSnapshot,
                                                   LoadMode mode, const FetchTicket* started, bool* refused) {
    if (refused != nullptr) {
        *refused = false;
    }
    FetchTicket ticket;
    bool haveTicket = false;
    if (started != nullptr) {
        ticket = *started;
        haveTicket = true;
    }
    // Releases the registration on every exit, a throw included; a no-op once superseded or already released.
    smatchet::ScopeExit releaseTicket([this, &ticket, &haveTicket]() {
        if (haveTicket) {
            ReleaseIssueFetch(ticket);
        }
    });
    if (!backend || issueId.empty()) {
        return VoidOk();
    }
    ITrackerFieldCatalog* const catalog = backend->FieldCatalog();
    const bool variesPerIssue = catalog != nullptr && catalog->IssueEditMetaVariesPerIssue(issueId);
    std::string project;
    const bool hasScope = ScopeProjectFor(issueId, issueTypeKey, variesPerIssue, project);
    if (AnsweredFromCache(backendKey, issueId, hasScope ? &project : nullptr, issueTypeKey, variesPerIssue, mode)) {
        return VoidOk();
    }

    if (smatchet::offline::IsOfflineState(deps_.TrackerConnectivity())) {
        return VoidResult::Err("Tracker is offline; edit permissions were not refreshed.");
    }
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        if (!haveTicket) {
            if (!TryBeginIssueFetchLocked(InFlightKey(backendKey, issueId), mode, ticket)) {
                // This issue is being fetched already: it stays as it is until that answer lands.
                if (refused != nullptr) {
                    *refused = true;
                }
                return VoidOk();
            }
            haveTicket = true;
        } else if (!IssueFetchCurrentLocked(ticket)) {
            // A Refresh or Invalidate retired the fetch this call was launched for: no request, its answer would
            // be dropped.
            if (refused != nullptr) {
                *refused = true;
            }
            return VoidOk();
        }
    }
    const TrackerConfig cfg = configSnapshot ? *configSnapshot : ConfigManager::Load();
    const FetchResult fetched = FetchEditMeta(catalog, cfg, issueId);
    const bool ok = fetched.Ok;
    // Only a representative answer (an editable field) speaks for the scope: one uneditable issue must not
    // narrow its siblings. The others keep their own entry.
    const bool promote =
        ok && hasScope && smatchet::editmeta::IsRepresentativeEditPermissions(fetched.Cache.fieldCanEdit);
    bool scopeUnchanged = false;
    const bool stale = StoreFetchResult(ticket, backendKey, issueId, hasScope ? &project : nullptr, issueTypeKey,
                                        variesPerIssue, fetched, promote, scopeUnchanged);
    const std::string& fetchError = fetched.Error;
    const IssueEditMetaCache& cache = fetched.Cache;

    if (!ok) {
        LOG_WARN("EditMetaCacheService: editmeta fetch failed issue=%s err=%s", issueId.c_str(), fetchError.c_str());
        return VoidResult::Err(fetchError);
    }
    if (stale) {
        return VoidOk(); // retired while in flight: not a failure, and nothing to store or announce
    }
    if (promote && !scopeUnchanged) {
        PostScopeSave(backendKey, project, issueTypeKey, cache.fieldCanEdit); // queued: no SQLite on this thread
    }
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return VoidOk();
}

VoidResult EditMetaCacheService::RefreshIssueEditMeta(const std::string& issueId,
                                                      const std::string* issueTypeKeyOverride) {
    const std::string issueTypeKey = (issueTypeKeyOverride && !issueTypeKeyOverride->empty())
                                         ? *issueTypeKeyOverride
                                         : ResolveIssueTypeKeyForIssue(issueId);
    return RefreshIssueEditMetaFor(deps_.BackendShared(), deps_.CacheBackendKey(), issueId, issueTypeKey);
}

VoidResult EditMetaCacheService::RefreshIssueEditMetaFor(const std::shared_ptr<ITrackerBackend>& backend,
                                                         const std::string& backendKey, const std::string& issueId,
                                                         const std::string& issueTypeKey) {
    // Nothing is dropped up front: the issue's own entry and its scope keep answering while this fetch runs, a
    // failed fetch leaves them as they are, and a successful one replaces them. Beginning it supersedes any
    // fetch of this issue still running.
    return LoadIssueEditMeta(backend, backendKey, issueId, issueTypeKey, nullptr, LoadMode::Refresh);
}

void EditMetaCacheService::InvalidateIssueEditMeta(const std::string& issueId) {
    InvalidateIssueEditMetaFor(deps_.CacheBackendKey(), issueId);
}

void EditMetaCacheService::InvalidateIssueEditMetaFor(const std::string& backendKey, const std::string& issueId) {
    if (issueId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    const auto backendIt = issueEditMeta_.find(backendKey);
    if (backendIt != issueEditMeta_.end()) {
        backendIt->second.erase(issueId);
    }
    // A fetch still running for this issue started before the change that made the caller invalidate: its
    // answer is stale and is dropped when it returns.
    issueFetchInFlight_.erase(InFlightKey(backendKey, issueId));
}

void EditMetaCacheService::PruneEditMetaCacheToActiveTickets() {
    // Union across EVERY live pane, not just the calling one: this cache is process-wide, so a
    // prune scoped to one pane's roster evicts the entries another pane's open editor is using and
    // forces a fresh editmeta round-trip on its next keystroke (multi-pane scoping audit).
    const std::vector<std::shared_ptr<const std::vector<CachedTicket>>> snaps =
        deps_.GetActiveTicketsSnapshotsAllPanes();
    std::unordered_set<std::string> keep;
    std::unordered_set<std::string> keepScopes;
    for (std::size_t s = 0; s < snaps.size(); ++s) {
        if (!snaps[s]) {
            continue;
        }
        const std::vector<CachedTicket>& tickets = *snaps[s];
        keep.reserve(keep.size() + tickets.size());
        keepScopes.reserve(keepScopes.size() + tickets.size());
        for (const auto& t : tickets) {
            if (!t.id.empty()) {
                keep.insert(t.id);
            }
            const std::string typeKey = IssueTypeKeyOfTicket(t);
            if (!typeKey.empty()) {
                keepScopes.insert(
                    smatchet::editmeta::BuildScopeStoreKey(smatchet::editmeta::ProjectOfIssue(t.id), typeKey));
            }
        }
    }

    // The union holds bare ids (the snapshots do not carry their pane's backend namespace), so an id
    // is kept in every namespace where it is active in some pane: a prune may keep a little extra,
    // never drop an entry an open editor uses.
    std::lock_guard<std::mutex> lock(editMetaMutex_);
    for (auto& byBackend : issueEditMeta_) {
        EditMetaById& byId = byBackend.second;
        for (auto it = byId.begin(); it != byId.end();) {
            it = keep.find(it->first) == keep.end() ? byId.erase(it) : std::next(it);
        }
    }
    for (auto& byBackend : scopeEditMeta_) {
        for (auto& byProject : byBackend.second) {
            for (auto& entry : byProject.second) {
                // Scope entries are never erased: saved rows load once per backend, and a live fetch replaced
                // the restored entry in place, so erasing it would lose the scope's saved permissions for the
                // rest of the session (they still answer CanEdit* offline). A live entry for a scope no pane
                // shows is demoted instead, so it is refetched when next needed online. The map is bounded by
                // the tracker's projects and issue types.
                if (entry.second.live && keepScopes.find(smatchet::editmeta::BuildScopeStoreKey(
                                             byProject.first, entry.first)) == keepScopes.end()) {
                    entry.second.live = false;
                }
            }
        }
    }
}

void EditMetaCacheService::WarmIssueEditMetaAsync(const std::string& issueId) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    if (!backend || issueId.empty()) {
        return;
    }
    if (smatchet::offline::IsOfflineState(deps_.TrackerConnectivity())) {
        return; // Pillar 6: no fetch while offline; the issue stays optimistic
    }
    const std::string backendKey = deps_.CacheBackendKey();
    const std::string inFlightKey = InFlightKey(backendKey, issueId);
    FetchTicket ticket;
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        const auto backendIt = issueEditMeta_.find(backendKey);
        if (backendIt != issueEditMeta_.end()) {
            const auto it = backendIt->second.find(issueId);
            if (it != backendIt->second.end() && it->second.loaded) {
                return;
            }
            if (it != backendIt->second.end() && std::chrono::steady_clock::now() < it->second.retryAfter) {
                return; // failed recently — back off instead of refetching every frame
            }
        }
    }

    // Resolved now, on the UI thread, with the backend and namespace above: the worker loads the
    // pane that kicked it even if focus moves before it runs.
    const std::string issueTypeKey = ResolveIssueTypeKeyForIssue(issueId);
    const ITrackerFieldCatalog* const catalog = backend->FieldCatalog();
    const bool variesPerIssue = catalog != nullptr && catalog->IssueEditMetaVariesPerIssue(issueId);
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        // A backend whose answer does not vary per issue needs no row fetch once its scope has a live answer.
        std::string project;
        if (!variesPerIssue && ScopeProjectFor(issueId, issueTypeKey, variesPerIssue, project)) {
            const IssueEditMetaCache* scopeMeta = FindScopeLoaded(scopeEditMeta_, backendKey, project, issueTypeKey);
            if (scopeMeta != nullptr && scopeMeta->live) {
                return;
            }
        }
        if (!TryBeginIssueFetchLocked(inFlightKey, LoadMode::IssueOwn, ticket)) {
            return;
        }
    }
    // From here the registration is released exactly once, whether the task runs (LoadIssueEditMeta releases it),
    // is dropped unrun (shutdown), or the launch throws: a later frame can always try again.
    std::shared_ptr<smatchet::LaunchClaim> claim;
    try {
        claim = std::make_shared<smatchet::LaunchClaim>([this, ticket]() { ReleaseIssueFetch(ticket); });
        const TrackerConfig warmupTrackerCfg = ConfigManager::Load();
        deps_.LaunchBackgroundTask(
            [this, backend, backendKey, issueId, issueTypeKey, ticket, warmupTrackerCfg, claim]() {
                if (!claim->TakeForRun()) {
                    return;
                }
                // Best-effort async warmup: ignore fetch failure (issue stays optimistic) — discard VoidResult.
                LoadIssueEditMeta(backend, backendKey, issueId, issueTypeKey, &warmupTrackerCfg, LoadMode::IssueOwn,
                                  &ticket);
            });
    } catch (const std::exception& ex) {
        LOG_WARN("EditMetaCacheService: editmeta warmup for %s did not start: %s", issueId.c_str(), ex.what());
        if (claim) {
            claim->ReleaseIfUntaken();
        } else {
            ReleaseIssueFetch(ticket);
        }
    }
}

void EditMetaCacheService::OnConnectivityRecovered() {
    {
        std::lock_guard<std::mutex> lock(editMetaMutex_);
        for (auto& byBackend : issueEditMeta_) {
            for (auto& kv : byBackend.second) {
                kv.second.retryAfter = std::chrono::steady_clock::time_point();
            }
        }
    }
    savedTypesLoad_.ClearBackoff();
}
