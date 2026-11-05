// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=companion TU of AppController that connects the left-view history service to pane contexts and needs the full class definition; the UI reaches the service through its own header and adds no includer; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"

#include "CacheBackendKeyPure.h"
#include "Config/TunableLimits.h"
#include "ConfigManager.h"
#include "GridLiveContext.h"
#include "ITrackerBackend.h"
#include "ITicketSyncDeps.h"
#include "LeftViewHistoryService.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "SmatchetTicketChangeNotifications.h"
#include "Sync/MembershipDiffPure.h"
#include "Sync/TicketChangeDiffPure.h"
#include "Sync/ViewListingDiffPure.h"
#include "TimeNowPure.h"
#include "TrackerFieldSchema.h"

#include <chrono>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::unordered_map<std::string, std::string> FieldLabelsOf(GridLiveContext& ctx) {
    std::shared_ptr<const std::vector<TrackerField>> fields;
    {
        std::lock_guard<std::mutex> lock(ctx.fieldCatalog.availableFieldsMutex_);
        fields = ctx.fieldCatalog.AvailableFields;
    }
    std::unordered_map<std::string, std::string> labels;
    if (fields) {
        labels.reserve(fields->size());
        for (const TrackerField& field : *fields) {
            labels[field.Id] = field.Name;
        }
    }
    return labels;
}

} // namespace

void AppController::initLeftViewHistory_() {
    if (leftViewHistory_) {
        return;
    }
    smatchet::LeftViewHistoryService::Deps deps;
    deps.GetCache = [this]() { return std::atomic_load(&Cache); };
    deps.Launch = [this](std::function<void()> task) { LaunchBackgroundTask(std::move(task)); };
    deps.PostToUi = [this](std::function<void()> task) {
        mainThreadDispatcher.PostCompletionToMainThread(std::move(task));
    };
    // A recreate joins every background task, so a tracker call in flight must stop waiting for it too.
    deps.ShouldCancel = [this]() { return shuttingDown_.load() || cacheRecreating_.load(); };
    deps.IsOffline = [this]() { return IsTrackerOffline(); };
    deps.MaxRows = []() { return smatchet::limits::Get().LeftViewHistoryMaxRows; };
    deps.MaxReasonKeys = []() { return smatchet::limits::Get().LeftViewReasonMaxKeys; };
    deps.NowUnixSec = []() { return static_cast<std::int64_t>(std::time(nullptr)); };
    deps.ResolveReader = [this](const std::string& backendKey, const TrackerConfig& cfg,
                                std::shared_ptr<ITrackerIssueReader>& out) {
        // Only a config that points at the row's site: another site could answer the same key with another issue.
        if (smatchet::cache_keys::TrackerCacheBackendKey(cfg) != backendKey) {
            return false;
        }
        for (auto& entry : gridContexts_) {
            if (entry.second && entry.second->CacheBackendKeyCopy() == backendKey) {
                std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&entry.second->Backend);
                if (backend) {
                    out = std::shared_ptr<ITrackerIssueReader>(backend, &backend->Reader());
                    return true;
                }
            }
        }
        return false;
    };
    deps.ResolveFieldLabels = [this](const std::string& backendKey) {
        for (auto& entry : gridContexts_) {
            if (entry.second && entry.second->CacheBackendKeyCopy() == backendKey) {
                return FieldLabelsOf(*entry.second);
            }
        }
        return std::unordered_map<std::string, std::string>();
    };
    deps.OnReloadDepartures = [this](const std::vector<smatchet::LeftViewRecord>& rows) { onReloadDepartures_(rows); };
    deps.LoadViews = [](const TrackerConfig& cfg) { return ConfigManager::LoadViewsOrBootstrap(cfg); };
    deps.OpenUrl = [this](const std::string& url) { OpenUrl(url); };
    leftViewHistory_.reset(new smatchet::LeftViewHistoryService(std::move(deps)));
    leftViewHistory_->Load();
}

void AppController::FlushLeftViewHistoryBeforeShutdown_() {
    // Once shuttingDown_ is set, LaunchBackgroundTask drops the queue's drain task, so a departure recorded just
    // before quit would never reach SQLite. Bounded like the other pre-shutdown flushes.
    if (leftViewHistory_ && !leftViewHistory_->Flush(std::chrono::seconds(5))) {
        LOG_WARN("AppController shutdown: a queued left-view history write did not finish within 5 s.");
    }
}

smatchet::LeftViewCapture AppController::makeLeftViewCapture_(GridLiveContext& ctx, const std::string& viewId,
                                                              const std::string& viewName, const std::string& query,
                                                              const TrackerConfig& cfg) {
    smatchet::LeftViewCapture capture;
    capture.backendKey = ctx.CacheBackendKeyCopy();
    capture.paneId = ctx.PaneId;
    capture.viewId = viewId;
    capture.viewName = viewName;
    capture.query = query;
    capture.cfg = cfg;
    capture.fieldLabels = FieldLabelsOf(ctx);
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&ctx.Backend); // latch: a live swap must not free it
    if (backend) {
        capture.reader = std::shared_ptr<ITrackerIssueReader>(backend, &backend->Reader());
    }
    return capture;
}

void AppController::recordMonitorDepartures_(GridLiveContext& ctx, const std::string& paneId,
                                             const std::vector<CachedTicket>& prev,
                                             const smatchet::MembershipReconcilePlan& plan,
                                             const TrackerConfig& probedCfg) {
    if (!leftViewHistory_ || plan.residentRemovals.empty() || !ctx.monitorState.ViewListedKnown ||
        !smatchet::SameViewQuery(probedCfg.JqlQuery, ctx.monitorState.ViewListedQuery)) {
        return;
    }
    std::unordered_map<std::string, const CachedTicket*> rowsById;
    rowsById.reserve(prev.size());
    for (const CachedTicket& row : prev) {
        rowsById[row.id] = &row;
    }
    std::vector<smatchet::LeftViewDeparture> departures;
    std::vector<std::string> keys;
    for (const smatchet::MembershipRemovalVerdict& verdict : plan.residentRemovals) {
        if (ctx.monitorState.ViewListed.find(verdict.issueKey) == ctx.monitorState.ViewListed.end()) {
            continue;
        }
        smatchet::LeftViewDeparture departure;
        departure.issueKey = verdict.issueKey;
        departure.deleted = !verdict.stillExists;
        const auto row = rowsById.find(verdict.issueKey);
        if (row != rowsById.end()) {
            departure.fieldValues = row->second->fieldValues;
        }
        departures.push_back(std::move(departure));
        keys.push_back(verdict.issueKey);
    }
    if (departures.empty()) {
        return;
    }
    ctx.monitorState.NoteViewRowsLeft(keys);
    // The cache row of a left issue stays (it still exists), so this pane must stop claiming it: the next local
    // refresh would bring it back, and with the listing no longer naming it the monitor would leave it there.
    const std::string backendKey = ctx.CacheBackendKeyCopy();
    DropPaneOwnedTicketIds(backendKey, paneId, keys);
    leftViewHistory_->RecordMonitorDepartures(makeLeftViewCapture_(ctx, ctx.monitorState.ViewListedViewId,
                                                                   ctx.monitorState.ViewListedViewName,
                                                                   ctx.monitorState.ViewListedQuery, probedCfg),
                                              std::move(departures));
}

void AppController::noteMonitorRowsAdded_(GridLiveContext& ctx, const std::vector<std::string>& addedKeys,
                                          const TrackerConfig& probedCfg) {
    if (leftViewHistory_ && !addedKeys.empty() && ctx.monitorState.ViewListedKnown &&
        smatchet::SameViewQuery(probedCfg.JqlQuery, ctx.monitorState.ViewListedQuery)) {
        leftViewHistory_->RecordListingAdditions(ctx.CacheBackendKeyCopy(), ctx.monitorState.ViewListedViewId,
                                                 ctx.monitorState.ViewListedQuery, addedKeys);
    }
}

void AppController::onViewListingCompleted_(GridLiveContext& ctx, ViewListingCompletion completion) {
    if (!leftViewHistory_ || completion.cfg == nullptr) {
        return;
    }
    leftViewNotifyEnabled_ = completion.cfg->TicketChangeMonitorEnabled;
    std::vector<smatchet::LeftViewDeparture> lastKnown;
    lastKnown.reserve(completion.droppedRows.size());
    for (CachedTicket& row : completion.droppedRows) {
        smatchet::LeftViewDeparture departure;
        departure.issueKey = row.id;
        departure.fieldValues = std::move(row.fieldValues);
        lastKnown.push_back(std::move(departure));
    }
    leftViewHistory_->RecordListing(
        makeLeftViewCapture_(ctx, completion.viewId, completion.viewName, completion.query, *completion.cfg),
        std::move(completion.listed), std::move(completion.stillHeld), std::move(lastKnown));
}

void AppController::onReloadDepartures_(const std::vector<smatchet::LeftViewRecord>& rows) {
    if (!leftViewNotifyEnabled_ || rows.empty()) {
        return;
    }
    std::vector<smatchet::TicketChangeSummary> changes;
    changes.reserve(rows.size());
    for (const smatchet::LeftViewRecord& row : rows) {
        smatchet::TicketChangeSummary summary;
        summary.kind = smatchet::TicketChangeKind::LeftView;
        summary.issueId = row.issueKey;
        changes.push_back(std::move(summary));
    }
    NotifyTicketChanges(changes, rows.front().paneId);
}
