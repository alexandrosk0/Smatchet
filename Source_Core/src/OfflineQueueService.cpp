#include "OfflineQueueService.h"

#include "AppController.h"
#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "MarkdownConvert.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

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
