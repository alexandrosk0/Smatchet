#include "AppController.h"
#include "ITrackerIssueMutations.h" // fan-in Phase 2: AppController.h fwd-decls it now; this TU calls Mutations() methods.

#include "MarkdownConvert.h"
#include "OfflineQueueService.h"
#include "TextMerge.h"
#include "TrackerFieldPayload.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "JiraClient.h"
#include "ProjectResolver.h"
#include "Tracker/NewIssueInheritFields.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "LocalCacheManager.h"
#include "StringUtil.h"
#include "Views.h"

// SanitizeOfflineQueueDetail + FormatOfflineQueueTerminalLine moved to OfflineQueueService.cpp
// alongside their only callers (TickOfflineCreates / TickOfflineFieldEdits) in Phase 1C of
// the item 12 extraction.

// --- Create-issue helpers -------------------------------------------------

IssueDraft AppController::BuildDraftFromLastTicket(const TrackerConfig& cfg) const {
    const auto snap = GetActiveTicketsSnapshot();
    const auto& tickets = *snap;
    CachedTicket lastTicket;
    if (!tickets.empty()) {
        lastTicket = tickets.back();
    }
    const std::vector<std::string>& inheritIds = smatchet::tracker::NewIssueInheritFieldIdsFor(cfg);
    // No global cfg.ProjectKey exists — pass "" as the legacy fallback.
    const std::string resolvedProject = smatchet::ResolveProjectForDraft(
        focusedContext().Backend ? &focusedContext().Backend->Connectivity() : nullptr, cfg.JqlQuery, lastTicket.id,
        /*legacyFallback*/ std::string());
    // DR6: SetFieldCatalog mutates AvailableFields on a background worker; copy under the
    // guard (latched once) rather than passing the live vector by reference into a call that
    // outlives the read — an unguarded read risks a torn / reallocated-out-from-under UAF.
    const GridContextFieldCatalog& cat = fieldCatalog();
    std::vector<TrackerField> availableFieldsCopy;
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        availableFieldsCopy = cat.AvailableFields;
    }
    return IssueDraftHelpers::FromCachedTicket(lastTicket, availableFieldsCopy, resolvedProject, cfg.DefaultIssueTypeId,
                                               cfg.DefaultIssueTypeName, inheritIds);
}

RequiredFieldSet AppController::GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                                    const std::string& issueTypeName) const {
    RequiredFieldSet result;
    // DR6: guard the scan of AvailableIssueTypeMeta — SetFieldCatalog reassigns it on a
    // background worker. The loop body only reads entry fields and never re-locks the guard,
    // so holding it across the whole scan is deadlock-free.
    const GridContextFieldCatalog& cat = fieldCatalog();
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        for (const auto& entry : cat.AvailableIssueTypeMeta) {
            const bool projectMatch = entry.ProjectKey.empty() || projectKey.empty() || entry.ProjectKey == projectKey;
            if (!projectMatch) {
                continue;
            }
            const bool idMatch = !issueTypeId.empty() && entry.IssueTypeId == issueTypeId;
            const bool nameMatch = !issueTypeName.empty() && entry.IssueTypeName == issueTypeName;
            if (idMatch || nameMatch) {
                result.FieldIds = entry.RequiredFieldIds;
                result.IsSubtask = entry.IsSubtask;
                break;
            }
        }
    }
    const auto cfg = ConfigManager::Load();
    if (smatchet::tracker::IsPlaneBackendType(cfg.TrackerType)) {
        result.RequiresIssueType = false;
    }
    return result; // empty -> hard minimum only
}

std::future<IssueCreateResult> AppController::CreateIssueAsync(const IssueDraft& draft,
                                                               smatchet::ui::CancelToken cancel) {
    // Snapshot state the worker needs up front so we don't race with UI edits.
    // DR6: copy AvailableFields under its guard — SetFieldCatalog reassigns the vector on a
    // background worker, so an unguarded copy-construct can read a half-reassigned / reallocated
    // source.
    const GridContextFieldCatalog& cat = fieldCatalog();
    std::shared_ptr<std::vector<TrackerField>> catalogCopy;
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        catalogCopy = std::make_shared<std::vector<TrackerField>>(cat.AvailableFields);
    }
    const RequiredFieldSet required = GetRequiredFieldSet(draft.ProjectKey, draft.IssueTypeId, draft.IssueTypeName);
    std::shared_ptr<std::promise<IssueCreateResult>> promise = std::make_shared<std::promise<IssueCreateResult>>();
    std::future<IssueCreateResult> future = promise->get_future();

    if (ConfigManager::Load().ReadOnlyMode) {
        IssueCreateResult err;
        err.Error = "Read-only mode is enabled in Preferences.";
        promise->set_value(std::move(err));
        return future;
    }

    // Latch a strong handle for the worker: a live tracker swap (SetBackend) while the create
    // runs would otherwise free the backend under the `mutations` pointer below (ADR 0012).
    std::shared_ptr<ITrackerBackend> backend = std::atomic_load(&focusedContext().Backend);
    if (!backend) {
        IssueCreateResult err;
        err.Error = "Tracker backend is not initialized.";
        promise->set_value(std::move(err));
        return future;
    }

    LocalCacheManager* cache = Cache.get();
    IssueDraft draftCopy = draft;

    // Update path: prune fields whose draft value already matches cache so we don't
    // round-trip ADF descriptions (formatting loss), re-run no-op transitions for
    // status, or re-add issues to their current sprint.
    if (!draftCopy.ExistingIssueKey.empty()) {
        const auto snap = GetActiveTicketsSnapshot();
        if (snap) {
            const std::string key = draftCopy.ExistingIssueKey;
            auto tIt = std::find_if(snap->begin(), snap->end(), [&](const auto& t) { return t.id == key; });
            if (tIt != snap->end()) {
                IssueDraftHelpers::PruneUnchangedFields(draftCopy, *tIt);
            }
        }
    }

    ITrackerIssueMutations* const mutations = backend->Mutations();
    if (!mutations) {
        IssueCreateResult err;
        err.Error = "Tracker backend does not support issue mutations.";
        promise->set_value(std::move(err));
        return future;
    }

    // Snapshot the cache namespace on the UI thread — the worker must write the new ticket
    // under the backend that produced it, even if a tracker swap lands mid-create (Slice 1b).
    const std::string cacheBackendKey = focusedContext().CacheBackendKeyCopy();

    // `backend` is captured to keep the latched backend (and thus `mutations`) alive for the
    // duration of the worker, per ADR 0012.
    LaunchBackgroundTask(
        [this, promise, backend, mutations, cache, cacheBackendKey, draftCopy, catalogCopy, required, cancel]() {
            // Cooperative cancel (WS-A): if the owner abandoned this create (bulk-import
            // `.clear()` / window-close / shutdown) before the worker started, return a
            // benign cancelled result without the network round-trip — and crucially
            // without dereferencing `this` for the post-create refresh/hydration below.
            if (cancel.IsCancelled()) {
                IssueCreateResult cancelled;
                cancelled.Error = "Cancelled.";
                promise->set_value(std::move(cancelled));
                return;
            }
            IssueCreateResult result =
                IssueCreatePipeline::Run(*mutations, cache, cacheBackendKey, draftCopy, required, *catalogCopy);
            // Re-check after the (long) create before the expensive refresh + hydration:
            // a cancel that landed while the create was in flight skips touching `this`
            // again, so a signalled shutdown drains fast (the result is still reported).
            if (result.Ok && !cancel.IsCancelled()) {
                RefreshLocalData();
                requestDeferredLiveTrackerBackendSuccessNotify_();
                // Same hydration as the grid after Create: fetch server-truth fields and merge into SQLite.
                const std::string key = result.IssueKey;
                if (!key.empty()) {
                    PrefetchIssueTicketsForKeys({key}, true);
                }
            }
            promise->set_value(std::move(result));
        });
    return future;
}

std::int64_t AppController::QueueCreateOffline(const IssueDraft& draft) {
    return offlineQueue_ ? offlineQueue_->QueueCreateOffline(draft) : 0;
}

// GetPendingCreateCount / GetDeadPendingCreateCount / GetDeadPendingCreates / GetPendingCreates:
// moved to OfflineQueueService in Phase 1A of the item 12 extraction. Thin delegators below.

size_t AppController::GetPendingCreateCount() const {
    return offlineQueue_ ? offlineQueue_->GetPendingCreateCount() : 0;
}

size_t AppController::GetDeadPendingCreateCount() const {
    return offlineQueue_ ? offlineQueue_->GetDeadPendingCreateCount() : 0;
}

std::vector<DeadPendingCreate> AppController::GetDeadPendingCreates() const {
    return offlineQueue_ ? offlineQueue_->GetDeadPendingCreates() : std::vector<DeadPendingCreate>{};
}

std::vector<PendingCreate> AppController::GetPendingCreates() const {
    return offlineQueue_ ? offlineQueue_->GetPendingCreates() : std::vector<PendingCreate>{};
}

AppController::DeadLetterRestoreSummary
AppController::RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds) {
    return offlineQueue_ ? offlineQueue_->RestoreDeadPendingCreates(originalIds)
                         : AppController::DeadLetterRestoreSummary{};
}

std::string AppController::TakeLegacyPendingStartupBanner() {
    return offlineQueue_ ? offlineQueue_->TakeLegacyPendingStartupBanner() : std::string{};
}

AppController::DeadLetterDeleteSummary
AppController::DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds) {
    return offlineQueue_ ? offlineQueue_->DeleteDeadPendingCreates(deadIds) : AppController::DeadLetterDeleteSummary{};
}

AppController::PendingQueueDeleteSummary
AppController::DeletePendingCreates(const std::vector<std::int64_t>& pendingIds) {
    return offlineQueue_ ? offlineQueue_->DeletePendingCreates(pendingIds) : AppController::PendingQueueDeleteSummary{};
}

std::int64_t AppController::QueueFieldEditOffline(const std::string& issueKey, const std::string& fieldId,
                                                  const std::string& fieldsPayloadJson, std::string& outError,
                                                  const std::string& originalRichValue,
                                                  const std::string& originalValue, bool hasOriginalValue) {
    if (!offlineQueue_) {
        outError = "Offline queue not initialized.";
        return 0;
    }
    return offlineQueue_->QueueFieldEditOffline(issueKey, fieldId, fieldsPayloadJson, outError, originalRichValue,
                                                originalValue, hasOriginalValue);
}

std::vector<PendingFieldEditRecord> AppController::GetPendingFieldEdits() const {
    return offlineQueue_ ? offlineQueue_->GetPendingFieldEdits() : std::vector<PendingFieldEditRecord>{};
}

std::vector<DeadPendingFieldEdit> AppController::GetDeadPendingFieldEdits() const {
    return offlineQueue_ ? offlineQueue_->GetDeadPendingFieldEdits() : std::vector<DeadPendingFieldEdit>{};
}

void AppController::ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedValue,
                                             const std::string& richKind, const std::string& kind) {
    if (offlineQueue_) {
        offlineQueue_->ResolveFieldEditConflict(id, resolvedValue, richKind, kind);
        offlineQueue_->RestartReplayTimersNow(std::chrono::steady_clock::now());
    }
}

AppController::PendingFieldEditDeleteSummary
AppController::DeletePendingFieldEdits(const std::vector<std::int64_t>& ids) {
    return offlineQueue_ ? offlineQueue_->DeletePendingFieldEdits(ids) : AppController::PendingFieldEditDeleteSummary{};
}

AppController::DeadFieldEditDeleteSummary
AppController::DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds) {
    return offlineQueue_ ? offlineQueue_->DeleteDeadPendingFieldEdits(deadIds)
                         : AppController::DeadFieldEditDeleteSummary{};
}

AppController::DeadFieldEditRestoreSummary
AppController::RestoreDeadPendingFieldEdits(const std::vector<std::int64_t>& originalIds) {
    return offlineQueue_ ? offlineQueue_->RestoreDeadPendingFieldEdits(originalIds)
                         : AppController::DeadFieldEditRestoreSummary{};
}

// TickOfflineCreates / TickOfflineFieldEdits: moved to OfflineQueueService in Phase 1C of the
// item 12 extraction. The replay-timer state members + their mutex live in the service too.

void AppController::TickOfflineCreates() {
    if (offlineQueue_) {
        offlineQueue_->TickOfflineCreates();
    }
}

void AppController::TickOfflineFieldEdits() {
    if (offlineQueue_) {
        offlineQueue_->TickOfflineFieldEdits();
    }
}
