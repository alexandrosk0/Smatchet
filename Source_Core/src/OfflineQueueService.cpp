#include "OfflineQueueService.h"

#include "AppController.h"
#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "ITrackerClient.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "MarkdownConvert.h"
#include "TextMerge.h"
#include "TrackerHttpUtils.h"
#include "Views.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace {
// Anonymous-namespace helpers (formerly in AppController_IssueCreateOffline.cpp). Used by the
// Tick* replay loops below to format dead-letter audit lines.
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
} // namespace

OfflineQueueService::OfflineQueueService(AppController& app) : app_(app) {}

std::size_t OfflineQueueService::GetPendingCreateCount() const {
    if (!app_.Cache) {
        return 0;
    }
    try {
        return app_.Cache->LoadPendingCreates().size();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetPendingCreateCount failed: %s", ex.what());
        return 0;
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetPendingCreateCount failed: unknown exception");
        return 0;
    }
}

std::size_t OfflineQueueService::GetDeadPendingCreateCount() const {
    if (!app_.Cache) {
        return 0;
    }
    try {
        return app_.Cache->GetDeadPendingCreateCount();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreateCount failed: %s", ex.what());
        return 0;
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreateCount failed: unknown exception");
        return 0;
    }
}

std::vector<PendingCreate> OfflineQueueService::GetPendingCreates() const {
    if (!app_.Cache) {
        return {};
    }
    try {
        return app_.Cache->LoadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetPendingCreates failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetPendingCreates failed: unknown exception");
        return {};
    }
}

std::vector<DeadPendingCreate> OfflineQueueService::GetDeadPendingCreates() const {
    if (!app_.Cache) {
        return {};
    }
    try {
        return app_.Cache->LoadDeadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreates failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreates failed: unknown exception");
        return {};
    }
}

std::string OfflineQueueService::TakeLegacyPendingStartupBanner() {
    return std::move(legacyPendingStartupBanner_);
}

// --- Phase 1B: write methods + remaining field-edit read accessors ----------------------

std::int64_t OfflineQueueService::QueueCreateOffline(const IssueDraft& draft) {
    if (ConfigManager::Load().ReadOnlyMode) {
        LOG_WARN("OfflineQueueService::QueueCreateOffline blocked by read-only mode.");
        return 0;
    }
    if (!app_.Cache) {
        LOG_WARN("OfflineQueueService::QueueCreateOffline skipped: cache not initialized.");
        return 0;
    }
    const std::string payload = IssueDraftHelpers::ToJson(draft);
    try {
        const std::int64_t id = app_.Cache->EnqueuePendingCreate(payload);
        LOG_INFO("OfflineQueueService: queued offline create id=%lld", static_cast<long long>(id));
        BackendAuditTrail::AppendResult(
            "offline_queue_create", "ui", std::string(), std::to_string(id), true, std::string(),
            nlohmann::json{{"pending_create_id", id}, {"draft", IssueDraftHelpers::ToJson(draft)}});
        return id;
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::QueueCreateOffline failed: %s", ex.what());
        BackendAuditTrail::AppendResult("offline_queue_create", "ui", std::string(),
                                        BackendAuditTrail::MakeOperationId("offline-queue"), false, ex.what(),
                                        nlohmann::json{{"draft", IssueDraftHelpers::ToJson(draft)}});
        return 0;
    }
}

AppController::DeadLetterRestoreSummary
OfflineQueueService::RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds) {
    AppController::DeadLetterRestoreSummary summary;
    if (!app_.Cache || originalIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : originalIds) {
        try {
            if (app_.Cache->RestoreDeadPendingCreate(id)) {
                ++summary.Restored;
                BackendAuditTrail::AppendResult("offline_dead_letter_restore", "ui", std::string(), std::to_string(id),
                                                true, std::string(),
                                                nlohmann::json{{"original_pending_create_id", id}});
            } else {
                ++summary.Failed;
                BackendAuditTrail::AppendResult("offline_dead_letter_restore", "ui", std::string(), std::to_string(id),
                                                false, "Row not found.",
                                                nlohmann::json{{"original_pending_create_id", id}});
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("OfflineQueueService::RestoreDeadPendingCreates id=%lld err=%s", static_cast<long long>(id),
                      ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_restore", "ui", std::string(), std::to_string(id),
                                            false, ex.what(), nlohmann::json{{"original_pending_create_id", id}});
        } catch (...) {
            LOG_ERROR("OfflineQueueService::RestoreDeadPendingCreates id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_restore", "ui", std::string(), std::to_string(id),
                                            false, "Unknown exception.",
                                            nlohmann::json{{"original_pending_create_id", id}});
        }
    }
    return summary;
}

AppController::DeadLetterDeleteSummary
OfflineQueueService::DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds) {
    AppController::DeadLetterDeleteSummary summary;
    if (!app_.Cache || deadIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : deadIds) {
        try {
            app_.Cache->DeleteDeadPendingCreate(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_dead_letter_delete", "ui", std::string(), std::to_string(id), true,
                                            std::string(), nlohmann::json{{"dead_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("OfflineQueueService::DeleteDeadPendingCreates dead_id=%lld err=%s", static_cast<long long>(id),
                      ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_delete", "ui", std::string(), std::to_string(id),
                                            false, ex.what(), nlohmann::json{{"dead_id", id}});
        } catch (...) {
            LOG_ERROR("OfflineQueueService::DeleteDeadPendingCreates dead_id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_delete", "ui", std::string(), std::to_string(id),
                                            false, "Unknown exception.", nlohmann::json{{"dead_id", id}});
        }
    }
    return summary;
}

AppController::PendingQueueDeleteSummary
OfflineQueueService::DeletePendingCreates(const std::vector<std::int64_t>& pendingIds) {
    AppController::PendingQueueDeleteSummary summary;
    if (!app_.Cache || pendingIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : pendingIds) {
        try {
            app_.Cache->DeletePendingCreate(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_queue_delete", "ui", std::string(), std::to_string(id), true,
                                            std::string(), nlohmann::json{{"pending_create_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("OfflineQueueService::DeletePendingCreates id=%lld err=%s", static_cast<long long>(id),
                      ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_delete", "ui", std::string(), std::to_string(id), false,
                                            ex.what(), nlohmann::json{{"pending_create_id", id}});
        } catch (...) {
            LOG_ERROR("OfflineQueueService::DeletePendingCreates id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_delete", "ui", std::string(), std::to_string(id), false,
                                            "Unknown exception.", nlohmann::json{{"pending_create_id", id}});
        }
    }
    return summary;
}

std::int64_t OfflineQueueService::QueueFieldEditOffline(const std::string& issueKey, const std::string& fieldId,
                                                       const std::string& fieldsPayloadJson, std::string& outError,
                                                       const std::string& originalRichValue) {
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("OfflineQueueService::QueueFieldEditOffline blocked by read-only mode issue=%s field=%s",
                 issueKey.c_str(), fieldId.c_str());
        return 0;
    }
    if (!app_.Cache) {
        outError = "Cache is not initialized.";
        return 0;
    }
    if (issueKey.empty() || fieldId.empty() || fieldsPayloadJson.empty()) {
        outError = "Invalid offline field edit enqueue parameters.";
        return 0;
    }
    try {
        const std::int64_t id =
            app_.Cache->EnqueuePendingFieldEdit(issueKey, fieldId, fieldsPayloadJson, originalRichValue);
        LOG_INFO("OfflineQueueService: queued offline field edit id=%lld issue=%s field=%s",
                 static_cast<long long>(id), issueKey.c_str(), fieldId.c_str());
        BackendAuditTrail::AppendResult("offline_queue_field_edit", "ui", issueKey, std::to_string(id), true,
                                        std::string(),
                                        nlohmann::json{{"pending_field_edit_id", id}, {"field_id", fieldId}});
        return id;
    } catch (const std::exception& ex) {
        outError = ex.what();
        LOG_ERROR("OfflineQueueService::QueueFieldEditOffline failed: %s", ex.what());
        BackendAuditTrail::AppendResult("offline_queue_field_edit", "ui", issueKey,
                                        BackendAuditTrail::MakeOperationId("offline-field-queue"), false, ex.what(),
                                        nlohmann::json{{"field_id", fieldId}});
        return 0;
    }
}

std::vector<PendingFieldEditRecord> OfflineQueueService::GetPendingFieldEdits() const {
    if (!app_.Cache) {
        return {};
    }
    try {
        return app_.Cache->LoadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetPendingFieldEdits failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetPendingFieldEdits failed: unknown exception");
        return {};
    }
}

std::vector<DeadPendingFieldEdit> OfflineQueueService::GetDeadPendingFieldEdits() const {
    if (!app_.Cache) {
        return {};
    }
    try {
        return app_.Cache->LoadDeadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingFieldEdits failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingFieldEdits failed: unknown exception");
        return {};
    }
}

void OfflineQueueService::ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedMarkdown,
                                                   const std::string& richKind) {
    if (!app_.Cache) return;
    try {
        // Rebuild the final backend payload from the resolved Markdown.
        std::string resolvedPayloadJson;
        if (richKind == "adf") {
            const nlohmann::json adfDoc = MarkdownConvert::MarkdownToAdf(resolvedMarkdown);
            // We don't know the field id here, but the existing payload key is preserved by
            // wrapping as-is. The replay will use the stored payload JSON directly.
            resolvedPayloadJson = nlohmann::json{{std::string("__resolved__"), adfDoc}}.dump();
        } else {
            resolvedPayloadJson = MarkdownConvert::MarkdownToHtml(resolvedMarkdown);
        }
        // Simple approach: store the resolved payload back — the replay will use it directly.
        // We parse the existing payload first to keep the correct field key.
        try {
            auto existing = app_.Cache->LoadPendingFieldEdits();
            const auto rowIt = std::find_if(existing.begin(), existing.end(),
                                            [&](const auto& row) { return row.Id == id; });
            if (rowIt != existing.end()) {
                const auto& row = *rowIt;
                nlohmann::json newPayload;
                try {
                    newPayload = nlohmann::json::parse(row.FieldsPayloadJson);
                } catch (...) {
                    newPayload = nlohmann::json::object();
                }
                // Determine the actual payload key: may be "description" (Jira)
                // or "description_html" (Plane). Use the key already present in the payload.
                std::string payloadKey = row.FieldId;
                if (!newPayload.contains(payloadKey)) {
                    const std::string altKey = row.FieldId + "_html";
                    if (newPayload.contains(altKey)) payloadKey = altKey;
                }
                if (richKind == "adf") {
                    newPayload[payloadKey] = MarkdownConvert::MarkdownToAdf(resolvedMarkdown);
                } else {
                    newPayload[payloadKey] = MarkdownConvert::MarkdownToHtml(resolvedMarkdown);
                }
                app_.Cache->ResolveFieldEditConflict(id, newPayload.dump());
                return;
            }
        } catch (...) {
        }
        app_.Cache->ResolveFieldEditConflict(id, resolvedPayloadJson);
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::ResolveFieldEditConflict id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
    }
}

AppController::PendingFieldEditDeleteSummary
OfflineQueueService::DeletePendingFieldEdits(const std::vector<std::int64_t>& ids) {
    AppController::PendingFieldEditDeleteSummary summary;
    if (!app_.Cache || ids.empty()) {
        return summary;
    }
    for (const std::int64_t id : ids) {
        try {
            app_.Cache->DeletePendingFieldEdit(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(),
                                            std::to_string(id), true, std::string(),
                                            nlohmann::json{{"pending_field_edit_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("OfflineQueueService::DeletePendingFieldEdits id=%lld err=%s", static_cast<long long>(id),
                      ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(),
                                            std::to_string(id), false, ex.what(),
                                            nlohmann::json{{"pending_field_edit_id", id}});
        } catch (...) {
            LOG_ERROR("OfflineQueueService::DeletePendingFieldEdits id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(),
                                            std::to_string(id), false, "Unknown exception.",
                                            nlohmann::json{{"pending_field_edit_id", id}});
        }
    }
    return summary;
}

AppController::DeadFieldEditDeleteSummary
OfflineQueueService::DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds) {
    AppController::DeadFieldEditDeleteSummary summary;
    if (!app_.Cache || deadIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : deadIds) {
        try {
            app_.Cache->DeleteDeadPendingFieldEdit(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(),
                                            std::to_string(id), true, std::string(),
                                            nlohmann::json{{"dead_field_edit_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("OfflineQueueService::DeleteDeadPendingFieldEdits dead_id=%lld err=%s",
                      static_cast<long long>(id), ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(),
                                            std::to_string(id), false, ex.what(),
                                            nlohmann::json{{"dead_field_edit_id", id}});
        } catch (...) {
            LOG_ERROR("OfflineQueueService::DeleteDeadPendingFieldEdits dead_id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(),
                                            std::to_string(id), false, "Unknown exception.",
                                            nlohmann::json{{"dead_field_edit_id", id}});
        }
    }
    return summary;
}

// --- Phase 1C: replay loops + replay-timer accessors ------------------------------------

void OfflineQueueService::PushReplayTimersForward(std::chrono::steady_clock::time_point pushTo) {
    std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
    nextOfflineReplayAt_ = (std::max)(nextOfflineReplayAt_, pushTo);
    nextOfflineFieldEditReplayAt_ = (std::max)(nextOfflineFieldEditReplayAt_, pushTo);
}

void OfflineQueueService::RestartReplayTimersNow(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
    nextOfflineReplayAt_ = now;
    nextOfflineFieldEditReplayAt_ = now;
}

void OfflineQueueService::TickOfflineFieldEdits() {
    if (ConfigManager::Load().ReadOnlyMode) {
        return;
    }
    if (!app_.Cache || !app_.Backend) {
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
        pending = app_.Cache->LoadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::TickOfflineFieldEdits load failed: %s", ex.what());
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    } catch (...) {
        LOG_ERROR("OfflineQueueService::TickOfflineFieldEdits load failed: unknown exception");
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    }
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    }

    LocalCacheManager* cache = app_.Cache.get();
    ITrackerClient* backend = app_.Backend.get();
    if (!backend) {
        return;
    }

    AppController& appRef = app_;
    app_.LaunchBackgroundTask([this, &appRef, pending, cache, backend]() {
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
                LOG_ERROR("OfflineQueueService::TickOfflineFieldEdits %s failed id=%lld err=%s", action,
                          static_cast<long long>(id), ex.what());
                return false;
            } catch (...) {
                ++cacheOpFailures;
                LOG_ERROR("OfflineQueueService::TickOfflineFieldEdits %s failed id=%lld err=unknown exception", action,
                          static_cast<long long>(id));
                return false;
            }
        };

        const int kMaxReplayAttempts = OfflineFieldEditQueue::kMaxReplayAttempts;
        for (const auto& row : pending) {
            if (row.HasMergeConflict) {
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
            // server document (theirs) and attempt to merge before replaying.
            bool skipMergeConflict = false;
            if (!row.OriginalRichValue.empty() && fieldsPayload.is_object()) {
                const std::string& fid = row.FieldId;
                std::string payloadKey = fid;
                if (!fieldsPayload.contains(fid)) {
                    const std::string altKey = fid + "_html";
                    if (fieldsPayload.contains(altKey)) payloadKey = altKey;
                }
                if (fieldsPayload.contains(payloadKey)) {
                    size_t biDetect = 0;
                    const std::string& brDetect = row.OriginalRichValue;
                    while (biDetect < brDetect.size() &&
                           (brDetect[biDetect] == ' ' || brDetect[biDetect] == '\t' ||
                            brDetect[biDetect] == '\n' || brDetect[biDetect] == '\r')) ++biDetect;
                    const bool isAdf = biDetect < brDetect.size() && brDetect[biDetect] == '{';

                    auto toMd = [](const std::string& rich) -> std::string {
                        if (rich.empty()) return rich;
                        size_t i = 0;
                        while (i < rich.size() &&
                               (rich[i] == ' ' || rich[i] == '\t' || rich[i] == '\n' || rich[i] == '\r')) ++i;
                        if (i < rich.size() && rich[i] == '{') {
                            try {
                                auto j = nlohmann::json::parse(rich);
                                if (j.is_object() && j.value("type", std::string()) == "doc")
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
                        const TrackerConfig cfgForFetch = ConfigManager::Load();
                        const ViewsStore viewsForFetch = ConfigManager::LoadViewsOrBootstrap(cfgForFetch);
                        std::vector<CachedTicket> freshTickets;
                        std::string fetchErr;
                        const std::vector<std::string> keysForFetch = {row.IssueKey};
                        if (backend->FetchIssuesForKeys(cfgForFetch, keysForFetch, viewsForFetch, freshTickets,
                                                        fetchErr) &&
                            !freshTickets.empty()) {

                            const CachedTicket& fresh = freshTickets.front();
                            const std::string theirsRich = fresh.GetFieldRichValue(fid);

                            if (!theirsRich.empty() && theirsRich != row.OriginalRichValue) {
                                const std::string baseMd = toMd(row.OriginalRichValue);
                                const std::string theirsMd = toMd(theirsRich);

                                std::string mineMd;
                                const auto& myVal = fieldsPayload[payloadKey];
                                if (myVal.is_object() && myVal.value("type", std::string()) == "doc") {
                                    mineMd = MarkdownConvert::AdfToMarkdown(myVal);
                                } else if (myVal.is_string()) {
                                    bool fell = false;
                                    mineMd = MarkdownConvert::HtmlSubsetToMarkdown(myVal.get<std::string>(), &fell);
                                    if (fell) mineMd = toMd(myVal.get<std::string>());
                                }

                                const TextMerge::MergeResult merged =
                                    TextMerge::ThreeWayMerge(baseMd, mineMd, theirsMd);
                                if (merged.IsClean) {
                                    if (isAdf) {
                                        fieldsPayload[payloadKey] = MarkdownConvert::MarkdownToAdf(merged.Text);
                                    } else {
                                        fieldsPayload[payloadKey] = MarkdownConvert::MarkdownToHtml(merged.Text);
                                    }
                                    LOG_INFO("TickOfflineFieldEdits: 3-way merge clean for issue=%s field=%s",
                                             row.IssueKey.c_str(), fid.c_str());
                                } else {
                                    LOG_WARN("TickOfflineFieldEdits: 3-way merge conflict for issue=%s field=%s — "
                                             "suspending replay pending user resolution",
                                             row.IssueKey.c_str(), fid.c_str());
                                    const nlohmann::json ctx = {{"base", baseMd},
                                                                {"mine", mineMd},
                                                                {"theirs", theirsMd},
                                                                {"fieldId", fid},
                                                                {"richKind", isAdf ? "adf" : "html"}};
                                    tryCacheMutation("mark_field_edit_conflict", row.Id,
                                                     [&]() { cache->MarkFieldEditConflict(row.Id, ctx.dump()); });
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
                ++failures;
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
                appRef.requestDeferredLiveTrackerBackendSuccessNotify_();
                BackendAuditTrail::AppendResult(
                    "offline_replay_field_edit", "offline_field_replay", row.IssueKey, std::to_string(row.Id), true,
                    std::string(), nlohmann::json{{"pending_field_edit_id", row.Id}, {"field_id", row.FieldId}});
            } else {
                ++failures;
            }
        }

        if (successes > 0) {
            appRef.RefreshLocalData();
        }
        if (successes > 0 || failures > 0 || archived > 0 || cacheOpFailures > 0) {
            LOG_INFO("OfflineQueueService: offline field edit replay finished successes=%d failures=%d archived=%d "
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

void OfflineQueueService::TickOfflineCreates() {
    if (ConfigManager::Load().ReadOnlyMode) {
        return;
    }
    if (!app_.Cache || !app_.Backend) {
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
        pending = app_.Cache->LoadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::TickOfflineCreates load failed: %s", ex.what());
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    } catch (...) {
        LOG_ERROR("OfflineQueueService::TickOfflineCreates load failed: unknown exception");
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    }
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    }

    auto catalogCopy = std::make_shared<std::vector<TrackerField>>(app_.AvailableFields);
    ITrackerClient* backend = app_.Backend.get();
    LocalCacheManager* cache = app_.Cache.get();

    AppController& appRef = app_;
    app_.LaunchBackgroundTask([this, &appRef, pending, backend, cache, catalogCopy]() {
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
                LOG_ERROR("OfflineQueueService::TickOfflineCreates %s failed id=%lld err=%s", action,
                          static_cast<long long>(id), ex.what());
                return false;
            } catch (...) {
                ++cacheOpFailures;
                LOG_ERROR("OfflineQueueService::TickOfflineCreates %s failed id=%lld err=unknown exception", action,
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
                LOG_WARN("OfflineQueueService: archiving malformed pending_create id=%lld err=%s",
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
                appRef.GetRequiredFieldSet(draft.ProjectKey, draft.IssueTypeId, draft.IssueTypeName);
            ranCreate = true;
            IssueCreateResult result = IssueCreatePipeline::Run(*backend, cache, draft, required, *catalogCopy);
            if (result.Ok) {
                if (tryCacheMutation("delete_pending_create", pc.Id, [&]() { cache->DeletePendingCreate(pc.Id); })) {
                    ++successes;
                    appRef.requestDeferredLiveTrackerBackendSuccessNotify_();
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
            appRef.RefreshLocalData();
        }
        if (successes > 0 || failures > 0 || archived > 0 || cacheOpFailures > 0) {
            LOG_INFO("OfflineQueueService: offline replay finished successes=%d failures=%d archived=%d "
                     "cache_op_failures=%d",
                     successes, failures, archived, cacheOpFailures);
        }
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
