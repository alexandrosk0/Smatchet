#include "OfflineQueueService.h"

#include "AppController.h"
#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "IOfflineQueueDeps.h"
#include "IssueCreatePipeline.h"
#include "IssueDraft.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"
#include "ISyncCache.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "MarkdownConvert.h"
#include "OfflineFieldConflictPolicy.h"
#include "OfflineQueueReplayPolicy.h"
#include "TextMerge.h"
#include "TrackerHttpPure.h"
#include "Views.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace {

// Minimal RAII "run this closure on scope exit" helper — fires on normal return AND on
// exception unwind, unlike a bare tail statement. TickOfflineCreates uses it to guarantee
// offlineReplayInFlight_ resets even if ReplayOneCreate throws mid-loop (a bare
// cache->SaveTicket in IssueCreatePipeline::Run could do exactly this before that call
// site was wrapped in try/catch — CPP_CODE_AUDIT.md #6). Without this, the
// LaunchBackgroundTask firewall catches the exception (no crash) but the lambda tail that
// resets the latch never runs, so offline replay goes silently dead until restart.
class ScopeExit {
  public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeExit() {
        if (fn_) {
            fn_();
        }
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

  private:
    std::function<void()> fn_;
};

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

// Build the AUDIT-TRAIL copy of a queued draft: parse the serialized payload back into a
// structured JSON object and run it through the audit trail's key-name redactor while the
// nested `fields` are still real JSON keys. A flat serialized string would slip the sensitive
// nested keys past the redactor (security #10). RedactJson is idempotent, so AppendEvent's own
// redaction pass leaves this already-redacted object unchanged. On the (not-expected) parse
// failure we emit a placeholder rather than the raw string, so we never leak an unredacted draft.
nlohmann::json MakeAuditDraft(const std::string& payload) {
    std::string parseErr;
    nlohmann::json parsed = smatchet::json_safe::ParseBounded(payload, parseErr);
    if (!parseErr.empty()) {
        return nlohmann::json{{"redaction_error", "draft payload not parseable for audit redaction"}};
    }
    return BackendAuditTrail::RedactJson(parsed);
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

// Skip leading ASCII whitespace, returning the index of the first non-space char (or size()).
size_t FirstNonWhitespace(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
    return i;
}

// Multi-grid Slice 1c replay-matching rule (ADR-0018 decision 4): a context's replay tick
// processes ONLY rows whose `backend_key` strictly equals the replaying context's key. Rows
// whose backend has no live context simply stay queued — never replayed against a wrong
// backend, never dropped. Post-stamp an empty key cannot occur; if encountered (corrupt or
// hand-edited DB) treat as no-match + WARN. `RowT` is PendingCreate / PendingFieldEditRecord
// (both carry `Id` + `BackendKey`).
template <typename RowT>
void FilterRowsToReplayBackendKey(std::vector<RowT>& rows, const std::string& replayBackendKey, const char* tickName) {
    if (replayBackendKey.empty()) {
        // Tick fired before the backend resolved its cache key (CR-951-2). Stay-queued is the
        // safe outcome — but make the skip visible (once per tick, not per row) instead of
        // silently dropping every keyed row from the replay set.
        if (!rows.empty()) {
            LOG_WARN("OfflineQueueService::%s: replay backend key is empty (tick before backend init?) — "
                     "%zu queued row(s) skipped this pass, left queued",
                     tickName, rows.size());
        }
        rows.clear();
        return;
    }
    std::vector<RowT> matched;
    matched.reserve(rows.size());
    for (auto& row : rows) {
        if (row.BackendKey == replayBackendKey) {
            matched.push_back(std::move(row));
        } else if (row.BackendKey.empty()) {
            LOG_WARN("OfflineQueueService::%s: row id=%lld has empty backend_key (corrupt/hand-edited DB) — "
                     "treated as no-match, left queued",
                     tickName, static_cast<long long>(row.Id));
        }
    }
    rows.swap(matched);
}
} // namespace

namespace OfflineFieldEditMergeDetail {
// Pure rich-content -> markdown helpers, factored out of the 3-way merge path so they are
// unit-testable in isolation and keep `ResolveFieldEditThreeWayMerge` under the branch cap.

bool DetectRichKindIsAdf(const std::string& rich) {
    const size_t i = FirstNonWhitespace(rich);
    return i < rich.size() && rich[i] == '{';
}

std::string RichToMarkdown(const std::string& rich) {
    if (rich.empty())
        return rich;
    const size_t i = FirstNonWhitespace(rich);
    if (i < rich.size() && rich[i] == '{') {
        try {
            std::string parseErr;
            auto j = smatchet::json_safe::ParseBounded(rich, parseErr);
            if (parseErr.empty() && j.is_object() && j.value("type", std::string()) == "doc")
                return MarkdownConvert::AdfToMarkdown(j);
        } catch (...) {
            // catch-all-ok: malformed/partial rich JSON falls through to return the
            // raw string verbatim below — a non-throwing deterministic fallback for
            // untrusted queued payloads (mirrors the pre-refactor merge-path catch).
        }
    }
    if (i < rich.size() && rich[i] == '<') {
        bool fell = false;
        return MarkdownConvert::HtmlSubsetToMarkdown(rich, &fell);
    }
    return rich;
}

// Convert the queued "mine" payload value (ADF doc object or HTML/markdown string) to markdown.
std::string MineValueToMarkdown(const nlohmann::json& myVal) {
    if (myVal.is_object() && myVal.value("type", std::string()) == "doc") {
        return MarkdownConvert::AdfToMarkdown(myVal);
    }
    if (myVal.is_string()) {
        bool fell = false;
        std::string mineMd = MarkdownConvert::HtmlSubsetToMarkdown(myVal.get<std::string>(), &fell);
        if (fell)
            mineMd = RichToMarkdown(myVal.get<std::string>());
        return mineMd;
    }
    return std::string();
}
} // namespace OfflineFieldEditMergeDetail

OfflineQueueService::OfflineQueueService(IOfflineQueueDeps& deps) : deps_(deps) {}

std::size_t OfflineQueueService::GetPendingCreateCount() const {
    if (!deps_.Cache()) {
        return 0;
    }
    try {
        return deps_.Cache()->LoadPendingCreates().size();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetPendingCreateCount failed: %s", ex.what());
        return 0;
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetPendingCreateCount failed: unknown exception");
        return 0;
    }
}

std::size_t OfflineQueueService::GetDeadPendingCreateCount() const {
    if (!deps_.Cache()) {
        return 0;
    }
    try {
        return deps_.Cache()->GetDeadPendingCreateCount();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreateCount failed: %s", ex.what());
        return 0;
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreateCount failed: unknown exception");
        return 0;
    }
}

std::vector<PendingCreate> OfflineQueueService::GetPendingCreates() const {
    if (!deps_.Cache()) {
        return {};
    }
    try {
        return deps_.Cache()->LoadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetPendingCreates failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetPendingCreates failed: unknown exception");
        return {};
    }
}

std::vector<DeadPendingCreate> OfflineQueueService::GetDeadPendingCreates() const {
    if (!deps_.Cache()) {
        return {};
    }
    try {
        return deps_.Cache()->LoadDeadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreates failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreates failed: unknown exception");
        return {};
    }
}

std::string OfflineQueueService::TakeLegacyPendingStartupBanner() { return std::move(legacyPendingStartupBanner_); }

void OfflineQueueService::RunLegacyProjectSweep(const std::string& legacyJiraProjectKey,
                                                const std::string& legacyPlaneProjectId,
                                                const std::string& trackerType) {
    static const std::string kSweepFlag = "legacy_project_swept_v1";
    if (!deps_.Cache()) {
        LOG_WARN("OfflineQueueService::RunLegacyProjectSweep skipped: cache not initialized.");
        return;
    }
    try {
        if (deps_.Cache()->HasCacheMetaFlag(kSweepFlag)) {
            return;
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::RunLegacyProjectSweep flag probe failed: %s", ex.what());
        return;
    }

    const bool backendIsPlane = ConfigManager::NormalizeViewsBackendKey(trackerType) == "Plane";
    const std::string& legacyForBackend = backendIsPlane ? legacyPlaneProjectId : legacyJiraProjectKey;

    std::vector<PendingCreate> rows;
    try {
        rows = deps_.Cache()->LoadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::RunLegacyProjectSweep LoadPendingCreates failed: %s", ex.what());
        return;
    }

    LegacyProjectSweepTally tally;
    for (const PendingCreate& pc : rows) {
        SweepOneLegacyPendingCreate(pc, legacyForBackend, tally);
    }

    try {
        deps_.Cache()->SetCacheMetaFlag(kSweepFlag);
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::RunLegacyProjectSweep flag set failed: %s", ex.what());
    }

    LOG_INFO("Legacy-project sweep: recovered=%d, dead-lettered=%d, untouched=%d", tally.Recovered, tally.DeadLettered,
             tally.Untouched);
}

void OfflineQueueService::SweepOneLegacyPendingCreate(const PendingCreate& pc, const std::string& legacyForBackend,
                                                      LegacyProjectSweepTally& tally) {
    IssueDraft draft;
    std::string parseErr;
    if (!IssueDraftHelpers::FromJson(pc.Payload, draft, parseErr)) {
        // Malformed payloads are handled by the normal replay tick, which archives them.
        // Skip here so we don't double-archive.
        ++tally.Untouched;
        return;
    }
    if (!draft.ProjectKey.empty()) {
        ++tally.Untouched;
        return;
    }
    // Strategy 1: parent key prefix (Jira-style PROJ-123).
    std::string recoveredKey;
    std::string recoverySource;
    if (!draft.ExistingIssueKey.empty()) {
        const std::size_t dashPos = draft.ExistingIssueKey.find('-');
        if (dashPos != std::string::npos && dashPos > 0) {
            recoveredKey = draft.ExistingIssueKey.substr(0, dashPos);
            recoverySource = std::string("parent='") + draft.ExistingIssueKey + "' -> '" + recoveredKey + "'";
        }
    }
    // Strategy 2: legacy global project (per-backend).
    if (recoveredKey.empty() && !legacyForBackend.empty()) {
        recoveredKey = legacyForBackend;
        recoverySource = std::string("legacy='") + legacyForBackend + "'";
    }

    if (!recoveredKey.empty()) {
        draft.ProjectKey = recoveredKey;
        const std::string newPayload = IssueDraftHelpers::ToJson(draft);
        try {
            deps_.Cache()->UpdatePendingCreatePayload(pc.Id, newPayload);
            ++tally.Recovered;
            LOG_INFO("Sweep: pending_create id=%lld project recovered from %s", static_cast<long long>(pc.Id),
                     recoverySource.c_str());
            BackendAuditTrail::AppendResult("offline_legacy_project_sweep", "startup_migration", std::string(),
                                            std::to_string(pc.Id), true, std::string(),
                                            nlohmann::json{{"pending_create_id", pc.Id},
                                                           {"recovery_source", recoverySource},
                                                           {"project", recoveredKey}});
        } catch (const std::exception& ex) {
            LOG_ERROR("Sweep: UpdatePendingCreatePayload failed id=%lld err=%s", static_cast<long long>(pc.Id),
                      ex.what());
            ++tally.Untouched;
        }
        return;
    }

    // Strategy 3: dead-letter.
    const std::string terminal = FormatOfflineQueueTerminalLine(
        "offline_legacy_project_sweep", "startup_migration", "legacy_missing_project",
        std::string("IssueDraft has no ProjectKey and no parent/legacy fallback is available."));
    try {
        deps_.Cache()->ArchivePendingCreate(pc.Id, "legacy_missing_project", terminal);
        ++tally.DeadLettered;
        LOG_WARN("Sweep: pending_create id=%lld dead-lettered (legacy_missing_project)", static_cast<long long>(pc.Id));
        BackendAuditTrail::AppendResult(
            "offline_dead_letter", "startup_migration", std::string(), std::to_string(pc.Id), true, std::string(),
            nlohmann::json{{"pending_create_id", pc.Id}, {"reason", "legacy_missing_project"}});
    } catch (const std::exception& ex) {
        LOG_ERROR("Sweep: ArchivePendingCreate failed id=%lld err=%s; falling back to delete",
                  static_cast<long long>(pc.Id), ex.what());
        try {
            deps_.Cache()->DeletePendingCreate(pc.Id);
            ++tally.DeadLettered;
            LOG_WARN("Sweep: pending_create id=%lld dropped (archive unavailable). payload_preview=%.200s",
                     static_cast<long long>(pc.Id), pc.Payload.c_str());
        } catch (const std::exception& ex2) {
            LOG_ERROR("Sweep: DeletePendingCreate fallback failed id=%lld err=%s", static_cast<long long>(pc.Id),
                      ex2.what());
            ++tally.Untouched;
        }
    }
}

// --- Phase 1B: write methods + remaining field-edit read accessors ----------------------

std::int64_t OfflineQueueService::QueueCreateOffline(const IssueDraft& draft) {
    if (ConfigManager::Load().ReadOnlyMode) {
        LOG_WARN("OfflineQueueService::QueueCreateOffline blocked by read-only mode.");
        return 0;
    }
    // DR6: this method runs on an off-UI thread (Lua automation / MCP worker), so take ONE
    // atomic_load snapshot of the cache and use it for the whole enqueue — the UI thread may swap
    // the cache in RecreateLocalCacheDatabase between the null-check and the EnqueuePendingCreate
    // below. Holding the shared_ptr keeps it alive across that gap. Mirrors the ADR-0012 Backend
    // atomic-swap latched-handle pattern.
    std::shared_ptr<ISyncCache> cache = deps_.CacheShared();
    if (!cache) {
        LOG_WARN("OfflineQueueService::QueueCreateOffline skipped: cache not initialized.");
        return 0;
    }
    const std::string payload = IssueDraftHelpers::ToJson(draft);
    // The live queue payload above is the FULL, unredacted draft — replay must reconstruct the
    // real field values. The audit copy is a SEPARATE, structurally-redacted view of the same
    // draft (security #10): `auditDraft` is the only form that touches the audit trail.
    const nlohmann::json auditDraft = MakeAuditDraft(payload);
    try {
        // Stamp the enqueuing context's backend namespace (multi-grid Slice 1c) — replay
        // strictly matches this key, so a create queued under Jira never replays against Plane.
        const std::int64_t id = cache->EnqueuePendingCreate(deps_.CacheBackendKey(), payload);
        LOG_INFO("OfflineQueueService: queued offline create id=%lld", static_cast<long long>(id));
        BackendAuditTrail::AppendResult("offline_queue_create", "ui", std::string(), std::to_string(id), true,
                                        std::string(),
                                        nlohmann::json{{"pending_create_id", id}, {"draft", auditDraft}});
        return id;
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::QueueCreateOffline failed: %s", ex.what());
        BackendAuditTrail::AppendResult("offline_queue_create", "ui", std::string(),
                                        BackendAuditTrail::MakeOperationId("offline-queue"), false, ex.what(),
                                        nlohmann::json{{"draft", auditDraft}});
        return 0;
    }
}

AppController::DeadLetterRestoreSummary
OfflineQueueService::RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds) {
    AppController::DeadLetterRestoreSummary summary;
    if (!deps_.Cache() || originalIds.empty()) {
        return summary;
    }
    // Restored creates are re-queued as FRESH creates: the cache restore applies the
    // `ExistingIssueKey` + issuekey/key scrub (`IssueDraftHelpers::ScrubFreshCreatePayload`)
    // INSIDE its own transaction (CR-951-1, CR-959) — atomic with the restore, no full
    // dead-table pre-load, and no exception path that can skip the scrub.
    for (const std::int64_t id : originalIds) {
        try {
            if (deps_.Cache()->RestoreDeadPendingCreate(id)) {
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

AppController::DeadFieldEditRestoreSummary
OfflineQueueService::RestoreDeadPendingFieldEdits(const std::vector<std::int64_t>& originalIds) {
    AppController::DeadFieldEditRestoreSummary summary;
    if (!deps_.Cache() || originalIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : originalIds) {
        try {
            if (deps_.Cache()->RestoreDeadPendingFieldEdit(id)) {
                ++summary.Restored;
                BackendAuditTrail::AppendResult("offline_dead_letter_restore_field_edit", "ui", std::string(),
                                                std::to_string(id), true, std::string(),
                                                nlohmann::json{{"original_pending_field_edit_id", id}});
            } else {
                ++summary.Failed;
                BackendAuditTrail::AppendResult("offline_dead_letter_restore_field_edit", "ui", std::string(),
                                                std::to_string(id), false, "Row not found.",
                                                nlohmann::json{{"original_pending_field_edit_id", id}});
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("OfflineQueueService::RestoreDeadPendingFieldEdits id=%lld err=%s", static_cast<long long>(id),
                      ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_restore_field_edit", "ui", std::string(),
                                            std::to_string(id), false, ex.what(),
                                            nlohmann::json{{"original_pending_field_edit_id", id}});
        } catch (...) {
            LOG_ERROR("OfflineQueueService::RestoreDeadPendingFieldEdits id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_letter_restore_field_edit", "ui", std::string(),
                                            std::to_string(id), false, "Unknown exception.",
                                            nlohmann::json{{"original_pending_field_edit_id", id}});
        }
    }
    return summary;
}

AppController::DeadLetterDeleteSummary
OfflineQueueService::DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds) {
    AppController::DeadLetterDeleteSummary summary;
    if (!deps_.Cache() || deadIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : deadIds) {
        try {
            deps_.Cache()->DeleteDeadPendingCreate(id);
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
    if (!deps_.Cache() || pendingIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : pendingIds) {
        try {
            deps_.Cache()->DeletePendingCreate(id);
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
                                                        const std::string& originalRichValue,
                                                        const std::string& originalValue, bool hasOriginalValue) {
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("OfflineQueueService::QueueFieldEditOffline blocked by read-only mode issue=%s field=%s",
                 issueKey.c_str(), fieldId.c_str());
        return 0;
    }
    if (!deps_.Cache()) {
        outError = SmatchetLocalization::T("offline.cache_unavailable",
                                           "Local cache is unavailable, so this edit cannot be queued offline. "
                                           "Restart Smatchet or check Settings -> Preferences -> Local data.");
        return 0;
    }
    if (issueKey.empty() || fieldId.empty() || fieldsPayloadJson.empty()) {
        outError = "This edit could not be queued offline (incomplete edit data). Retry the edit once Tracker is "
                   "reachable.";
        return 0;
    }
    try {
        // Stamp the enqueuing context's backend namespace (multi-grid Slice 1c) — see
        // QueueCreateOffline.
        const std::int64_t id =
            deps_.Cache()->EnqueuePendingFieldEdit(deps_.CacheBackendKey(), issueKey, fieldId, fieldsPayloadJson,
                                                   originalRichValue, originalValue, hasOriginalValue);
        LOG_INFO("OfflineQueueService: queued offline field edit id=%lld issue=%s field=%s", static_cast<long long>(id),
                 issueKey.c_str(), fieldId.c_str());
        BackendAuditTrail::AppendResult("offline_queue_field_edit", "ui", issueKey, std::to_string(id), true,
                                        std::string(),
                                        nlohmann::json{{"pending_field_edit_id", id}, {"field_id", fieldId}});
        return id;
    } catch (const std::exception& ex) {
        outError = "Saving this edit to the offline queue failed (local database error). Retry the edit once "
                   "Tracker is reachable.";
        LOG_ERROR("OfflineQueueService::QueueFieldEditOffline failed: %s", ex.what());
        BackendAuditTrail::AppendResult("offline_queue_field_edit", "ui", issueKey,
                                        BackendAuditTrail::MakeOperationId("offline-field-queue"), false, ex.what(),
                                        nlohmann::json{{"field_id", fieldId}});
        return 0;
    }
}

std::vector<PendingFieldEditRecord> OfflineQueueService::GetPendingFieldEdits() const {
    if (!deps_.Cache()) {
        return {};
    }
    try {
        return deps_.Cache()->LoadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetPendingFieldEdits failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetPendingFieldEdits failed: unknown exception");
        return {};
    }
}

std::vector<DeadPendingFieldEdit> OfflineQueueService::GetDeadPendingFieldEdits() const {
    if (!deps_.Cache()) {
        return {};
    }
    try {
        return deps_.Cache()->LoadDeadPendingFieldEdits();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingFieldEdits failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingFieldEdits failed: unknown exception");
        return {};
    }
}

const TrackerField* OfflineQueueService::FindCatalogField(const std::string& fieldId) const {
    const std::vector<TrackerField>& catalog = deps_.AvailableFields();
    const auto it =
        std::find_if(catalog.begin(), catalog.end(), [&](const TrackerField& f) { return f.Id == fieldId; });
    if (it == catalog.end()) {
        return nullptr;
    }
    return &(*it);
}

void OfflineQueueService::ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedValue,
                                                   const std::string& richKind, const std::string& kind) {
    if (!deps_.Cache())
        return;
    // ADR-0016 resolution shapes. A rich `text` resolution reconverts the resolved Markdown to
    // ADF/HTML via `richKind` and writes it into the payload key. A `scalar` resolution rebuilds
    // the payload through the production `BuildFieldPayload` builder so structured fields keep
    // their `{"id":...}` / array shape (#854 — a bare display string dead-letters with HTTP 400).
    // An `unverified` resolution ("Force Mine") keeps the original queued payload unchanged.
    const bool isRichText = (kind != "scalar" && kind != "unverified");
    const bool isUnverified = (kind == "unverified");
    try {
        try {
            auto existing = deps_.Cache()->LoadPendingFieldEdits();
            const auto rowIt =
                std::find_if(existing.begin(), existing.end(), [&](const auto& row) { return row.Id == id; });
            if (rowIt != existing.end()) {
                const auto& row = *rowIt;
                if (isUnverified) {
                    // Force Mine: replay the original backend-format payload untouched; only the
                    // conflict flag + bases are cleared by ResolveFieldEditConflict.
                    deps_.Cache()->ResolveFieldEditConflict(id, row.FieldsPayloadJson);
                    return;
                }
                nlohmann::json newPayload;
                {
                    // Malformed stored payload → rebuild from the key.
                    std::string parseErr;
                    newPayload = smatchet::json_safe::ParseBounded(row.FieldsPayloadJson, parseErr);
                    if (!parseErr.empty()) {
                        newPayload = nlohmann::json::object();
                    }
                }
                std::string payloadKey = row.FieldId;
                if (!newPayload.contains(payloadKey)) {
                    const std::string altKey = row.FieldId + "_html";
                    if (newPayload.contains(altKey))
                        payloadKey = altKey;
                }
                if (isRichText) {
                    if (richKind == "adf") {
                        newPayload[payloadKey] = MarkdownConvert::MarkdownToAdf(resolvedValue);
                    } else {
                        newPayload[payloadKey] = MarkdownConvert::MarkdownToHtml(resolvedValue);
                    }
                } else {
                    // Scalar (#854): rebuild the payload through the SAME production builder the
                    // live edit path uses (`BuildFieldPayload` -> `BuildValue`), so a structured
                    // field (single/multi-select, status, priority, issuetype, user, component,
                    // cascading, labels) keeps its `{"id":...}` / array shape instead of being
                    // clobbered with the bare display string (which Jira rejects with HTTP 400 ->
                    // retry -> dead-letter -> silent data loss). `mine`/`theirs` are display labels
                    // and `BuildValue` resolves them (by id OR label) exactly as on a normal edit.
                    // Genuinely string-valued fields (summary, free text) naturally come back as a
                    // bare string, so no special-casing is needed. The legacy verbatim write is the
                    // degraded fallback only when the field schema or the mutations builder is
                    // unavailable (so resolution still completes rather than no-ops).
                    bool rebuilt = false;
                    const TrackerField* field = FindCatalogField(row.FieldId);
                    if (field) {
                        const std::shared_ptr<ITrackerIssueMutations> mutations = deps_.MutationsShared();
                        if (mutations) {
                            std::vector<std::string> rawValues;
                            rawValues.push_back(resolvedValue);
                            const Result<nlohmann::json, TrackerError> built =
                                mutations->BuildFieldPayload(*field, rawValues);
                            if (built) {
                                newPayload = built.value();
                                rebuilt = true;
                            } else {
                                LOG_WARN("OfflineQueueService::ResolveFieldEditConflict id=%lld field=%s — "
                                         "BuildFieldPayload failed (%s); falling back to verbatim string",
                                         static_cast<long long>(id), row.FieldId.c_str(), built.error().Detail.c_str());
                            }
                        }
                    }
                    if (!rebuilt) {
                        newPayload[payloadKey] = resolvedValue;
                    }
                }
                deps_.Cache()->ResolveFieldEditConflict(id, newPayload.dump());
                return;
            }
        } catch (...) { // catch-all-ok: load/find failure → log-and-skip below (never fabricate a payload)
        }
        // Row not found (or load threw): there is no real field key to write under, so a fabricated
        // payload would lose the field key and misapply / dead-letter on the next replay. Leave the
        // row untouched — it stays conflict-suspended and the user can retry resolution. (ADR-0016.)
        LOG_WARN("OfflineQueueService::ResolveFieldEditConflict id=%lld — pending row not loadable; skipping (no "
                 "fabricated payload)",
                 static_cast<long long>(id));
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::ResolveFieldEditConflict id=%lld err=%s", static_cast<long long>(id),
                  ex.what());
    }
}

AppController::PendingFieldEditDeleteSummary
OfflineQueueService::DeletePendingFieldEdits(const std::vector<std::int64_t>& ids) {
    AppController::PendingFieldEditDeleteSummary summary;
    if (!deps_.Cache() || ids.empty()) {
        return summary;
    }
    for (const std::int64_t id : ids) {
        try {
            deps_.Cache()->DeletePendingFieldEdit(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            true, std::string(), nlohmann::json{{"pending_field_edit_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("OfflineQueueService::DeletePendingFieldEdits id=%lld err=%s", static_cast<long long>(id),
                      ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            false, ex.what(), nlohmann::json{{"pending_field_edit_id", id}});
        } catch (...) {
            LOG_ERROR("OfflineQueueService::DeletePendingFieldEdits id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_queue_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            false, "Unknown exception.", nlohmann::json{{"pending_field_edit_id", id}});
        }
    }
    return summary;
}

AppController::DeadFieldEditDeleteSummary
OfflineQueueService::DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds) {
    AppController::DeadFieldEditDeleteSummary summary;
    if (!deps_.Cache() || deadIds.empty()) {
        return summary;
    }
    for (const std::int64_t id : deadIds) {
        try {
            deps_.Cache()->DeleteDeadPendingFieldEdit(id);
            ++summary.Deleted;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            true, std::string(), nlohmann::json{{"dead_field_edit_id", id}});
        } catch (const std::exception& ex) {
            LOG_ERROR("OfflineQueueService::DeleteDeadPendingFieldEdits dead_id=%lld err=%s",
                      static_cast<long long>(id), ex.what());
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            false, ex.what(), nlohmann::json{{"dead_field_edit_id", id}});
        } catch (...) {
            LOG_ERROR("OfflineQueueService::DeleteDeadPendingFieldEdits dead_id=%lld unknown exception",
                      static_cast<long long>(id));
            ++summary.Failed;
            BackendAuditTrail::AppendResult("offline_dead_field_edit_delete", "ui", std::string(), std::to_string(id),
                                            false, "Unknown exception.", nlohmann::json{{"dead_field_edit_id", id}});
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
    if (!deps_.Cache() || !deps_.ReaderShared()) {
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
        pending = deps_.Cache()->LoadPendingFieldEdits();
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
    // Capture-then-check token (issue #1081): latched at work-capture time, re-checked
    // before the post-replay RefreshLocalData below so a replay that completed against the
    // OLD backend never wholesale-replaces the NEW backend's ActiveTickets after a swap.
    const std::uint64_t capturedGeneration = deps_.BackendGeneration();
    // Backend-scoped replay (multi-grid Slice 1c): only rows queued against THIS context's
    // backend are replayed; other backends' rows stay queued for their own context.
    FilterRowsToReplayBackendKey(pending, deps_.CacheBackendKey(), "TickOfflineFieldEdits");
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    }

    ISyncCache* cache = deps_.Cache();
    // LATCHED strong role handles (debt 2026-06-07): the shared_ptrs are captured into the
    // background task below, so a live backend swap (or pane-context retirement, multi-grid
    // Slice 3) mid-replay cannot dangle the subobject pointers — the replay completes
    // against the latched backend.
    std::shared_ptr<ITrackerIssueReader> reader = deps_.ReaderShared();
    std::shared_ptr<ITrackerIssueMutations> mutations = deps_.MutationsShared();
    if (!reader || !mutations) {
        // Mirror the three early-returns above: release the in-flight latch before bailing.
        // Otherwise offlineFieldEditReplayInFlight_ stays true forever, every later tick
        // short-circuits at the in-flight guard above, and offline field edits silently never
        // replay (the user's offline edits are lost). See build-quality-velocity-hardening #16.
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineFieldEditReplayInFlight_ = false;
        return;
    }

    IOfflineQueueDeps& depsRef = deps_;
    deps_.LaunchBackgroundTask([this, &depsRef, pending, cache, reader, mutations, capturedGeneration]() {
        // Guarantees offlineFieldEditReplayInFlight_ resets on every exit path from this lambda —
        // normal return OR an exception unwinding out of ReplayOneFieldEdit (RefreshLocalData
        // rethrows SQLite errors; the clean-merge MarkdownToAdf path can also throw), mirroring
        // the creates path. Without it a throw leaves the in-flight flag latched, every later tick
        // short-circuits at the in-flight guard, and queued edits never replay until restart (DR5).
        // Defaults to a conservative 30s retry delay; the normal-completion path overwrites it.
        std::chrono::seconds nextDelay{30};
        ScopeExit resetInFlight([this, &nextDelay]() {
            const auto nextAt = std::chrono::steady_clock::now() + nextDelay;
            std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
            nextOfflineFieldEditReplayAt_ = nextAt;
            offlineFieldEditReplayInFlight_ = false;
        });

        FieldEditReplayTally tally;
        for (const auto& row : pending) {
            if (row.HasMergeConflict) {
                continue;
            }
            ReplayOneFieldEdit(row, cache, reader.get(), mutations.get(), depsRef, tally);
        }

        if (tally.Successes > 0) {
            if (depsRef.BackendGeneration() == capturedGeneration) {
                // Pre-check above is only a cheap skip of the full-table cache read; the
                // checked overload re-checks the generation under activeTicketsMutex_
                // immediately before the swap-in (TOCTOU close).
                depsRef.RefreshLocalData(capturedGeneration);
            } else {
                LOG_INFO("OfflineQueueService::TickOfflineFieldEdits skipped RefreshLocalData — backend "
                         "generation moved mid-replay (issue #1081); replayed rows stay cached under their "
                         "own backend key.");
            }
        }
        if (tally.Successes > 0 || tally.Failures > 0 || tally.Archived > 0 || tally.CacheOpFailures > 0) {
            LOG_INFO("OfflineQueueService: offline field edit replay finished successes=%d failures=%d archived=%d "
                     "cache_op_failures=%d",
                     tally.Successes, tally.Failures, tally.Archived, tally.CacheOpFailures);
        }

        std::chrono::seconds delay{5};
        if (!tally.RanUpdate && !pending.empty()) {
            delay = std::chrono::seconds(300);
        } else if (tally.Failures > 0 && tally.Successes == 0) {
            delay = std::chrono::seconds(30);
        }
        nextDelay = delay;
    });
}

bool OfflineQueueService::RunFieldEditCacheMutation(const char* action, std::int64_t id,
                                                    const std::function<void()>& fn, FieldEditReplayTally& tally) {
    try {
        fn();
        return true;
    } catch (const std::exception& ex) {
        ++tally.CacheOpFailures;
        LOG_ERROR("OfflineQueueService::TickOfflineFieldEdits %s failed id=%lld err=%s", action,
                  static_cast<long long>(id), ex.what());
        return false;
    } catch (...) {
        ++tally.CacheOpFailures;
        LOG_ERROR("OfflineQueueService::TickOfflineFieldEdits %s failed id=%lld err=unknown exception", action,
                  static_cast<long long>(id));
        return false;
    }
}

OfflineQueueService::FieldEditConflictOutcome
OfflineQueueService::EvaluateFieldEditConflict(const PendingFieldEditRecord& row, nlohmann::json& fieldsPayload,
                                               ISyncCache* cache, ITrackerIssueReader* reader,
                                               FieldEditReplayTally& tally) {
    // Decide which base this row captured: rich (3-way text merge) vs scalar (2-way display
    // compare). A row with neither base keeps last-write-wins (documented residue, ADR-0016).
    // Scalar presence keys on the HasOriginalValue flag (NOT emptiness) so a genuinely BLANK
    // captured base is still conflict-checked instead of mistaken for a legacy no-base row.
    const bool hasRichBase = !row.OriginalRichValue.empty();
    const bool hasScalarBase = row.HasOriginalValue;
    if ((!hasRichBase && !hasScalarBase) || !fieldsPayload.is_object()) {
        return FieldEditConflictOutcome::Proceed;
    }

    // Single server re-fetch shared by both kinds. Decision (c): a based edit never dies
    // silently — a transient failure retries (until the cap, then asks); a permanent failure
    // (4xx, TrackerError::IsRetryable()==false) asks immediately.
    std::vector<CachedTicket> freshTickets;
    std::string fetchErr;
    bool fetchOk = false;
    bool fetchErrTransient = false;
    try {
        const TrackerConfig cfgForFetch = ConfigManager::Load();
        const ViewsStore viewsForFetch = ConfigManager::LoadViewsOrBootstrap(cfgForFetch);
        const std::vector<std::string> keysForFetch = {row.IssueKey};
        auto fetchResult = reader->FetchIssuesForKeys(cfgForFetch, keysForFetch, viewsForFetch);
        fetchOk = static_cast<bool>(fetchResult);
        if (fetchOk) {
            freshTickets = std::move(fetchResult.value());
        } else {
            fetchErr = fetchResult.error().Detail;
            // N12 item 13: the backend's structured kind decides retryability — no text sniff.
            fetchErrTransient = fetchResult.error().IsRetryable();
        }
    } catch (const std::exception& ex) {
        fetchOk = false;
        fetchErr = ex.what();
        fetchErrTransient = false; // a thrown re-fetch is a bug shape, not a transport signal
        LOG_WARN("TickOfflineFieldEdits: conflict re-fetch threw issue=%s field=%s err=%s", row.IssueKey.c_str(),
                 row.FieldId.c_str(), ex.what());
    } catch (...) {
        fetchOk = false;
        fetchErr = "unknown exception";
        fetchErrTransient = false;
        LOG_WARN("TickOfflineFieldEdits: conflict re-fetch threw (unknown) issue=%s field=%s", row.IssueKey.c_str(),
                 row.FieldId.c_str());
    }

    if (!fetchOk || freshTickets.empty()) {
        const bool transient = fetchOk ? true : fetchErrTransient;
        if (transient && !OfflineQueueReplayPolicy::ShouldArchive(row.Attempts + 1)) {
            // Transient + below cap: retry next tick (caller bumps attempts). Never a silent replay.
            LOG_INFO("TickOfflineFieldEdits: re-fetch unavailable (transient) issue=%s field=%s — retry next tick",
                     row.IssueKey.c_str(), row.FieldId.c_str());
            return FieldEditConflictOutcome::RetryTransient;
        }
        // Permanent error, OR transient that would hit the cap → ask the user (never dead-letter).
        LOG_WARN("TickOfflineFieldEdits: server value unverifiable issue=%s field=%s — recording unverified conflict",
                 row.IssueKey.c_str(), row.FieldId.c_str());
        return RecordUnverifiedFieldEditConflict(row, fieldsPayload, cache, tally);
    }

    const CachedTicket& fresh = freshTickets.front();
    if (hasRichBase) {
        return ResolveFieldEditThreeWayMerge(row, fieldsPayload, fresh, cache, tally);
    }
    return ResolveFieldEditScalarConflict(row, fieldsPayload, fresh, cache, tally);
}

OfflineQueueService::FieldEditConflictOutcome
OfflineQueueService::RecordUnverifiedFieldEditConflict(const PendingFieldEditRecord& row,
                                                       const nlohmann::json& fieldsPayload, ISyncCache* cache,
                                                       FieldEditReplayTally& tally) {
    // Best-effort "mine" display for the modal: the payload value under the field key, stringified.
    std::string mine;
    if (fieldsPayload.is_object()) {
        std::string payloadKey = row.FieldId;
        if (!fieldsPayload.contains(payloadKey)) {
            const std::string altKey = row.FieldId + "_html";
            if (fieldsPayload.contains(altKey))
                payloadKey = altKey;
        }
        if (fieldsPayload.contains(payloadKey)) {
            const nlohmann::json& v = fieldsPayload[payloadKey];
            mine = v.is_string() ? v.get<std::string>() : v.dump();
        }
    }
    nlohmann::json ctx;
    ctx["kind"] = "unverified";
    ctx["mine"] = mine;
    ctx["fieldId"] = row.FieldId;
    const std::string auditOp = BackendAuditTrail::MakeOperationId("offline-field-conflict");
    const nlohmann::json auditData{
        {"pending_field_edit_id", row.Id}, {"field_id", row.FieldId}, {"kind", "unverified"}};
    BackendAuditTrail::AppendBegin("offline_field_edit_conflict", FieldEditAuditSource::Current(), row.IssueKey,
                                   auditOp, auditData);
    RunFieldEditCacheMutation(
        "mark_field_edit_conflict_unverified", row.Id, [&]() { cache->MarkFieldEditConflict(row.Id, ctx.dump()); },
        tally);
    BackendAuditTrail::AppendResult("offline_field_edit_conflict", FieldEditAuditSource::Current(), row.IssueKey,
                                    auditOp, true, std::string(), auditData);
    return FieldEditConflictOutcome::Suspend;
}

OfflineQueueService::FieldEditConflictOutcome
OfflineQueueService::ResolveFieldEditScalarConflict(const PendingFieldEditRecord& row,
                                                    const nlohmann::json& fieldsPayload, const CachedTicket& fresh,
                                                    ISyncCache* cache, FieldEditReplayTally& tally) {
    const std::string& fid = row.FieldId;
    const std::string theirs = fresh.GetFieldValue(fid);
    // Presence-aware: a captured BLANK base is a real reference, so blank→non-blank is a move.
    if (!OfflineFieldConflictPolicy::ServerMovedFromCapturedBase(row.HasOriginalValue, row.OriginalValue, theirs)) {
        return FieldEditConflictOutcome::Proceed; // server unchanged → replay the queued value.
    }
    // The locally-applied display value is the user's intent ("mine"); the queued payload is
    // backend format and not shown. We surface the field's current cached display as mine.
    std::string mine = row.OriginalValue;
    if (fieldsPayload.is_object()) {
        std::string payloadKey = fid;
        if (!fieldsPayload.contains(payloadKey)) {
            const std::string altKey = fid + "_html";
            if (fieldsPayload.contains(altKey))
                payloadKey = altKey;
        }
        if (fieldsPayload.contains(payloadKey)) {
            const nlohmann::json& v = fieldsPayload[payloadKey];
            if (v.is_string())
                mine = v.get<std::string>();
        }
    }
    LOG_WARN("TickOfflineFieldEdits: scalar conflict issue=%s field=%s — suspending replay pending user resolution",
             row.IssueKey.c_str(), fid.c_str());
    nlohmann::json ctx;
    ctx["kind"] = "scalar";
    ctx["base"] = row.OriginalValue;
    ctx["mine"] = mine;
    ctx["theirs"] = theirs;
    ctx["fieldId"] = fid;
    const std::string auditOp = BackendAuditTrail::MakeOperationId("offline-field-conflict");
    const nlohmann::json auditData{{"pending_field_edit_id", row.Id}, {"field_id", fid}, {"kind", "scalar"}};
    BackendAuditTrail::AppendBegin("offline_field_edit_conflict", FieldEditAuditSource::Current(), row.IssueKey,
                                   auditOp, auditData);
    RunFieldEditCacheMutation(
        "mark_field_edit_conflict_scalar", row.Id, [&]() { cache->MarkFieldEditConflict(row.Id, ctx.dump()); }, tally);
    BackendAuditTrail::AppendResult("offline_field_edit_conflict", FieldEditAuditSource::Current(), row.IssueKey,
                                    auditOp, true, std::string(), auditData);
    return FieldEditConflictOutcome::Suspend;
}

OfflineQueueService::FieldEditConflictOutcome
OfflineQueueService::ResolveFieldEditThreeWayMerge(const PendingFieldEditRecord& row, nlohmann::json& fieldsPayload,
                                                   const CachedTicket& fresh, ISyncCache* cache,
                                                   FieldEditReplayTally& tally) {
    const std::string& fid = row.FieldId;
    std::string payloadKey = fid;
    if (!fieldsPayload.contains(fid)) {
        const std::string altKey = fid + "_html";
        if (fieldsPayload.contains(altKey))
            payloadKey = altKey;
    }
    if (!fieldsPayload.contains(payloadKey)) {
        return FieldEditConflictOutcome::Proceed;
    }

    const bool isAdf = OfflineFieldEditMergeDetail::DetectRichKindIsAdf(row.OriginalRichValue);
    const std::string theirsRich = fresh.GetFieldRichValue(fid);
    // Only an UNCHANGED server (theirs == base) skips the merge. An empty `theirsRich` that differs
    // from a non-empty base is a concurrent server clear/delete — a real change worth asking about
    // (ADR-0016: a clear IS a change), so it falls through to the 3-way merge below. When base is
    // also empty, theirs==base catches the no-op case here.
    if (theirsRich == row.OriginalRichValue) {
        return FieldEditConflictOutcome::Proceed; // server unchanged → replay the queued value.
    }

    const std::string baseMd = OfflineFieldEditMergeDetail::RichToMarkdown(row.OriginalRichValue);
    const std::string theirsMd = OfflineFieldEditMergeDetail::RichToMarkdown(theirsRich);
    const std::string mineMd = OfflineFieldEditMergeDetail::MineValueToMarkdown(fieldsPayload[payloadKey]);

    const TextMerge::MergeResult merged = TextMerge::ThreeWayMerge(baseMd, mineMd, theirsMd);
    const bool suspended =
        ApplyOrRecordMergeResult(row, fieldsPayload, payloadKey, isAdf, merged, baseMd, mineMd, theirsMd, cache, tally);
    return suspended ? FieldEditConflictOutcome::Suspend : FieldEditConflictOutcome::Proceed;
}

bool OfflineQueueService::ApplyOrRecordMergeResult(const PendingFieldEditRecord& row, nlohmann::json& fieldsPayload,
                                                   const std::string& payloadKey, bool isAdf,
                                                   const TextMerge::MergeResult& merged, const std::string& baseMd,
                                                   const std::string& mineMd, const std::string& theirsMd,
                                                   ISyncCache* cache, FieldEditReplayTally& tally) {
    if (merged.IsClean) {
        if (isAdf) {
            fieldsPayload[payloadKey] = MarkdownConvert::MarkdownToAdf(merged.Text);
        } else {
            fieldsPayload[payloadKey] = MarkdownConvert::MarkdownToHtml(merged.Text);
        }
        LOG_INFO("TickOfflineFieldEdits: 3-way merge clean for issue=%s field=%s", row.IssueKey.c_str(),
                 row.FieldId.c_str());
        return false;
    }
    LOG_WARN("TickOfflineFieldEdits: 3-way merge conflict for issue=%s field=%s — "
             "suspending replay pending user resolution",
             row.IssueKey.c_str(), row.FieldId.c_str());
    // `kind:"text"` selects the rich modal branch; `richKind` (orthogonal) drives reconversion.
    const nlohmann::json ctx = {{"kind", "text"},     {"base", baseMd},         {"mine", mineMd},
                                {"theirs", theirsMd}, {"fieldId", row.FieldId}, {"richKind", isAdf ? "adf" : "html"}};
    const std::string auditOp = BackendAuditTrail::MakeOperationId("offline-field-conflict");
    const nlohmann::json auditData{{"pending_field_edit_id", row.Id}, {"field_id", row.FieldId}, {"kind", "text"}};
    BackendAuditTrail::AppendBegin("offline_field_edit_conflict", FieldEditAuditSource::Current(), row.IssueKey,
                                   auditOp, auditData);
    RunFieldEditCacheMutation(
        "mark_field_edit_conflict", row.Id, [&]() { cache->MarkFieldEditConflict(row.Id, ctx.dump()); }, tally);
    BackendAuditTrail::AppendResult("offline_field_edit_conflict", FieldEditAuditSource::Current(), row.IssueKey,
                                    auditOp, true, std::string(), auditData);
    return true;
}

void OfflineQueueService::ReplayOneFieldEdit(const PendingFieldEditRecord& row, ISyncCache* cache,
                                             ITrackerIssueReader* reader, ITrackerIssueMutations* mutations,
                                             IOfflineQueueDeps& depsRef, FieldEditReplayTally& tally) {
    const int kMaxReplayAttempts = OfflineQueueReplayPolicy::kMaxReplayAttempts;
    if (OfflineQueueReplayPolicy::ShouldArchive(row.Attempts)) {
        char detailBuf[384];
        std::snprintf(detailBuf, sizeof(detailBuf),
                      "Queued offline field edit id %lld already at attempts=%d (max=%d).",
                      static_cast<long long>(row.Id), row.Attempts, kMaxReplayAttempts);
        const std::string terminal =
            FormatOfflineQueueTerminalLine("offline_field_replay", "max_attempt_gate", "max_attempts", detailBuf);
        if (RunFieldEditCacheMutation(
                "archive_pending_field_edit", row.Id,
                [&]() { cache->ArchivePendingFieldEdit(row.Id, "max_attempts", terminal); }, tally)) {
            ++tally.Archived;
        } else {
            ++tally.Failures;
        }
        return;
    }

    nlohmann::json fieldsPayload;
    {
        std::string parseErr;
        fieldsPayload = smatchet::json_safe::ParseBounded(row.FieldsPayloadJson, parseErr);
        if (!parseErr.empty()) {
            const std::string terminal =
                FormatOfflineQueueTerminalLine("offline_field_replay", "parse_payload", "malformed_json", parseErr);
            if (RunFieldEditCacheMutation(
                    "archive_pending_field_edit_malformed", row.Id,
                    [&]() { cache->ArchivePendingFieldEdit(row.Id, "malformed_json", terminal); }, tally)) {
                ++tally.Archived;
            } else {
                ++tally.Failures;
            }
            return;
        }
    }

    tally.RanUpdate = true;

    const FieldEditConflictOutcome conflict = EvaluateFieldEditConflict(row, fieldsPayload, cache, reader, tally);
    if (conflict == FieldEditConflictOutcome::Suspend) {
        // A rich, scalar, or unverified conflict was recorded — the row stays suspended for the
        // user to resolve before it replays.
        ++tally.Failures;
        return;
    }
    if (conflict == FieldEditConflictOutcome::RetryTransient) {
        // Transient re-fetch failure on a based row, below the cap: bump attempts, retry next tick.
        const int nextAttempts = row.Attempts + 1;
        (void)RunFieldEditCacheMutation(
            "update_pending_field_edit_refetch_retry", row.Id,
            [&]() { cache->UpdatePendingFieldEdit(row.Id, nextAttempts, "conflict re-fetch unavailable (transient)"); },
            tally);
        ++tally.Failures;
        return;
    }

    const TrackerError updateErr = mutations->UpdateIssueFields(row.IssueKey, fieldsPayload);
    if (!updateErr.IsOk()) {
        HandleFieldEditUpdateFailure(row, cache, updateErr, tally);
        return;
    }

    if (RunFieldEditCacheMutation(
            "delete_pending_field_edit", row.Id, [&]() { cache->DeletePendingFieldEdit(row.Id); }, tally)) {
        ++tally.Successes;
        depsRef.RequestDeferredLiveTrackerBackendSuccessNotify();
        BackendAuditTrail::AppendResult("offline_replay_field_edit", "offline_field_replay", row.IssueKey,
                                        std::to_string(row.Id), true, std::string(),
                                        nlohmann::json{{"pending_field_edit_id", row.Id}, {"field_id", row.FieldId}});
    } else {
        ++tally.Failures;
    }
}

void OfflineQueueService::HandleFieldEditUpdateFailure(const PendingFieldEditRecord& row, ISyncCache* cache,
                                                       const TrackerError& updateErr, FieldEditReplayTally& tally) {
    // N12 item 13: the mutation path returns a structured TrackerError — its kind decides
    // retryability directly; the Detail text is only for messages/audit.
    const std::string& err = updateErr.Detail;
    if (updateErr.IsRetryable()) {
        const int nextAttempts = row.Attempts + 1;
        if (OfflineQueueReplayPolicy::ShouldArchive(nextAttempts)) {
            const std::string terminal =
                FormatOfflineQueueTerminalLine("offline_field_replay", "transport_cap", "max_attempts",
                                               "Transport failures exhausted replay attempts: " + err);
            if (RunFieldEditCacheMutation(
                    "archive_pending_field_edit_transport_cap", row.Id,
                    [&]() {
                        cache->UpdatePendingFieldEdit(row.Id, nextAttempts, err);
                        cache->ArchivePendingFieldEdit(row.Id, "transport_cap", terminal);
                    },
                    tally)) {
                ++tally.Archived;
            } else {
                ++tally.Failures;
            }
        } else {
            (void)RunFieldEditCacheMutation(
                "update_pending_field_edit", row.Id,
                [&]() { cache->UpdatePendingFieldEdit(row.Id, nextAttempts, err); }, tally);
            ++tally.Failures;
        }
    } else {
        const std::string terminal =
            FormatOfflineQueueTerminalLine("offline_field_replay", "replay_rejected", "jira_error", err);
        if (RunFieldEditCacheMutation(
                "archive_pending_field_edit_rejected", row.Id,
                [&]() { cache->ArchivePendingFieldEdit(row.Id, "replay_rejected", terminal); }, tally)) {
            ++tally.Archived;
        } else {
            ++tally.Failures;
        }
    }
}

bool OfflineQueueService::RunCreateCacheMutation(const char* action, std::int64_t id, const std::function<void()>& fn,
                                                 CreateReplayTally& tally) {
    try {
        fn();
        return true;
    } catch (const std::exception& ex) {
        ++tally.CacheOpFailures;
        LOG_ERROR("OfflineQueueService::TickOfflineCreates %s failed id=%lld err=%s", action,
                  static_cast<long long>(id), ex.what());
        return false;
    } catch (...) {
        ++tally.CacheOpFailures;
        LOG_ERROR("OfflineQueueService::TickOfflineCreates %s failed id=%lld err=unknown exception", action,
                  static_cast<long long>(id));
        return false;
    }
}

void OfflineQueueService::ReplayOneCreate(const PendingCreate& pc, ISyncCache* cache, ITrackerIssueMutations* mutations,
                                          const std::vector<TrackerField>& catalog, const std::string& backendKey,
                                          IOfflineQueueDeps& depsRef, CreateReplayTally& tally) {
    const int kMaxReplayAttempts = OfflineQueueReplayPolicy::kMaxReplayAttempts;
    const auto archivePending = [&](const std::string& reason, const std::string& terminalError) -> bool {
        return RunCreateCacheMutation(
            "archive_pending_create", pc.Id, [&]() { cache->ArchivePendingCreate(pc.Id, reason, terminalError); },
            tally);
    };

    if (OfflineQueueReplayPolicy::ShouldArchive(pc.Attempts)) {
        char detailBuf[384];
        std::snprintf(detailBuf, sizeof(detailBuf),
                      "Queued offline create id %lld already at attempts=%d (max=%d). "
                      "Offline replay did not call Jira create; entry moved to Failed offline creates.",
                      static_cast<long long>(pc.Id), pc.Attempts, kMaxReplayAttempts);
        const std::string terminal =
            FormatOfflineQueueTerminalLine("offline_replay", "max_attempt_gate", "max_attempts", detailBuf);
        if (archivePending("max_attempts", terminal)) {
            ++tally.Archived;
            BackendAuditTrail::AppendResult("offline_dead_letter", "offline_replay", std::string(),
                                            std::to_string(pc.Id), true, std::string(),
                                            nlohmann::json{{"pending_create_id", pc.Id}, {"reason", "max_attempts"}});
        } else {
            ++tally.Failures;
        }
        return;
    }
    IssueDraft draft;
    std::string parseErr;
    if (!IssueDraftHelpers::FromJson(pc.Payload, draft, parseErr)) {
        LOG_WARN("OfflineQueueService: archiving malformed pending_create id=%lld err=%s",
                 static_cast<long long>(pc.Id), parseErr.c_str());
        const std::string terminal =
            FormatOfflineQueueTerminalLine("offline_replay", "parse_payload", "malformed_payload",
                                           std::string("IssueDraft JSON parse failed: ") + parseErr);
        if (archivePending("malformed_payload", terminal)) {
            ++tally.Archived;
            BackendAuditTrail::AppendResult(
                "offline_dead_letter", "offline_replay", std::string(), std::to_string(pc.Id), true, std::string(),
                nlohmann::json{{"pending_create_id", pc.Id}, {"reason", "malformed_payload"}, {"error", parseErr}});
        } else {
            ++tally.Failures;
        }
        return;
    }
    const RequiredFieldSet required =
        depsRef.GetRequiredFieldSet(draft.ProjectKey, draft.IssueTypeId, draft.IssueTypeName);
    tally.RanCreate = true;
    // `backendKey` is the key CAPTURED at work-capture time (issue #1081) — the key the rows
    // were queued + filtered against. A write-time deps re-read here raced a mid-replay
    // backend swap and seeded the old backend's row under the new backend's namespace.
    IssueCreateResult result = IssueCreatePipeline::Run(*mutations, cache, backendKey, draft, required, catalog);
    if (result.Ok) {
        if (RunCreateCacheMutation(
                "delete_pending_create", pc.Id, [&]() { cache->DeletePendingCreate(pc.Id); }, tally)) {
            ++tally.Successes;
            depsRef.RequestDeferredLiveTrackerBackendSuccessNotify();
            BackendAuditTrail::AppendResult(
                "offline_replay_create", "offline_replay", result.IssueKey, std::to_string(pc.Id), true, std::string(),
                nlohmann::json{{"pending_create_id", pc.Id}, {"attempts_before", pc.Attempts}});
        } else {
            ++tally.Failures;
        }
        return;
    }
    const int nextAttempts = pc.Attempts + 1;
    if (OfflineQueueReplayPolicy::ShouldArchive(nextAttempts)) {
        std::string trackerPart =
            result.Error.empty() ? std::string("Create pipeline returned failure with empty error on final attempt.")
                                 : std::string("Create pipeline error: ") + result.Error;
        char headBuf[224];
        std::snprintf(headBuf, sizeof(headBuf), "Offline replay attempts went from %d to %d (cap=%d). ", pc.Attempts,
                      nextAttempts, kMaxReplayAttempts);
        const std::string terminalError = FormatOfflineQueueTerminalLine(
            "offline_replay", "issue_create", "max_attempts", std::string(headBuf) + trackerPart);
        const bool archivedOk = RunCreateCacheMutation(
            "archive_pending_create", pc.Id,
            [&]() {
                cache->UpdatePendingCreate(pc.Id, nextAttempts, terminalError);
                cache->ArchivePendingCreate(pc.Id, "max_attempts", terminalError);
            },
            tally);
        if (archivedOk) {
            ++tally.Archived;
            BackendAuditTrail::AppendResult(
                "offline_dead_letter", "offline_replay", std::string(), std::to_string(pc.Id), true, std::string(),
                nlohmann::json{{"pending_create_id", pc.Id}, {"reason", "max_attempts"}, {"error", result.Error}});
        } else {
            ++tally.Failures;
        }
        return;
    }
    const bool updateOk = RunCreateCacheMutation(
        "update_pending_create", pc.Id, [&]() { cache->UpdatePendingCreate(pc.Id, nextAttempts, result.Error); },
        tally);
    (void)updateOk;
    BackendAuditTrail::AppendResult(
        "offline_replay_create", "offline_replay", std::string(), std::to_string(pc.Id), false, result.Error,
        nlohmann::json{
            {"pending_create_id", pc.Id}, {"attempts_before", pc.Attempts}, {"attempts_after", nextAttempts}});
    ++tally.Failures;
}

void OfflineQueueService::TickOfflineCreates() {
    if (ConfigManager::Load().ReadOnlyMode) {
        return;
    }
    if (!deps_.Cache() || !deps_.ReaderShared()) {
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
        pending = deps_.Cache()->LoadPendingCreates();
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
    // Capture key + generation TOGETHER at work-capture time (issue #1081): the captured key
    // is passed through ReplayOneCreate into IssueCreatePipeline::Run so a successful create
    // seeds the cache under the backend the rows were QUEUED against — a write-time
    // CacheBackendKey() re-read after a mid-replay swap landed Jira rows in the GitHub
    // namespace (proven TOCTOU). The generation gates the post-replay RefreshLocalData.
    const std::string capturedBackendKey = deps_.CacheBackendKey();
    const std::uint64_t capturedGeneration = deps_.BackendGeneration();
    // Backend-scoped replay (multi-grid Slice 1c): only rows queued against THIS context's
    // backend are replayed; other backends' rows stay queued for their own context.
    FilterRowsToReplayBackendKey(pending, capturedBackendKey, "TickOfflineCreates");
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    }

    auto catalogCopy = std::make_shared<std::vector<TrackerField>>(deps_.AvailableFields());
    // LATCHED strong mutation handle (debt 2026-06-07): captured into the background task so
    // a live backend swap / pane-context retirement mid-replay cannot dangle it.
    std::shared_ptr<ITrackerIssueMutations> mutations2 = deps_.MutationsShared();
    if (!mutations2) {
        // Mirror the null guard in TickOfflineFieldEdits: a latched backend with no mutations
        // support must release the in-flight latch before bailing, otherwise ReplayOneCreate
        // dereferences a null mutation handle and every later tick short-circuits (DR5).
        std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
        offlineReplayInFlight_ = false;
        return;
    }
    ISyncCache* cache = deps_.Cache();

    IOfflineQueueDeps& depsRef = deps_;
    deps_.LaunchBackgroundTask(
        [this, &depsRef, pending, mutations2, cache, catalogCopy, capturedBackendKey, capturedGeneration]() {
            // Guarantees offlineReplayInFlight_ resets on every exit path from this lambda —
            // normal return, an early return, OR an exception unwinding out of
            // ReplayOneCreate (LaunchBackgroundTask's own top-level catch prevents a crash but
            // does NOT run this lambda's tail) — see the ScopeExit doc comment / #6. Defaults
            // to a conservative 30s retry delay; the normal-completion path below overwrites
            // it with the tally-computed delay before the guard fires.
            std::chrono::seconds nextDelay{30};
            ScopeExit resetInFlight([this, &nextDelay]() {
                const auto nextAt = std::chrono::steady_clock::now() + nextDelay;
                std::lock_guard<std::mutex> lock(offlineReplayScheduleMutex_);
                nextOfflineReplayAt_ = nextAt;
                offlineReplayInFlight_ = false;
            });

            CreateReplayTally tally;
            for (const auto& pc : pending) {
                ReplayOneCreate(pc, cache, mutations2.get(), *catalogCopy, capturedBackendKey, depsRef, tally);
            }
            if (tally.Successes > 0) {
                if (depsRef.BackendGeneration() == capturedGeneration) {
                    // Pre-check above is only a cheap skip of the full-table cache read; the
                    // checked overload re-checks the generation under activeTicketsMutex_
                    // immediately before the swap-in (TOCTOU close).
                    depsRef.RefreshLocalData(capturedGeneration);
                } else {
                    LOG_INFO("OfflineQueueService::TickOfflineCreates skipped RefreshLocalData — backend "
                             "generation moved mid-replay (issue #1081); created rows stay cached under "
                             "backend key '%s'.",
                             capturedBackendKey.c_str());
                }
            }
            if (tally.Successes > 0 || tally.Failures > 0 || tally.Archived > 0 || tally.CacheOpFailures > 0) {
                LOG_INFO("OfflineQueueService: offline replay finished successes=%d failures=%d archived=%d "
                         "cache_op_failures=%d",
                         tally.Successes, tally.Failures, tally.Archived, tally.CacheOpFailures);
            }
            std::chrono::seconds delay{5};
            if (!tally.RanCreate && !pending.empty()) {
                delay = std::chrono::seconds(300);
            } else if (tally.Failures > 0 && tally.Successes == 0) {
                delay = std::chrono::seconds(30);
            }
            nextDelay = delay;
        });
}
