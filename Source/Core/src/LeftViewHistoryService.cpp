#include "LeftViewHistoryService.h"

#include "CacheBackendKeyPure.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "Sync/LeftViewReasonPure.h"
#include "Sync/RefusalIsolatingFetchPure.h"
#include "Sync/RemoteVersionPure.h"
#include "Sync/ViewListingDiffPure.h"

#include <algorithm>
#include <exception>
#include <map>
#include <unordered_map>
#include <utility>

namespace smatchet {

namespace {

constexpr const char* kViewListingKind = "view_listing";
constexpr const char* kPendingReasonText = "Finding the reason...";

// Summary and link from a row's fields, whichever the backend named them.
std::string SummaryOf(const std::unordered_map<std::string, std::string>& fields) {
    const auto it = fields.find("summary");
    return it != fields.end() ? it->second : std::string();
}

std::string UrlFieldOf(const std::unordered_map<std::string, std::string>& fields) {
    const auto it = fields.find("url");
    return it != fields.end() ? it->second : std::string();
}

// How far back an explanation may look: the row's own stamp, or when the view last listed it. 0 keeps all.
std::int64_t SinceMsFor(const std::unordered_map<std::string, std::string>& fields, std::int64_t listedAtUnixSec) {
    const auto updated = fields.find("updated");
    std::int64_t ms = 0;
    if (updated != fields.end() && sync::ParseRemoteUpdatedMs(updated->second, ms)) {
        return ms;
    }
    return listedAtUnixSec * 1000;
}

SavedViewListing LoadSavedListing(LocalCacheManager& cache, const std::string& backendKey, const std::string& viewId) {
    LookupCacheRow row;
    if (!cache.TryGetLookup(backendKey, kViewListingKind, viewId, row)) {
        return SavedViewListing();
    }
    return ViewListingFromJson(row.PayloadJson, row.UpdatedAtEpochSec);
}

void ApplyReason(LeftViewRecord& row, const LeftViewReason& reason) {
    row.reasonKind = reason.kind;
    row.reason = reason.text;
    row.author = reason.author;
    row.reasonDetailJson = ReasonDetailToJson(reason.changes);
    if (reason.kind == LeftViewReasonKind::Deleted) {
        row.kind = LeftViewKind::Deleted;
    }
}

LeftViewRecord BuildRow(const LeftViewCapture& capture, const LeftViewDeparture& departure, std::int64_t nowUnixSec,
                        std::int64_t listedAtUnixSec) {
    LeftViewRecord row;
    row.backendKey = capture.backendKey;
    row.paneId = capture.paneId;
    row.viewId = capture.viewId;
    row.viewName = capture.viewName;
    row.query = capture.query;
    row.issueKey = departure.issueKey;
    row.summary = SummaryOf(departure.fieldValues);
    row.lastKnownJson = LastKnownFieldsToJson(departure.fieldValues);
    row.browseUrl = UrlFieldOf(departure.fieldValues);
    if (row.browseUrl.empty() && !departure.deleted && capture.reader) {
        row.browseUrl = capture.reader->BuildBrowseUrl(capture.cfg, departure.issueKey);
    }
    row.leftAtUnixSec = nowUnixSec;
    row.listedAtUnixSec = listedAtUnixSec;
    row.reasonKind = LeftViewReasonKind::Pending;
    row.reason = kPendingReasonText;
    if (departure.deleted) {
        ApplyReason(row, DeletedReason());
    }
    return row;
}

} // namespace

void SettleInterruptedLookup(LeftViewRecord& row) {
    if (row.reasonKind == LeftViewReasonKind::Pending) {
        row.reasonKind = LeftViewReasonKind::NotChecked;
        row.reason = "Reason not checked: the app closed before the lookup finished.";
    }
}

LeftViewHistoryService::LeftViewHistoryService(Deps deps) : deps_(std::move(deps)), io_(deps_.Launch) {}

void LeftViewHistoryService::Load() {
    const std::uint64_t generation = generation_;
    io_.Post([this, generation]() {
        /* PILLAR2_WORKER_ONLY */ // est-latency: one indexed SELECT - off the UI thread
        std::vector<LeftViewRecord> loaded;
        const std::shared_ptr<LocalCacheManager> cache = deps_.GetCache();
        if (cache) {
            loaded = cache->LoadLeftViewRows(deps_.MaxRows());
        }
        for (LeftViewRecord& row : loaded) {
            SettleInterruptedLookup(row);
        }
        deps_.PostToUi([this, generation, loaded = std::move(loaded)]() mutable {
            if (generation != generation_) {
                return;
            }
            // Rows committed before this read landed are in `loaded` too; rows newer than it stay on top.
            std::unordered_map<std::int64_t, bool> have;
            for (const LeftViewRecord& row : rows_) {
                have[row.id] = true;
            }
            for (LeftViewRecord& row : loaded) {
                if (have.find(row.id) == have.end()) {
                    rows_.push_back(std::move(row));
                }
            }
            std::stable_sort(rows_.begin(), rows_.end(), [](const LeftViewRecord& a, const LeftViewRecord& b) {
                return a.leftAtUnixSec != b.leftAtUnixSec ? a.leftAtUnixSec > b.leftAtUnixSec : a.id > b.id;
            });
            ++revision_;
        });
    });
}

void LeftViewHistoryService::ResetForCacheRecreate() {
    ++generation_;
    rows_.clear();
    details_ = LeftViewDetails();
    ++detailsRequestSeq_;
    ++revision_;
    Load();
}

void LeftViewHistoryService::RecordListing(const LeftViewCapture& capture, std::vector<std::string> listed,
                                           std::unordered_set<std::string> stillHeld,
                                           std::vector<LeftViewDeparture> lastKnown) {
    if (capture.viewId.empty() || listed.empty()) {
        return; // nothing to key the listing on, or the suspect empty answer the sync's wipe guard also ignores
    }
    const std::shared_ptr<const LeftViewCapture> shared = std::make_shared<const LeftViewCapture>(capture);
    const std::uint64_t generation = generation_;
    RetryUnavailable_(*shared);
    io_.Post([this, shared, listed = std::move(listed), stillHeld = std::move(stillHeld),
              lastKnown = std::move(lastKnown), generation]() {
        /* PILLAR2_WORKER_ONLY */ // est-latency: one lookup read plus one transaction - off the UI thread
        const std::shared_ptr<LocalCacheManager> cache = deps_.GetCache();
        if (!cache) {
            return;
        }
        const SavedViewListing saved = LoadSavedListing(*cache, shared->backendKey, shared->viewId);
        const std::vector<std::string> departed = DepartedFromListing(saved, listed, shared->query, stillHeld);
        std::unordered_map<std::string, const LeftViewDeparture*> rowsByKey;
        for (const LeftViewDeparture& row : lastKnown) {
            rowsByKey[row.issueKey] = &row;
        }
        const std::int64_t now = deps_.NowUnixSec();
        const std::size_t maxLookups = static_cast<std::size_t>((std::max)(0, deps_.MaxReasonKeys()));
        std::vector<LeftViewRecord> rows;
        std::vector<LookupJob> jobs;
        for (const std::string& key : departed) {
            LeftViewDeparture departure;
            departure.issueKey = key;
            const auto known = rowsByKey.find(key);
            if (known != rowsByKey.end()) {
                departure = *known->second;
            }
            LeftViewRecord row = BuildRow(*shared, departure, now, saved.savedAtUnixSec);
            LookupJob job;
            job.issueKey = key;
            job.needsExistenceProbe = true; // a reload never probed: a missing issue may be deleted or just unreadable
            job.lastKnown = departure.fieldValues;
            job.sinceMs = SinceMsFor(departure.fieldValues, saved.savedAtUnixSec);
            if (jobs.size() >= maxLookups) {
                ApplyReason(row, NotCheckedReason());
            } else {
                jobs.push_back(std::move(job));
            }
            rows.push_back(std::move(row));
        }
        CommitDepartures_(shared, std::move(rows), std::move(jobs), ViewListingToJson(shared->query, listed), now,
                          generation, /*notifyAsReload=*/true);
    });
}

void LeftViewHistoryService::RecordMonitorDepartures(const LeftViewCapture& capture,
                                                     std::vector<LeftViewDeparture> departures) {
    if (capture.viewId.empty() || departures.empty()) {
        return;
    }
    const std::shared_ptr<const LeftViewCapture> shared = std::make_shared<const LeftViewCapture>(capture);
    const std::uint64_t generation = generation_;
    io_.Post([this, shared, departures = std::move(departures), generation]() {
        /* PILLAR2_WORKER_ONLY */ // est-latency: one lookup read plus one transaction - off the UI thread
        const std::shared_ptr<LocalCacheManager> cache = deps_.GetCache();
        if (!cache) {
            return;
        }
        const SavedViewListing saved = LoadSavedListing(*cache, shared->backendKey, shared->viewId);
        if (!saved.known || !SameViewQuery(saved.query, shared->query)) {
            return; // not what the view's last reload answered, so these rows cannot be shown to have left it
        }
        std::vector<std::string> keys;
        keys.reserve(departures.size());
        for (const LeftViewDeparture& departure : departures) {
            keys.push_back(departure.issueKey);
        }
        const std::vector<std::string> stillListed = KeysStillListed(saved, keys);
        if (stillListed.empty()) {
            return; // another pane on this view already recorded them
        }
        const std::unordered_set<std::string> toRecord(stillListed.begin(), stillListed.end());
        const std::int64_t now = deps_.NowUnixSec();
        const std::size_t maxLookups = static_cast<std::size_t>((std::max)(0, deps_.MaxReasonKeys()));
        std::vector<LeftViewRecord> rows;
        std::vector<LookupJob> jobs;
        for (const LeftViewDeparture& departure : departures) {
            if (toRecord.find(departure.issueKey) == toRecord.end()) {
                continue;
            }
            LeftViewRecord row = BuildRow(*shared, departure, now, saved.savedAtUnixSec);
            if (!departure.deleted) {
                if (jobs.size() >= maxLookups) {
                    ApplyReason(row, NotCheckedReason());
                } else {
                    LookupJob job;
                    job.issueKey = departure.issueKey;
                    job.lastKnown = departure.fieldValues;
                    job.sinceMs = SinceMsFor(departure.fieldValues, saved.savedAtUnixSec);
                    jobs.push_back(std::move(job));
                }
            }
            rows.push_back(std::move(row));
        }
        CommitDepartures_(shared, std::move(rows), std::move(jobs),
                          ViewListingToJson(saved.query, ListingWithout(saved, stillListed)), saved.savedAtUnixSec,
                          generation,
                          /*notifyAsReload=*/false);
    });
}

void LeftViewHistoryService::RecordListingAdditions(const std::string& backendKey, const std::string& viewId,
                                                    const std::string& query, std::vector<std::string> keys) {
    if (viewId.empty() || keys.empty()) {
        return;
    }
    io_.Post([this, backendKey, viewId, query, keys = std::move(keys)]() {
        /* PILLAR2_WORKER_ONLY */ // est-latency: one lookup read plus one transaction - off the UI thread
        const std::shared_ptr<LocalCacheManager> cache = deps_.GetCache();
        if (!cache) {
            return;
        }
        const SavedViewListing saved = LoadSavedListing(*cache, backendKey, viewId);
        if (!saved.known || !SameViewQuery(saved.query, query)) {
            return; // the next complete listing is a baseline anyway
        }
        std::vector<std::string> ids = ListingWithout(saved, keys); // already-listed keys must not appear twice
        ids.insert(ids.end(), keys.begin(), keys.end());
        std::vector<LeftViewRecord> noRows;
        cache->CommitLeftViewDepartures(noRows, deps_.MaxRows(), backendKey, viewId,
                                        ViewListingToJson(saved.query, ids), saved.savedAtUnixSec);
    });
}

void LeftViewHistoryService::CommitDepartures_(const std::shared_ptr<const LeftViewCapture>& capture,
                                               std::vector<LeftViewRecord> rows, std::vector<LookupJob> jobs,
                                               const std::string& listingJson, std::int64_t listingSavedAtUnixSec,
                                               std::uint64_t generation, bool notifyAsReload) {
    const std::shared_ptr<LocalCacheManager> cache = deps_.GetCache();
    if (!cache || !cache->CommitLeftViewDepartures(rows, deps_.MaxRows(), capture->backendKey, capture->viewId,
                                                   listingJson, listingSavedAtUnixSec)) {
        return; // the listing did not move, so the next reload finds the same departures again
    }
    if (rows.empty()) {
        return;
    }
    std::unordered_map<std::string, std::int64_t> idByKey;
    for (const LeftViewRecord& row : rows) {
        idByKey[row.issueKey] = row.id;
    }
    for (LookupJob& job : jobs) {
        job.id = idByKey[job.issueKey];
    }
    deps_.PostToUi([this, rows = std::move(rows), generation, notifyAsReload]() mutable {
        if (generation != generation_) {
            return;
        }
        if (notifyAsReload && deps_.OnReloadDepartures) {
            deps_.OnReloadDepartures(rows);
        }
        rows_.insert(rows_.begin(), rows.begin(), rows.end());
        const std::size_t maxRows = static_cast<std::size_t>((std::max)(1, deps_.MaxRows()));
        if (rows_.size() > maxRows) {
            rows_.resize(maxRows);
        }
        ++revision_;
    });
    LaunchLookups_(capture, std::move(jobs), generation);
}

void LeftViewHistoryService::LaunchLookups_(const std::shared_ptr<const LeftViewCapture>& capture,
                                            std::vector<LookupJob> jobs, std::uint64_t generation) {
    if (jobs.empty()) {
        return;
    }
    // A separate worker: tracker calls must not stall the cache queue, and must not hold the cache handle.
    const std::shared_ptr<const std::vector<LookupJob>> shared =
        std::make_shared<const std::vector<LookupJob>>(std::move(jobs));
    try {
        deps_.Launch([this, capture, shared, generation]() {
            /* PILLAR2_WORKER_ONLY */ // est-latency: one to two batched tracker reads - off the UI thread
            try {
                RunLookups_(capture, *shared, generation);
            } catch (const std::exception& ex) {
                LOG_WARN("LeftViewHistoryService: reason lookup failed: %s", ex.what());
                SettleUnavailable_(*shared, generation);
            }
        });
    } catch (const std::exception& ex) {
        LOG_WARN("LeftViewHistoryService: could not start the reason lookup: %s", ex.what());
        SettleUnavailable_(*shared, generation);
    }
}

void LeftViewHistoryService::RetryUnavailable_(const LeftViewCapture& capture) {
    // A reason that could not be read is not final (Pillar 6): a completed reload proves this site answers again,
    // so ask once more. Bounded like any batch; rows the tracker answered for (NotVisible) are not asked again.
    if (!capture.reader || deps_.IsOffline()) {
        return;
    }
    const std::size_t maxLookups = static_cast<std::size_t>((std::max)(0, deps_.MaxReasonKeys()));
    std::map<std::string, std::vector<LookupJob>> jobsByQuery; // a lookup ranks changes against its row's query
    std::size_t count = 0;
    for (LeftViewRecord& row : rows_) {
        if (count >= maxLookups) {
            break;
        }
        if (row.reasonKind != LeftViewReasonKind::Unavailable || row.backendKey != capture.backendKey) {
            continue;
        }
        LookupJob job;
        job.id = row.id;
        job.issueKey = row.issueKey;
        job.needsExistenceProbe = true;
        job.lastKnown = LastKnownFieldsFromJson(row.lastKnownJson);
        job.sinceMs = SinceMsFor(job.lastKnown, row.listedAtUnixSec);
        row.reasonKind = LeftViewReasonKind::Pending; // in memory only: the stored row stays retryable
        row.reason = kPendingReasonText;
        jobsByQuery[row.query].push_back(std::move(job));
        ++count;
    }
    if (count == 0) {
        return;
    }
    ++revision_;
    for (auto& entry : jobsByQuery) {
        std::shared_ptr<LeftViewCapture> forQuery = std::make_shared<LeftViewCapture>(capture);
        forQuery->query = entry.first;
        LaunchLookups_(forQuery, std::move(entry.second), generation_);
    }
}

void LeftViewHistoryService::RunLookups_(const std::shared_ptr<const LeftViewCapture>& capture,
                                         const std::vector<LookupJob>& jobs, std::uint64_t generation) {
    // No request while offline (Pillar 6): the retry window would only delay the same answer.
    if (!capture->reader || deps_.IsOffline()) {
        SettleUnavailable_(jobs, generation);
        return;
    }
    const std::function<bool()> cancelled = deps_.ShouldCancel ? deps_.ShouldCancel : []() { return false; };
    std::vector<std::string> keys;
    keys.reserve(jobs.size());
    for (const LookupJob& job : jobs) {
        keys.push_back(job.issueKey);
    }

    // History first: it is dated and attributed. Any failure falls through to comparing rows. A batch naming a
    // deleted or hidden key is refused whole, so the refusals are isolated and the other keys keep their history.
    std::unordered_map<std::string, std::vector<TrackerFieldChange>> historyByKey;
    int historyBudget = sync::RefusalSplitBudget(keys.size());
    std::vector<std::string> historyRefused;
    auto history = sync::FetchItemsIsolatingRefusals<TrackerFieldChange>(
        [&](const std::vector<std::string>& batch) {
            return capture->reader->FetchIssueFieldChanges(capture->cfg, batch, cancelled);
        },
        cancelled, keys, historyBudget, historyRefused);
    if (history.has_value()) {
        for (TrackerFieldChange& change : history.value()) {
            historyByKey[change.issueKey].push_back(std::move(change));
        }
    }

    std::vector<std::string> needRow;
    std::unordered_map<std::string, std::vector<TrackerFieldChange>> changesByKey;
    for (const LookupJob& job : jobs) {
        const auto found = historyByKey.find(job.issueKey);
        std::vector<TrackerFieldChange> recent;
        if (found != historyByKey.end()) {
            recent = ChangesSince(found->second, job.sinceMs);
        }
        if (recent.empty()) {
            needRow.push_back(job.issueKey);
        } else {
            changesByKey[job.issueKey] = std::move(recent);
        }
    }

    std::unordered_map<std::string, CachedTicket> freshByKey;
    bool rowsAnswered = needRow.empty();
    if (!needRow.empty() && !cancelled()) {
        const ViewsStore views = deps_.LoadViews(capture->cfg);
        int budget = sync::RefusalSplitBudget(needRow.size());
        std::vector<std::string> refused;
        auto bodies = sync::FetchKeysIsolatingRefusals(
            [&](const std::vector<std::string>& batch) {
                return capture->reader->FetchIssuesForKeysCancellable(capture->cfg, batch, views, cancelled);
            },
            cancelled, needRow, budget, refused);
        if (bodies.has_value()) {
            rowsAnswered = true;
            for (CachedTicket& ticket : bodies.value()) {
                freshByKey[ticket.id] = std::move(ticket);
            }
        }
    }

    for (const LookupJob& job : jobs) {
        LeftViewRecord update;
        update.id = job.id;
        update.issueKey = job.issueKey;
        update.kind = LeftViewKind::LeftView;
        const auto fresh = freshByKey.find(job.issueKey);
        LeftViewReason reason;
        const auto changes = changesByKey.find(job.issueKey);
        if (changes != changesByKey.end()) {
            reason = BuildLeftViewReason(changes->second, capture->query, capture->fieldLabels);
        } else if (fresh != freshByKey.end()) {
            reason = BuildLeftViewReason(DiffRowFields(job.issueKey, job.lastKnown, fresh->second.fieldValues),
                                         capture->query, capture->fieldLabels);
            update.summary = SummaryOf(fresh->second.fieldValues);
            update.browseUrl = UrlFieldOf(fresh->second.fieldValues);
        } else if (rowsAnswered) {
            // The tracker answered without this issue. Only a reload still needs to ask whether it is gone: the
            // monitor's probe already answered that.
            reason = NotVisibleReason();
            if (job.needsExistenceProbe && !cancelled()) {
                auto exists = capture->reader->ProbeIssueExists(capture->cfg, job.issueKey);
                if (exists.has_value() && !exists.value()) {
                    reason = DeletedReason();
                }
            }
        } else {
            reason = UnavailableReason();
        }
        ApplyReason(update, reason);
        deps_.PostToUi([this, update = std::move(update), generation]() mutable {
            ApplyLookupResult_(std::move(update), generation);
        });
    }
}

void LeftViewHistoryService::SettleUnavailable_(const std::vector<LookupJob>& jobs, std::uint64_t generation) {
    for (const LookupJob& job : jobs) {
        LeftViewRecord update;
        update.id = job.id;
        update.issueKey = job.issueKey;
        ApplyReason(update, UnavailableReason());
        deps_.PostToUi([this, update = std::move(update), generation]() mutable {
            // A throw part-way through the lookups must not overwrite reasons they already found.
            const auto it = std::find_if(rows_.begin(), rows_.end(),
                                         [&update](const LeftViewRecord& row) { return row.id == update.id; });
            if (it != rows_.end() && it->reasonKind == LeftViewReasonKind::Pending) {
                ApplyLookupResult_(std::move(update), generation);
            }
        });
    }
}

void LeftViewHistoryService::ApplyLookupResult_(LeftViewRecord updated, std::uint64_t generation) {
    if (generation != generation_) {
        return;
    }
    const auto it = std::find_if(rows_.begin(), rows_.end(),
                                 [&updated](const LeftViewRecord& row) { return row.id == updated.id; });
    if (it == rows_.end()) {
        return; // dismissed while the lookup ran
    }
    it->kind = updated.kind;
    it->reasonKind = updated.reasonKind;
    it->reason = updated.reason;
    it->reasonDetailJson = updated.reasonDetailJson;
    it->author = updated.author;
    if (it->summary.empty()) {
        it->summary = updated.summary;
    }
    if (it->browseUrl.empty()) {
        it->browseUrl = updated.browseUrl;
    }
    if (it->kind == LeftViewKind::Deleted) {
        it->browseUrl.clear();
    }
    ++revision_;
    const LeftViewRecord toStore = *it;
    io_.Post([this, toStore]() {
        /* PILLAR2_WORKER_ONLY */ // est-latency: one single-row UPDATE - off the UI thread
        const std::shared_ptr<LocalCacheManager> cache = deps_.GetCache();
        if (cache) {
            cache->UpdateLeftViewReason(toStore);
        }
    });
}

void LeftViewHistoryService::Dismiss(std::int64_t id) {
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [id](const LeftViewRecord& row) { return row.id == id; }),
                rows_.end());
    if (details_.recordId == id) {
        CloseDetails();
    }
    ++revision_;
    io_.Post([this, id]() {
        /* PILLAR2_WORKER_ONLY */ // est-latency: one single-row DELETE - off the UI thread
        const std::shared_ptr<LocalCacheManager> cache = deps_.GetCache();
        if (cache) {
            cache->DeleteLeftViewRow(id);
        }
    });
}

void LeftViewHistoryService::ClearAll() {
    rows_.clear();
    CloseDetails();
    ++revision_;
    io_.Post([this]() {
        /* PILLAR2_WORKER_ONLY */ // est-latency: one bounded DELETE - off the UI thread
        const std::shared_ptr<LocalCacheManager> cache = deps_.GetCache();
        if (cache) {
            cache->ClearLeftViewRows();
        }
    });
}

void LeftViewHistoryService::RequestDetails(const LeftViewRecord& record, const TrackerConfig& uiCfg) {
    details_ = LeftViewDetails();
    details_.recordId = record.id;
    const std::uint64_t seq = ++detailsRequestSeq_;
    if (record.kind == LeftViewKind::Deleted) {
        details_.status = LeftViewDetails::Status::Idle; // nothing to read: the saved values are all there is
        return;
    }
    if (deps_.IsOffline()) {
        details_.status = LeftViewDetails::Status::Offline;
        return;
    }
    // The row's own tracker kind: the UI's config follows the focused pane, which may be on another tracker.
    TrackerConfig cfg = uiCfg;
    cfg.TrackerType = cache_keys::CacheBackendKeyKind(record.backendKey);
    std::shared_ptr<ITrackerIssueReader> reader;
    if (!deps_.ResolveReader(record.backendKey, cfg, reader) || !reader) {
        details_.status = LeftViewDetails::Status::NotConnected;
        return;
    }
    const std::string key = record.issueKey;
    details_.status = LeftViewDetails::Status::Loading;
    try {
        deps_.Launch([this, reader, cfg, key, seq]() {
            /* PILLAR2_WORKER_ONLY */ // est-latency: one keyed tracker read - off the UI thread
            std::unordered_map<std::string, std::string> fields;
            std::string error;
            try {
                const ViewsStore views = deps_.LoadViews(cfg);
                auto rows = reader->FetchIssuesForKeysCancellable(cfg, std::vector<std::string>(1, key), views,
                                                                  deps_.ShouldCancel);
                if (!rows.has_value()) {
                    error = rows.error().Detail;
                } else if (rows.value().empty()) {
                    error = "The tracker returned no issue for this key.";
                } else {
                    fields = rows.value().front().fieldValues;
                }
            } catch (const std::exception& ex) {
                error = ex.what();
            }
            deps_.PostToUi([this, seq, fields = std::move(fields), error = std::move(error)]() mutable {
                if (seq != detailsRequestSeq_) {
                    return; // the user moved on, or the cache was recreated
                }
                details_.status = error.empty() ? LeftViewDetails::Status::Live : LeftViewDetails::Status::Failed;
                details_.liveFields = std::move(fields);
                details_.error = std::move(error);
            });
        });
    } catch (const std::exception& ex) {
        // A launch that throws must not leave the view latched on Loading: Retry stays reachable.
        details_.status = LeftViewDetails::Status::Failed;
        details_.error = ex.what();
    }
}

void LeftViewHistoryService::CloseDetails() {
    details_ = LeftViewDetails();
    ++detailsRequestSeq_;
}

std::unordered_map<std::string, std::string>
LeftViewHistoryService::FieldLabelsFor(const std::string& backendKey) const {
    return deps_.ResolveFieldLabels ? deps_.ResolveFieldLabels(backendKey)
                                    : std::unordered_map<std::string, std::string>();
}

void LeftViewHistoryService::OpenUrl(const std::string& url) const {
    if (deps_.OpenUrl && !url.empty()) {
        deps_.OpenUrl(url);
    }
}

bool LeftViewHistoryService::Flush(std::chrono::milliseconds timeout) { return io_.Flush(timeout); }

} // namespace smatchet
