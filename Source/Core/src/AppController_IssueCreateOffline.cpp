#include "AppController.h"

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
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "LocalCacheManager.h"
#include "StringUtil.h"
#include "Views.h"

namespace {
// SanitizeOfflineQueueDetail + FormatOfflineQueueTerminalLine moved to OfflineQueueService.cpp
// alongside their only callers (TickOfflineCreates / TickOfflineFieldEdits) in Phase 1C of
// the item 12 extraction.

#if !defined(_WIN32)
std::string EscapeShellArg(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '"' || c == '\\' || c == '`' || c == '$') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}
#endif
} // namespace

// --- Create-issue helpers -------------------------------------------------

IssueDraft AppController::BuildDraftFromLastTicket(const TrackerConfig& cfg) const {
    const auto snap = GetActiveTicketsSnapshot();
    const auto& tickets = *snap;
    CachedTicket lastTicket;
    if (!tickets.empty()) {
        lastTicket = tickets.back();
    }
    const std::vector<std::string>& inheritIds =
        (cfg.TrackerType == "Plane") ? cfg.NewIssueInheritFieldIdsPlane : cfg.NewIssueInheritFieldIds;
    // PR 6: legacy global cfg.ProjectKey removed — pass "" as the legacy fallback.
    const std::string resolvedProject = smatchet::ResolveProjectForDraft(
        Backend ? &Backend->Connectivity() : nullptr, cfg.JqlQuery, lastTicket.id, /*legacyFallback*/ std::string());
    return IssueDraftHelpers::FromCachedTicket(lastTicket, AvailableFields, resolvedProject, cfg.DefaultIssueTypeId,
                                               cfg.DefaultIssueTypeName, inheritIds);
}

RequiredFieldSet AppController::GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                                    const std::string& issueTypeName) const {
    RequiredFieldSet result;
    for (const auto& entry : AvailableIssueTypeMeta) {
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
    const auto cfg = ConfigManager::Load();
    if (cfg.TrackerType == "Plane") {
        result.RequiresIssueType = false;
    }
    return result; // empty -> hard minimum only
}

std::future<IssueCreateResult> AppController::CreateIssueAsync(const IssueDraft& draft) {
    // Snapshot state the worker needs up front so we don't race with UI edits.
    auto catalogCopy = std::make_shared<std::vector<TrackerField>>(AvailableFields);
    const RequiredFieldSet required = GetRequiredFieldSet(draft.ProjectKey, draft.IssueTypeId, draft.IssueTypeName);
    std::shared_ptr<std::promise<IssueCreateResult>> promise = std::make_shared<std::promise<IssueCreateResult>>();
    std::future<IssueCreateResult> future = promise->get_future();

    if (ConfigManager::Load().ReadOnlyMode) {
        IssueCreateResult err;
        err.Error = "Read-only mode is enabled in Preferences.";
        promise->set_value(std::move(err));
        return future;
    }

    if (!Backend) {
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

    ITrackerIssueMutations* const mutations = Backend->Mutations();
    if (!mutations) {
        IssueCreateResult err;
        err.Error = "Tracker backend does not support issue mutations.";
        promise->set_value(std::move(err));
        return future;
    }

    LaunchBackgroundTask([this, promise, mutations, cache, draftCopy, catalogCopy, required]() {
        IssueCreateResult result = IssueCreatePipeline::Run(*mutations, cache, draftCopy, required, *catalogCopy);
        if (result.Ok) {
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
                                                  const std::string& originalRichValue) {
    if (!offlineQueue_) {
        outError = "Offline queue not initialized.";
        return 0;
    }
    return offlineQueue_->QueueFieldEditOffline(issueKey, fieldId, fieldsPayloadJson, outError, originalRichValue);
}

std::vector<PendingFieldEditRecord> AppController::GetPendingFieldEdits() const {
    return offlineQueue_ ? offlineQueue_->GetPendingFieldEdits() : std::vector<PendingFieldEditRecord>{};
}

std::vector<DeadPendingFieldEdit> AppController::GetDeadPendingFieldEdits() const {
    return offlineQueue_ ? offlineQueue_->GetDeadPendingFieldEdits() : std::vector<DeadPendingFieldEdit>{};
}

void AppController::ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedMarkdown,
                                             const std::string& richKind) {
    if (offlineQueue_) {
        offlineQueue_->ResolveFieldEditConflict(id, resolvedMarkdown, richKind);
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
