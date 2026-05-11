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
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "LocalCacheManager.h"
#include "StringUtil.h"
#include "Views.h"

namespace {
std::string SanitizeOfflineQueueDetail(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
        if (c == '=') {
            c = ':';
        }
    }
    return s;
}

std::string FormatOfflineQueueTerminalLine(const char* action, const char* stage, const char* reason,
                                           std::string detail) {
    detail = SanitizeOfflineQueueDetail(std::move(detail));
    std::string out;
    out.reserve(64u + detail.size());
    out += "action=";
    out += action;
    out += " stage=";
    out += stage;
    out += " reason=";
    out += reason;
    out += " detail=";
    out += detail;
    return out;
}

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
    const std::vector<std::string>& inheritIds = (cfg.TrackerType == "Plane") ? cfg.NewIssueInheritFieldIdsPlane : cfg.NewIssueInheritFieldIds;
    return IssueDraftHelpers::FromCachedTicket(lastTicket, AvailableFields, cfg.ProjectKey, cfg.DefaultIssueTypeId,
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

    ITrackerClient* backend = Backend.get();
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

    LaunchBackgroundTask([this, promise, backend, cache, draftCopy, catalogCopy, required]() {
        IssueCreateResult result = IssueCreatePipeline::Run(*backend, cache, draftCopy, required, *catalogCopy);
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
    return offlineQueue_ ? offlineQueue_->DeleteDeadPendingCreates(deadIds)
                         : AppController::DeadLetterDeleteSummary{};
}

AppController::PendingQueueDeleteSummary
AppController::DeletePendingCreates(const std::vector<std::int64_t>& pendingIds) {
    return offlineQueue_ ? offlineQueue_->DeletePendingCreates(pendingIds)
                         : AppController::PendingQueueDeleteSummary{};
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
    }
}

AppController::PendingFieldEditDeleteSummary
AppController::DeletePendingFieldEdits(const std::vector<std::int64_t>& ids) {
    return offlineQueue_ ? offlineQueue_->DeletePendingFieldEdits(ids)
                         : AppController::PendingFieldEditDeleteSummary{};
}

AppController::DeadFieldEditDeleteSummary
AppController::DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds) {
    return offlineQueue_ ? offlineQueue_->DeleteDeadPendingFieldEdits(deadIds)
                         : AppController::DeadFieldEditDeleteSummary{};
}

void AppController::TickOfflineFieldEdits() {
    if (ConfigManager::Load().ReadOnlyMode) {
        return;
    }
    if (!Cache || !Backend) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        if (offlineFieldEditReplayInFlight_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextOfflineFieldEditReplayAt_) {
            return;
        }
        offlineFieldEditReplayInFlight_ = true;
    }

    std::vector<PendingFieldEditRecord> pending;
    try {
        pending = Cache->LoadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::TickOfflineFieldEdits load failed: %s", ex.what());
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    } catch (...) {
        LOG_ERROR("AppController::TickOfflineFieldEdits load failed: unknown exception");
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    }
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    }

    LocalCacheManager* cache = Cache.get();
    ITrackerClient* backend = Backend.get();
    if (!backend) {
        return;
    }

    LaunchBackgroundTask([this, pending, cache, backend]() {
        int successes = 0;
        int failures = 0;
        int archived = 0;
        int cacheOpFailures = 0;
        bool ranUpdate = false;

        const auto tryCacheMutation = [&](const char* action, std::int64_t id,
                                          const std::function<void()>& fn) -> bool {
            try {
                fn();
                return true;
            } catch (const std::exception& ex) {
                ++cacheOpFailures;
                LOG_ERROR("AppController::TickOfflineFieldEdits %s failed id=%lld err=%s", action,
                          static_cast<long long>(id), ex.what());
                return false;
            } catch (...) {
                ++cacheOpFailures;
                LOG_ERROR("AppController::TickOfflineFieldEdits %s failed id=%lld err=unknown exception", action,
                          static_cast<long long>(id));
                return false;
            }
        };

        const int kMaxReplayAttempts = OfflineFieldEditQueue::kMaxReplayAttempts;
        for (const auto& row : pending) {
            if (row.HasMergeConflict) {
                // User must resolve the merge conflict via the offline queue UI before replay.
                continue;
            }
            if (row.Attempts >= kMaxReplayAttempts) {
                char detailBuf[384];
                std::snprintf(detailBuf, sizeof(detailBuf),
                              "Queued offline field edit id %lld already at attempts=%d (max=%d).",
                              static_cast<long long>(row.Id), row.Attempts, kMaxReplayAttempts);
                const std::string terminal = FormatOfflineQueueTerminalLine("offline_field_replay", "max_attempt_gate",
                                                                            "max_attempts", detailBuf);
                if (tryCacheMutation("archive_pending_field_edit", row.Id,
                                     [&]() { cache->ArchivePendingFieldEdit(row.Id, "max_attempts", terminal); })) {
                    ++archived;
                } else {
                    ++failures;
                }
                continue;
            }

            nlohmann::json fieldsPayload;
            try {
                fieldsPayload = nlohmann::json::parse(row.FieldsPayloadJson);
            } catch (const std::exception& ex) {
                const std::string terminal = FormatOfflineQueueTerminalLine("offline_field_replay", "parse_payload",
                                                                            "malformed_json", ex.what());
                if (tryCacheMutation("archive_pending_field_edit_malformed", row.Id,
                                     [&]() { cache->ArchivePendingFieldEdit(row.Id, "malformed_json", terminal); })) {
                    ++archived;
                } else {
                    ++failures;
                }
                continue;
            }

            ranUpdate = true;

            // 3-way merge: if the edit carries an original rich value (base), fetch the current
            // server document (theirs) and attempt to merge before replaying. This prevents
            // clobbering concurrent edits made while the device was offline.
            // Applies only to ADF/HTML description-class fields (OriginalRichValue non-empty).
            bool skipMergeConflict = false;
            if (!row.OriginalRichValue.empty() && fieldsPayload.is_object()) {
                // Identify the field key and its current raw payload inside fieldsPayload.
                // The queued payload is {fieldId: <adf-or-html>}; extract the inner value.
                const std::string& fid = row.FieldId;
                // Bug-fix: Plane's BuildFieldPayload puts the description value under
                // "description_html", not "description". Fall back to the _html key.
                std::string payloadKey = fid;
                if (!fieldsPayload.contains(fid)) {
                    const std::string altKey = fid + "_html";
                    if (fieldsPayload.contains(altKey)) payloadKey = altKey;
                }
                if (fieldsPayload.contains(payloadKey)) {
                    // Detect format from OriginalRichValue (base) once — used for mine extraction
                    // and for rebuilding the merged payload.
                    size_t biDetect = 0;
                    const std::string& brDetect = row.OriginalRichValue;
                    while (biDetect < brDetect.size() &&
                           (brDetect[biDetect]==' '||brDetect[biDetect]=='\t'||
                            brDetect[biDetect]=='\n'||brDetect[biDetect]=='\r')) ++biDetect;
                    const bool isAdf = biDetect < brDetect.size() && brDetect[biDetect] == '{';

                    auto toMd = [](const std::string& rich) -> std::string {
                        if (rich.empty()) return rich;
                        size_t i = 0;
                        while (i < rich.size() && (rich[i]==' '||rich[i]=='\t'||rich[i]=='\n'||rich[i]=='\r')) ++i;
                        if (i < rich.size() && rich[i] == '{') {
                            try {
                                auto j = nlohmann::json::parse(rich);
                                if (j.is_object() && j.value("type",std::string())=="doc")
                                    return MarkdownConvert::AdfToMarkdown(j);
                            } catch (...) {}
                        }
                        if (i < rich.size() && rich[i] == '<') {
                            bool fell = false;
                            return MarkdownConvert::HtmlSubsetToMarkdown(rich, &fell);
                        }
                        return rich;
                    };

                    try {
                        // --- Fetch current server state ---
                        const TrackerConfig cfgForFetch = ConfigManager::Load();
                        const ViewsStore viewsForFetch = ConfigManager::LoadViewsOrBootstrap(cfgForFetch);
                        std::vector<CachedTicket> freshTickets;
                        std::string fetchErr;
                        const std::vector<std::string> keysForFetch = {row.IssueKey};
                        if (backend->FetchIssuesForKeys(cfgForFetch, keysForFetch, viewsForFetch,
                                                         freshTickets, fetchErr) &&
                            !freshTickets.empty()) {

                            const CachedTicket& fresh = freshTickets.front();
                            const std::string theirsRich = fresh.GetFieldRichValue(fid);

                            if (!theirsRich.empty() && theirsRich != row.OriginalRichValue) {
                                const std::string baseMd   = toMd(row.OriginalRichValue);
                                const std::string theirsMd = toMd(theirsRich);

                                // Mine: extract from the queued payload and convert to Markdown.
                                // ADF objects are walked directly; HTML strings are converted.
                                std::string mineMd;
                                const auto& myVal = fieldsPayload[payloadKey];
                                if (myVal.is_object() && myVal.value("type",std::string())=="doc") {
                                    mineMd = MarkdownConvert::AdfToMarkdown(myVal);
                                } else if (myVal.is_string()) {
                                    // Plane stores HTML — convert to Markdown for merge surface.
                                    bool fell = false;
                                    mineMd = MarkdownConvert::HtmlSubsetToMarkdown(
                                        myVal.get<std::string>(), &fell);
                                    if (fell) mineMd = toMd(myVal.get<std::string>());
                                }

                                const TextMerge::MergeResult merged = TextMerge::ThreeWayMerge(baseMd, mineMd, theirsMd);
                                if (merged.IsClean) {
                                    // Rebuild payload under the correct key for this backend.
                                    if (isAdf) {
                                        fieldsPayload[payloadKey] = MarkdownConvert::MarkdownToAdf(merged.Text);
                                    } else {
                                        fieldsPayload[payloadKey] = MarkdownConvert::MarkdownToHtml(merged.Text);
                                    }
                                    LOG_INFO("TickOfflineFieldEdits: 3-way merge clean for issue=%s field=%s",
                                             row.IssueKey.c_str(), fid.c_str());
                                } else {
                                    // True conflict — mark the record and skip the network update.
                                    // The offline queue UI (SmatchetOfflineQueueUi) will show a
                                    // "Resolve Conflict" button that opens the PR-F modal.
                                    LOG_WARN("TickOfflineFieldEdits: 3-way merge conflict for issue=%s field=%s — "
                                             "suspending replay pending user resolution",
                                             row.IssueKey.c_str(), fid.c_str());
                                    const nlohmann::json ctx = {
                                        {"base",    baseMd},
                                        {"mine",    mineMd},
                                        {"theirs",  theirsMd},
                                        {"fieldId", fid},
                                        {"richKind", isAdf ? "adf" : "html"}
                                    };
                                    tryCacheMutation("mark_field_edit_conflict", row.Id,
                                        [&]() { cache->MarkFieldEditConflict(row.Id, ctx.dump()); });
                                    // Skip the UpdateIssueFields call; user must resolve first.
                                    skipMergeConflict = true;
                                }
                            }
                        }
                    } catch (const std::exception& ex) {
                        LOG_WARN("TickOfflineFieldEdits: 3-way merge fetch/merge failed issue=%s field=%s err=%s — "
                                 "replaying original edit as-is",
                                 row.IssueKey.c_str(), fid.c_str(), ex.what());
                    } catch (...) {
                        LOG_WARN("TickOfflineFieldEdits: 3-way merge failed (unknown) issue=%s field=%s — "
                                 "replaying original edit as-is",
                                 row.IssueKey.c_str(), fid.c_str());
                    }
                }
            }

            if (skipMergeConflict) {
                ++failures; // Count as pending — will retry after user resolves.
                continue;
            }
            std::string err;
            if (!backend->UpdateIssueFields(row.IssueKey, fieldsPayload, err)) {
                if (IsTrackerTransportErrorText(err)) {
                    const int nextAttempts = row.Attempts + 1;
                    if (nextAttempts >= kMaxReplayAttempts) {
                        const std::string terminal =
                            FormatOfflineQueueTerminalLine("offline_field_replay", "transport_cap", "max_attempts",
                                                           "Transport failures exhausted replay attempts: " + err);
                        if (tryCacheMutation("archive_pending_field_edit_transport_cap", row.Id, [&]() {
                                cache->UpdatePendingFieldEdit(row.Id, nextAttempts, err);
                                cache->ArchivePendingFieldEdit(row.Id, "transport_cap", terminal);
                            })) {
                            ++archived;
                        } else {
                            ++failures;
                        }
                    } else {
                        (void)tryCacheMutation("update_pending_field_edit", row.Id,
                                               [&]() { cache->UpdatePendingFieldEdit(row.Id, nextAttempts, err); });
                        ++failures;
                    }
                } else {
                    const std::string terminal =
                        FormatOfflineQueueTerminalLine("offline_field_replay", "replay_rejected", "jira_error", err);
                    if (tryCacheMutation("archive_pending_field_edit_rejected", row.Id, [&]() {
                            cache->ArchivePendingFieldEdit(row.Id, "replay_rejected", terminal);
                        })) {
                        ++archived;
                    } else {
                        ++failures;
                    }
                }
                continue;
            }

            if (tryCacheMutation("delete_pending_field_edit", row.Id,
                                 [&]() { cache->DeletePendingFieldEdit(row.Id); })) {
                ++successes;
                requestDeferredLiveTrackerBackendSuccessNotify_();
                BackendAuditTrail::AppendResult(
                    "offline_replay_field_edit", "offline_field_replay", row.IssueKey, std::to_string(row.Id), true,
                    std::string(), nlohmann::json{{"pending_field_edit_id", row.Id}, {"field_id", row.FieldId}});
            } else {
                ++failures;
            }
        }

        if (successes > 0) {
            RefreshLocalData();
        }
        if (successes > 0 || failures > 0 || archived > 0 || cacheOpFailures > 0) {
            LOG_INFO("AppController: offline field edit replay finished successes=%d failures=%d archived=%d "
                     "cache_op_failures=%d",
                     successes, failures, archived, cacheOpFailures);
        }

        std::chrono::seconds delay{5};
        if (!ranUpdate && !pending.empty()) {
            delay = std::chrono::seconds(300);
        } else if (failures > 0 && successes == 0) {
            delay = std::chrono::seconds(30);
        }
        const auto nextAt = std::chrono::steady_clock::now() + delay;
        {
            std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
            nextOfflineFieldEditReplayAt_ = nextAt;
            offlineFieldEditReplayInFlight_ = false;
        }
    });
}

void AppController::TickOfflineCreates() {
    if (ConfigManager::Load().ReadOnlyMode) {
        return;
    }
    if (!Cache || !Backend) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        if (offlineReplayInFlight_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextOfflineReplayAt_) {
            return;
        }
        offlineReplayInFlight_ = true;
    }

    std::vector<PendingCreate> pending;
    try {
        pending = Cache->LoadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("AppController::TickOfflineCreates load failed: %s", ex.what());
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    } catch (...) {
        LOG_ERROR("AppController::TickOfflineCreates load failed: unknown exception");
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    }
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    }

    // Defer heavy work to a worker. Grab snapshots the worker needs.
    auto catalogCopy = std::make_shared<std::vector<TrackerField>>(AvailableFields);
    ITrackerClient* backend = Backend.get();
    LocalCacheManager* cache = Cache.get();
    // Hard cap attempts per row so a permanently-bad payload doesn't loop forever.

    LaunchBackgroundTask([this, pending, backend, cache, catalogCopy]() {
        const int kMaxReplayAttempts = OfflineCreateQueue::kMaxReplayAttempts;
        int successes = 0;
        int failures = 0;
        int archived = 0;
        int cacheOpFailures = 0;
        bool ranCreate = false;
        const auto tryCacheMutation = [&](const char* action, std::int64_t id,
                                          const std::function<void()>& fn) -> bool {
            try {
                fn();
                return true;
            } catch (const std::exception& ex) {
                ++cacheOpFailures;
                LOG_ERROR("AppController::TickOfflineCreates %s failed id=%lld err=%s", action,
                          static_cast<long long>(id), ex.what());
                return false;
            } catch (...) {
                ++cacheOpFailures;
                LOG_ERROR("AppController::TickOfflineCreates %s failed id=%lld err=unknown exception", action,
                          static_cast<long long>(id));
                return false;
            }
        };
        const auto archivePending = [&](const PendingCreate& pc, const std::string& reason,
                                        const std::string& terminalError) -> bool {
            return tryCacheMutation("archive_pending_create", pc.Id,
                                    [&]() { cache->ArchivePendingCreate(pc.Id, reason, terminalError); });
        };
        for (const auto& pc : pending) {
            if (pc.Attempts >= kMaxReplayAttempts) {
                char detailBuf[384];
                std::snprintf(detailBuf, sizeof(detailBuf),
                              "Queued offline create id %lld already at attempts=%d (max=%d). "
                              "Offline replay did not call Jira create; entry moved to Failed offline creates.",
                              static_cast<long long>(pc.Id), pc.Attempts, kMaxReplayAttempts);
                const std::string terminal =
                    FormatOfflineQueueTerminalLine("offline_replay", "max_attempt_gate", "max_attempts", detailBuf);
                if (archivePending(pc, "max_attempts", terminal)) {
                    ++archived;
                    BackendAuditTrail::AppendResult(
                        "offline_dead_letter", "offline_replay", std::string(), std::to_string(pc.Id), true,
                        std::string(), nlohmann::json{{"pending_create_id", pc.Id}, {"reason", "max_attempts"}});
                } else {
                    ++failures;
                }
                continue;
            }
            IssueDraft draft;
            std::string parseErr;
            if (!IssueDraftHelpers::FromJson(pc.Payload, draft, parseErr)) {
                LOG_WARN("AppController: archiving malformed pending_create id=%lld err=%s",
                         static_cast<long long>(pc.Id), parseErr.c_str());
                const std::string terminal =
                    FormatOfflineQueueTerminalLine("offline_replay", "parse_payload", "malformed_payload",
                                                   std::string("IssueDraft JSON parse failed: ") + parseErr);
                if (archivePending(pc, "malformed_payload", terminal)) {
                    ++archived;
                    BackendAuditTrail::AppendResult("offline_dead_letter", "offline_replay", std::string(),
                                                    std::to_string(pc.Id), true, std::string(),
                                                    nlohmann::json{{"pending_create_id", pc.Id},
                                                                   {"reason", "malformed_payload"},
                                                                   {"error", parseErr}});
                } else {
                    ++failures;
                }
                continue;
            }
            const RequiredFieldSet required =
                GetRequiredFieldSet(draft.ProjectKey, draft.IssueTypeId, draft.IssueTypeName);
            ranCreate = true;
            IssueCreateResult result = IssueCreatePipeline::Run(*backend, cache, draft, required, *catalogCopy);
            if (result.Ok) {
                if (tryCacheMutation("delete_pending_create", pc.Id, [&]() { cache->DeletePendingCreate(pc.Id); })) {
                    ++successes;
                    requestDeferredLiveTrackerBackendSuccessNotify_();
                    BackendAuditTrail::AppendResult(
                        "offline_replay_create", "offline_replay", result.IssueKey, std::to_string(pc.Id), true,
                        std::string(), nlohmann::json{{"pending_create_id", pc.Id}, {"attempts_before", pc.Attempts}});
                } else {
                    ++failures;
                }
            } else {
                const int nextAttempts = pc.Attempts + 1;
                if (nextAttempts >= kMaxReplayAttempts) {
                    std::string trackerPart =
                        result.Error.empty()
                            ? std::string("Create pipeline returned failure with empty error on final attempt.")
                            : std::string("Create pipeline error: ") + result.Error;
                    char headBuf[224];
                    std::snprintf(headBuf, sizeof(headBuf), "Offline replay attempts went from %d to %d (cap=%d). ",
                                  pc.Attempts, nextAttempts, kMaxReplayAttempts);
                    const std::string terminalError = FormatOfflineQueueTerminalLine(
                        "offline_replay", "issue_create", "max_attempts", std::string(headBuf) + trackerPart);
                    const bool archivedOk = tryCacheMutation("archive_pending_create", pc.Id, [&]() {
                        cache->UpdatePendingCreate(pc.Id, nextAttempts, terminalError);
                        cache->ArchivePendingCreate(pc.Id, "max_attempts", terminalError);
                    });
                    if (archivedOk) {
                        ++archived;
                        BackendAuditTrail::AppendResult("offline_dead_letter", "offline_replay", std::string(),
                                                        std::to_string(pc.Id), true, std::string(),
                                                        nlohmann::json{{"pending_create_id", pc.Id},
                                                                       {"reason", "max_attempts"},
                                                                       {"error", result.Error}});
                    } else {
                        ++failures;
                    }
                } else {
                    const bool updateOk = tryCacheMutation("update_pending_create", pc.Id, [&]() {
                        cache->UpdatePendingCreate(pc.Id, nextAttempts, result.Error);
                    });
                    (void)updateOk;
                    BackendAuditTrail::AppendResult("offline_replay_create", "offline_replay", std::string(),
                                                    std::to_string(pc.Id), false, result.Error,
                                                    nlohmann::json{{"pending_create_id", pc.Id},
                                                                   {"attempts_before", pc.Attempts},
                                                                   {"attempts_after", nextAttempts}});
                    ++failures;
                }
            }
        }
        if (successes > 0) {
            RefreshLocalData();
        }
        if (successes > 0 || failures > 0 || archived > 0 || cacheOpFailures > 0) {
            LOG_INFO("AppController: offline replay finished successes=%d failures=%d archived=%d cache_op_failures=%d",
                     successes, failures, archived, cacheOpFailures);
        }
        // Back off if all failed; otherwise try again soon. Pending rows only skipped (max
        // attempts) still tick every few seconds — use a long delay when no create ran.
        std::chrono::seconds delay{5};
        if (!ranCreate && !pending.empty()) {
            delay = std::chrono::seconds(300);
        } else if (failures > 0 && successes == 0) {
            delay = std::chrono::seconds(30);
        }
        const auto nextAt = std::chrono::steady_clock::now() + delay;
        {
            std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
            nextOfflineReplayAt_ = nextAt;
            offlineReplayInFlight_ = false;
        }
    });
}





