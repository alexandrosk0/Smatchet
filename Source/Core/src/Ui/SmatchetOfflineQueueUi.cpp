#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
// SMATCHET_DEVIATION(rule=duplication; reason=pre-existing boilerplate / include-block clone surfaced by the ParseBounded security sweep touching this file; de-duping independent subsystems is DRY-CRITICAL; owner=security-audit; revisit=2026-09-30)
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "Json/BoundedJsonParse.h"
#include "MarkdownConvert.h"
#include "MarkdownPreviewRender.h"
#include "TrackerHttpUtils.h"
#include "SmatchetHelpMarker.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h"
#include "StringUtil.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

static std::string FormatEpochLocal(std::int64_t epochSec) {
    if (epochSec <= 0) {
        return "-";
    }
    const std::time_t tt = static_cast<std::time_t>(epochSec);
    std::tm tmLocal{};
#if defined(_WIN32)
    if (localtime_s(&tmLocal, &tt) != 0) {
        return "-";
    }
#else
    if (localtime_r(&tt, &tmLocal) == nullptr) {
        return "-";
    }
#endif
    char buf[64];
    const size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmLocal);
    if (n == 0) {
        return "-";
    }
    return std::string(buf, n);
}

static std::string BuildPayloadPreview(const std::string& payload, size_t maxChars) {
    std::string out;
    out.reserve((std::min)(payload.size(), maxChars));
    for (char ch : payload) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
        if (out.size() >= maxChars) {
            break;
        }
    }
    if (payload.size() > maxChars) {
        out += "...";
    }
    return out;
}

static float OfflineAuxTableOuterHeight(size_t rowCount) {
    const int visibleDataRows = static_cast<int>((std::min)(rowCount, size_t(10)));
    const ImGuiStyle& st = ImGui::GetStyle();
    const float headerH = ImGui::GetFrameHeightWithSpacing();
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    return headerH + static_cast<float>(visibleDataRows) * rowH + st.CellPadding.y * 2.0f + 2.0f;
}

static std::string ExtractBetweenKeys(const std::string& s, const std::string& startKey, const std::string& endKey) {
    const size_t p = s.find(startKey);
    if (p == std::string::npos) {
        return std::string();
    }
    const size_t a = p + startKey.size();
    if (endKey.empty()) {
        return s.substr(a);
    }
    const size_t b = s.find(endKey, a);
    if (b == std::string::npos) {
        return s.substr(a);
    }
    return s.substr(a, b - a);
}

static std::string DeadLetterActionFromLastError(const std::string& lastError) {
    return ExtractBetweenKeys(lastError, "action=", " stage=");
}

static std::string DeadLetterStageFromLastError(const std::string& lastError) {
    return ExtractBetweenKeys(lastError, "stage=", " reason=");
}

static std::string DeadLetterDetailFromLastError(const std::string& lastError) {
    return ExtractBetweenKeys(lastError, "detail=", "");
}

static std::string BuildDeadLetterRowTsv(const DeadPendingCreate& row) {
    const std::string createdAt = FormatEpochLocal(row.CreatedAtEpochSec);
    const std::string archivedAt = FormatEpochLocal(row.ArchivedAtEpochSec);
    const std::string action = DeadLetterActionFromLastError(row.LastError);
    const std::string stage = DeadLetterStageFromLastError(row.LastError);
    const std::string detailRaw = DeadLetterDetailFromLastError(row.LastError);
    const std::string detailCol = detailRaw.empty() ? row.LastError : detailRaw;
    std::string out;
    out.reserve(256 + row.LastError.size() + row.Payload.size());
    out += std::to_string(static_cast<long long>(row.DeadId));
    out += '\t';
    out += std::to_string(static_cast<long long>(row.OriginalId));
    out += '\t';
    out += std::to_string(row.Attempts);
    out += '\t';
    out += SanitizeClipboardCell(row.TerminalReason.empty() ? std::string("-") : row.TerminalReason);
    out += '\t';
    out += SanitizeClipboardCell(action.empty() ? std::string("-") : action);
    out += '\t';
    out += SanitizeClipboardCell(stage.empty() ? std::string("-") : stage);
    out += '\t';
    out += SanitizeClipboardCell(detailCol.empty() ? std::string("-") : detailCol);
    out += '\t';
    out += SanitizeClipboardCell(createdAt);
    out += '\t';
    out += SanitizeClipboardCell(archivedAt);
    out += '\t';
    out += SanitizeClipboardCell(row.Payload);
    return out;
}

static void ArmDeadLetterPanelStatus(UiDrawSession& d, const std::string& msg) {
    d.deadLetterPanelStatus = msg;
    d.deadLetterPanelStatusHasClearDeadline = true;
    d.deadLetterPanelStatusClearAt = std::chrono::steady_clock::now() + std::chrono::seconds(15);
}

static void ArmOfflineQueuePanelStatus(UiDrawSession& d, const std::string& msg) {
    d.offlineQueuePanelStatus = msg;
    d.offlineQueuePanelStatusHasClearDeadline = true;
    d.offlineQueuePanelStatusClearAt = std::chrono::steady_clock::now() + std::chrono::seconds(15);
}

static void OpenDeadLetterRowAsNewIssueDraft(UiDrawSession& d, const DeadPendingCreate& row) {
    if (d.newIssueCreateInFlight && d.newIssueCreateFuture.valid() &&
        d.newIssueCreateFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        ArmDeadLetterPanelStatus(d, "Open as draft: wait for the in-flight Create to finish, or cancel from the grid.");
        return;
    }
    if (d.newIssueCreateInFlight && d.newIssueCreateFuture.valid()) {
        (void)d.newIssueCreateFuture.get();
        d.newIssueCreateInFlight = false;
        d.newIssueDiscardAsyncCreateResult = false;
    }
    CancelUnfinishedNewIssueForGridChange(d);

    IssueDraft parsed;
    std::string err;
    if (!IssueDraftHelpers::FromJson(row.Payload, parsed, err)) {
        ArmDeadLetterPanelStatus(d, std::string("Open as draft: JSON parse failed: ") + err);
        return;
    }
    if (!parsed.ParentKey.empty() && parsed.FieldValues.find("parent") == parsed.FieldValues.end()) {
        parsed.FieldValues["parent"] = parsed.ParentKey;
    }
    parsed.ExistingIssueKey.clear();
    parsed.FieldValues.erase("issuekey");
    parsed.FieldValues.erase("key");
    if (!parsed.IssueTypeId.empty()) {
        parsed.FieldValues["issuetype"] = parsed.IssueTypeId;
    } else if (!parsed.IssueTypeName.empty()) {
        parsed.FieldValues["issuetype"] = parsed.IssueTypeName;
    }
    if (!parsed.ProjectKey.empty()) {
        parsed.FieldValues["project"] = parsed.ProjectKey;
    }

    d.newIssueDraft = std::move(parsed);
    d.newIssueDraftActive = true;
    d.newIssueDraftEditBufs.clear();
    d.newIssueMissingFieldIds.clear();
    d.newIssueCreateInFlight = false;
    d.newIssueDiscardAsyncCreateResult = false;

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Loaded archived offline create (id %lld) into new-issue draft — fix parent/type, then "
                  "Create or queue offline.",
                  static_cast<long long>(row.DeadId));
    ArmDeadLetterPanelStatus(d, buf);
}

static void OpenPendingCreateRowAsNewIssueDraft(UiDrawSession& d, const PendingCreate& row) {
    if (d.newIssueCreateInFlight && d.newIssueCreateFuture.valid() &&
        d.newIssueCreateFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        ArmOfflineQueuePanelStatus(d,
                                   "Open as draft: wait for the in-flight Create to finish, or cancel from the grid.");
        return;
    }
    if (d.newIssueCreateInFlight && d.newIssueCreateFuture.valid()) {
        (void)d.newIssueCreateFuture.get();
        d.newIssueCreateInFlight = false;
        d.newIssueDiscardAsyncCreateResult = false;
    }
    CancelUnfinishedNewIssueForGridChange(d);

    IssueDraft parsed;
    std::string err;
    if (!IssueDraftHelpers::FromJson(row.Payload, parsed, err)) {
        ArmOfflineQueuePanelStatus(d, std::string("Open as draft: JSON parse failed: ") + err);
        return;
    }
    if (!parsed.ParentKey.empty() && parsed.FieldValues.find("parent") == parsed.FieldValues.end()) {
        parsed.FieldValues["parent"] = parsed.ParentKey;
    }
    parsed.ExistingIssueKey.clear();
    parsed.FieldValues.erase("issuekey");
    parsed.FieldValues.erase("key");
    if (!parsed.IssueTypeId.empty()) {
        parsed.FieldValues["issuetype"] = parsed.IssueTypeId;
    } else if (!parsed.IssueTypeName.empty()) {
        parsed.FieldValues["issuetype"] = parsed.IssueTypeName;
    }
    if (!parsed.ProjectKey.empty()) {
        parsed.FieldValues["project"] = parsed.ProjectKey;
    }

    d.newIssueDraft = std::move(parsed);
    d.newIssueDraftActive = true;
    d.newIssueDraftEditBufs.clear();
    d.newIssueMissingFieldIds.clear();
    d.newIssueCreateInFlight = false;
    d.newIssueDiscardAsyncCreateResult = false;

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Loaded queued offline create (id %lld) into new-issue draft — fix fields, then Create "
                  "or leave queued.",
                  static_cast<long long>(row.Id));
    ArmOfflineQueuePanelStatus(d, buf);
}

static std::string BuildPendingQueueRowTsv(const PendingCreate& row) {
    const std::string createdAt = FormatEpochLocal(row.CreatedAtEpochSec);
    std::string out;
    out.reserve(128 + row.LastError.size() + row.Payload.size());
    out += std::to_string(static_cast<long long>(row.Id));
    out += '\t';
    out += std::to_string(row.Attempts);
    out += '\t';
    out += SanitizeClipboardCell(row.LastError.empty() ? std::string("-") : row.LastError);
    out += '\t';
    out += SanitizeClipboardCell(createdAt);
    out += '\t';
    out += SanitizeClipboardCell(row.Payload);
    return out;
}

enum class UnifiedOfflineKind { PendingCreate, DeadCreate, PendingFieldEdit, DeadFieldEdit };

struct UnifiedOfflineRow {
    std::string key;
    UnifiedOfflineKind kind = UnifiedOfflineKind::PendingCreate;
    std::string state;
    std::string type;
    std::int64_t dbId = 0;
    std::int64_t originalId = 0;
    std::string issue;
    std::string field;
    int attempts = 0;
    std::string terminalReason;
    std::string lastError;
    std::int64_t createdEpoch = 0;
    std::int64_t archivedEpoch = 0;
    std::string payload;
    bool hasMergeConflict = false;
    std::string conflictContextJson;
    /// Backend namespace the row was queued against (multi-grid Slice 1c). Flows from the
    /// PendingCreate / PendingFieldEditRecord / Dead* structs; rows whose backend has no live
    /// context stay queued and this tag attributes them.
    std::string backendKey;
};

static std::string MakeUnifiedOfflineRowKey(UnifiedOfflineKind kind, std::int64_t dbId) {
    switch (kind) {
    case UnifiedOfflineKind::PendingCreate:
        return std::string("PC:") + std::to_string(dbId);
    case UnifiedOfflineKind::DeadCreate:
        return std::string("DC:") + std::to_string(dbId);
    case UnifiedOfflineKind::PendingFieldEdit:
        return std::string("PF:") + std::to_string(dbId);
    case UnifiedOfflineKind::DeadFieldEdit:
        return std::string("DF:") + std::to_string(dbId);
    }
    return {};
}

static std::vector<UnifiedOfflineRow> BuildUnifiedOfflineRows(const std::vector<PendingCreate>& pendingCreates,
                                                              const std::vector<DeadPendingCreate>& deadCreates,
                                                              const std::vector<PendingFieldEditRecord>& pendingEdits,
                                                              const std::vector<DeadPendingFieldEdit>& deadEdits) {
    std::vector<UnifiedOfflineRow> rows;
    rows.reserve(pendingCreates.size() + deadCreates.size() + pendingEdits.size() + deadEdits.size());

    for (const PendingCreate& row : pendingCreates) {
        UnifiedOfflineRow u;
        u.kind = UnifiedOfflineKind::PendingCreate;
        u.key = MakeUnifiedOfflineRowKey(u.kind, row.Id);
        u.state = "Queued";
        u.type = "Issue create";
        u.dbId = row.Id;
        u.originalId = 0;
        u.attempts = row.Attempts;
        u.lastError = row.LastError;
        u.createdEpoch = row.CreatedAtEpochSec;
        u.archivedEpoch = 0;
        u.payload = row.Payload;
        u.backendKey = row.BackendKey;
        rows.push_back(std::move(u));
    }
    for (const DeadPendingCreate& row : deadCreates) {
        UnifiedOfflineRow u;
        u.kind = UnifiedOfflineKind::DeadCreate;
        u.key = MakeUnifiedOfflineRowKey(u.kind, row.DeadId);
        u.state = "Failed";
        u.type = "Issue create";
        u.dbId = row.DeadId;
        u.originalId = row.OriginalId;
        u.attempts = row.Attempts;
        u.terminalReason = row.TerminalReason;
        u.lastError = row.LastError;
        u.createdEpoch = row.CreatedAtEpochSec;
        u.archivedEpoch = row.ArchivedAtEpochSec;
        u.payload = row.Payload;
        u.backendKey = row.BackendKey;
        rows.push_back(std::move(u));
    }
    for (const PendingFieldEditRecord& row : pendingEdits) {
        UnifiedOfflineRow u;
        u.kind = UnifiedOfflineKind::PendingFieldEdit;
        u.key = MakeUnifiedOfflineRowKey(u.kind, row.Id);
        u.state = "Queued";
        u.type = "Field edit";
        u.dbId = row.Id;
        u.originalId = 0;
        u.issue = row.IssueKey;
        u.field = row.FieldId;
        u.attempts = row.Attempts;
        u.lastError = row.LastError;
        u.createdEpoch = row.CreatedAtEpochSec;
        u.archivedEpoch = 0;
        u.payload = row.FieldsPayloadJson;
        u.hasMergeConflict = row.HasMergeConflict;
        u.conflictContextJson = row.ConflictContextJson;
        u.backendKey = row.BackendKey;
        if (row.HasMergeConflict) {
            u.state = "Conflict";
        }
        rows.push_back(std::move(u));
    }
    for (const DeadPendingFieldEdit& row : deadEdits) {
        UnifiedOfflineRow u;
        u.kind = UnifiedOfflineKind::DeadFieldEdit;
        u.key = MakeUnifiedOfflineRowKey(u.kind, row.DeadId);
        u.state = "Failed";
        u.type = "Field edit";
        u.dbId = row.DeadId;
        u.originalId = row.OriginalId;
        u.issue = row.IssueKey;
        u.field = row.FieldId;
        u.attempts = row.Attempts;
        u.terminalReason = row.TerminalReason;
        u.lastError = row.LastError;
        u.createdEpoch = row.CreatedAtEpochSec;
        u.archivedEpoch = row.ArchivedAtEpochSec;
        u.payload = row.FieldsPayloadJson;
        u.backendKey = row.BackendKey;
        rows.push_back(std::move(u));
    }

    auto activityEpoch = [](const UnifiedOfflineRow& r) -> std::int64_t {
        if (r.kind == UnifiedOfflineKind::DeadCreate || r.kind == UnifiedOfflineKind::DeadFieldEdit) {
            return r.archivedEpoch;
        }
        return r.createdEpoch;
    };
    std::sort(rows.begin(), rows.end(), [&](const UnifiedOfflineRow& a, const UnifiedOfflineRow& b) {
        const std::int64_t ta = activityEpoch(a);
        const std::int64_t tb = activityEpoch(b);
        if (ta != tb) {
            return ta > tb;
        }
        if (a.kind != b.kind) {
            return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        }
        return a.dbId > b.dbId;
    });
    return rows;
}

static std::string BuildPendingFieldEditRowTsv(const PendingFieldEditRecord& row) {
    const std::string createdAt = FormatEpochLocal(row.CreatedAtEpochSec);
    std::string out;
    out.reserve(160 + row.LastError.size() + row.FieldsPayloadJson.size());
    out += std::to_string(static_cast<long long>(row.Id));
    out += '\t';
    out += SanitizeClipboardCell(row.IssueKey);
    out += '\t';
    out += SanitizeClipboardCell(row.FieldId);
    out += '\t';
    out += std::to_string(row.Attempts);
    out += '\t';
    out += SanitizeClipboardCell(row.LastError.empty() ? std::string("-") : row.LastError);
    out += '\t';
    out += SanitizeClipboardCell(createdAt);
    out += '\t';
    out += SanitizeClipboardCell(row.FieldsPayloadJson);
    return out;
}

static std::string BuildDeadFieldEditRowTsv(const DeadPendingFieldEdit& row) {
    const std::string createdAt = FormatEpochLocal(row.CreatedAtEpochSec);
    const std::string archivedAt = FormatEpochLocal(row.ArchivedAtEpochSec);
    std::string out;
    out.reserve(200 + row.TerminalReason.size() + row.LastError.size() + row.FieldsPayloadJson.size());
    out += std::to_string(static_cast<long long>(row.DeadId));
    out += '\t';
    out += std::to_string(static_cast<long long>(row.OriginalId));
    out += '\t';
    out += SanitizeClipboardCell(row.IssueKey);
    out += '\t';
    out += SanitizeClipboardCell(row.FieldId);
    out += '\t';
    out += std::to_string(row.Attempts);
    out += '\t';
    out += SanitizeClipboardCell(row.TerminalReason.empty() ? std::string("-") : row.TerminalReason);
    out += '\t';
    out += SanitizeClipboardCell(row.LastError.empty() ? std::string("-") : row.LastError);
    out += '\t';
    out += SanitizeClipboardCell(createdAt);
    out += '\t';
    out += SanitizeClipboardCell(archivedAt);
    out += '\t';
    out += SanitizeClipboardCell(row.FieldsPayloadJson);
    return out;
}

static std::string UnifiedOfflineRowLastErrorDisplay(const UnifiedOfflineRow& row) {
    if (row.kind == UnifiedOfflineKind::DeadCreate) {
        const std::string detail = DeadLetterDetailFromLastError(row.lastError);
        const std::string& show = detail.empty() ? row.lastError : detail;
        return show;
    }
    if (!row.terminalReason.empty()) {
        return row.terminalReason + (row.lastError.empty() ? std::string() : std::string(" — ") + row.lastError);
    }
    return row.lastError;
}

static std::string UnifiedOfflineLastErrorTooltip(const UnifiedOfflineRow& row) {
    if (row.kind == UnifiedOfflineKind::DeadCreate) {
        std::string s;
        if (!row.terminalReason.empty()) {
            s += row.terminalReason;
            s.push_back('\n');
        }
        const std::string action = DeadLetterActionFromLastError(row.lastError);
        const std::string stage = DeadLetterStageFromLastError(row.lastError);
        const std::string detail = DeadLetterDetailFromLastError(row.lastError);
        if (!action.empty()) {
            s += "action=";
            s += action;
            s.push_back('\n');
        }
        if (!stage.empty()) {
            s += "stage=";
            s += stage;
            s.push_back('\n');
        }
        if (!detail.empty()) {
            s += "detail=";
            s += detail;
        } else if (!row.lastError.empty()) {
            if (!s.empty() && s.back() != '\n') {
                s.push_back('\n');
            }
            s += row.lastError;
        }
        return s;
    }
    return UnifiedOfflineRowLastErrorDisplay(row);
}

static PendingCreate UnifiedOfflineToPendingCreate(const UnifiedOfflineRow& u) {
    PendingCreate pc;
    pc.Id = u.dbId;
    pc.Attempts = u.attempts;
    pc.LastError = u.lastError;
    pc.CreatedAtEpochSec = u.createdEpoch;
    pc.Payload = u.payload;
    return pc;
}

static DeadPendingCreate UnifiedOfflineToDeadPendingCreate(const UnifiedOfflineRow& u) {
    DeadPendingCreate dc;
    dc.DeadId = u.dbId;
    dc.OriginalId = u.originalId;
    dc.Attempts = u.attempts;
    dc.LastError = u.lastError;
    dc.CreatedAtEpochSec = u.createdEpoch;
    dc.ArchivedAtEpochSec = u.archivedEpoch;
    dc.TerminalReason = u.terminalReason;
    dc.Payload = u.payload;
    return dc;
}

static PendingFieldEditRecord UnifiedOfflineToPendingFieldEdit(const UnifiedOfflineRow& u) {
    PendingFieldEditRecord pf;
    pf.Id = u.dbId;
    pf.IssueKey = u.issue;
    pf.FieldId = u.field;
    pf.FieldsPayloadJson = u.payload;
    pf.Attempts = u.attempts;
    pf.LastError = u.lastError;
    pf.CreatedAtEpochSec = u.createdEpoch;
    return pf;
}

static DeadPendingFieldEdit UnifiedOfflineToDeadFieldEdit(const UnifiedOfflineRow& u) {
    DeadPendingFieldEdit df;
    df.DeadId = u.dbId;
    df.OriginalId = u.originalId;
    df.IssueKey = u.issue;
    df.FieldId = u.field;
    df.FieldsPayloadJson = u.payload;
    df.Attempts = u.attempts;
    df.LastError = u.lastError;
    df.CreatedAtEpochSec = u.createdEpoch;
    df.ArchivedAtEpochSec = u.archivedEpoch;
    df.TerminalReason = u.terminalReason;
    return df;
}

static std::string UnifiedOfflineRowClipboardLine(const UnifiedOfflineRow& u) {
    switch (u.kind) {
    case UnifiedOfflineKind::PendingCreate:
        return BuildPendingQueueRowTsv(UnifiedOfflineToPendingCreate(u));
    case UnifiedOfflineKind::DeadCreate:
        return BuildDeadLetterRowTsv(UnifiedOfflineToDeadPendingCreate(u));
    case UnifiedOfflineKind::PendingFieldEdit:
        return BuildPendingFieldEditRowTsv(UnifiedOfflineToPendingFieldEdit(u));
    case UnifiedOfflineKind::DeadFieldEdit:
        return BuildDeadFieldEditRowTsv(UnifiedOfflineToDeadFieldEdit(u));
    }
    return {};
}

// Persistent selection set for the unified offline-queue table. Single-instance panel,
// so a file-scope static is behaviour-identical to the former in-function static local.
static std::unordered_set<std::string> g_selectedOfflineRowKeys;

struct OfflineDrawCtx {
    AppController& app;
    UiDrawSession& d;
    std::vector<UnifiedOfflineRow>& rows;
    std::string hoveredKey;
};

struct OfflineQueueData {
    std::vector<PendingCreate> pendingCreates;
    std::vector<DeadPendingCreate> deadCreates;
    std::vector<PendingFieldEditRecord> pendingEdits;
    std::vector<DeadPendingFieldEdit> deadEdits;
    size_t total = 0;
};

static OfflineQueueData FetchOfflineQueueData(AppController& app) {
    OfflineQueueData data;
    data.pendingCreates = app.GetPendingCreates();
    data.deadCreates = app.GetDeadPendingCreates();
    data.pendingEdits = app.GetPendingFieldEdits();
    data.deadEdits = app.GetDeadPendingFieldEdits();
    data.total =
        data.pendingCreates.size() + data.deadCreates.size() + data.pendingEdits.size() + data.deadEdits.size();
    return data;
}

static void PruneOfflineSelectionToLiveRows(const std::vector<UnifiedOfflineRow>& rows) {
    std::unordered_set<std::string> liveKeys;
    liveKeys.reserve(rows.size());
    for (const UnifiedOfflineRow& r : rows) {
        liveKeys.insert(r.key);
    }
    for (auto it = g_selectedOfflineRowKeys.begin(); it != g_selectedOfflineRowKeys.end();) {
        if (liveKeys.count(*it) == 0) {
            it = g_selectedOfflineRowKeys.erase(it);
        } else {
            ++it;
        }
    }
}

static void ExpireOfflinePanelStatus(UiDrawSession& d) {
    const auto now = std::chrono::steady_clock::now();
    if (d.offlineQueuePanelStatusHasClearDeadline && now >= d.offlineQueuePanelStatusClearAt) {
        d.offlineQueuePanelStatus.clear();
        d.offlineQueuePanelStatusHasClearDeadline = false;
    }
    if (d.deadLetterPanelStatusHasClearDeadline && now >= d.deadLetterPanelStatusClearAt) {
        d.deadLetterPanelStatus.clear();
        d.deadLetterPanelStatusHasClearDeadline = false;
    }
}

// Deletes a single unified row from whichever backing table owns it; returns Deleted-count > 0.
static bool DeleteOfflineRowFromDb(AppController& app, const UnifiedOfflineRow& r) {
    switch (r.kind) {
    case UnifiedOfflineKind::PendingCreate:
        return app.DeletePendingCreates({r.dbId}).Deleted > 0;
    case UnifiedOfflineKind::DeadCreate:
        return app.DeleteDeadPendingCreates({r.dbId}).Deleted > 0;
    case UnifiedOfflineKind::PendingFieldEdit:
        return app.DeletePendingFieldEdits({r.dbId}).Deleted > 0;
    case UnifiedOfflineKind::DeadFieldEdit:
        return app.DeleteDeadPendingFieldEdits({r.dbId}).Deleted > 0;
    }
    return false;
}

// Returns true and copies to the clipboard when at least one row was serialized.
static bool CopyOfflineSelectionToClipboard(OfflineDrawCtx& ctx, const std::string& fallbackKey) {
    std::string out;
    if (!g_selectedOfflineRowKeys.empty()) {
        for (const UnifiedOfflineRow& r : ctx.rows) {
            if (g_selectedOfflineRowKeys.count(r.key) == 0) {
                continue;
            }
            if (!out.empty()) {
                out.push_back('\n');
            }
            out += UnifiedOfflineRowClipboardLine(r);
        }
    } else if (!fallbackKey.empty()) {
        auto rIt = std::find_if(ctx.rows.begin(), ctx.rows.end(),
                                [&](const UnifiedOfflineRow& r) { return r.key == fallbackKey; });
        if (rIt != ctx.rows.end()) {
            out = UnifiedOfflineRowClipboardLine(*rIt);
        }
    }
    if (out.empty()) {
        return false;
    }
    ImGui::SetClipboardText(out.c_str());
    return true;
}

static void DrawOfflineQueueHeader(OfflineDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    ImGui::SeparatorText("Offline Queue");

    if (!d.offlineQueuePanelStatus.empty()) {
        ImGui::TextWrapped("%s", d.offlineQueuePanelStatus.c_str());
    }
    if (!d.deadLetterPanelStatus.empty()) {
        ImGui::TextWrapped("%s", d.deadLetterPanelStatus.c_str());
    }

    ImGui::TextDisabled(
        "Queued rows post automatically when the tracker is reachable. Failed rows stopped retrying "
        "(too many attempts or a validation error) until you retry them. Discard only deletes the local copy.");
}

// Discard-confirm staging (P2-M1): queued creates/edits are the ONLY copy of
// offline-authored work, so a discard whose target set contains pending rows is parked
// here and executed from the panel-level confirm modal instead of running on the click.
static std::vector<std::string> g_offlineDiscardConfirmKeys;
static bool g_offlineDiscardConfirmRequested = false;

static bool OfflineKeysContainPendingWork(const OfflineDrawCtx& ctx, const std::vector<std::string>& keys) {
    for (const UnifiedOfflineRow& r : ctx.rows) {
        if ((r.kind == UnifiedOfflineKind::PendingCreate || r.kind == UnifiedOfflineKind::PendingFieldEdit) &&
            std::find(keys.begin(), keys.end(), r.key) != keys.end()) {
            return true;
        }
    }
    return false;
}

static void DiscardOfflineRowKeys(OfflineDrawCtx& ctx, const std::vector<std::string>& keys) {
    int disc = 0;
    int fail = 0;
    for (const std::string& key : keys) {
        auto rIt =
            std::find_if(ctx.rows.begin(), ctx.rows.end(), [&](const UnifiedOfflineRow& r) { return r.key == key; });
        if (rIt != ctx.rows.end()) {
            if (DeleteOfflineRowFromDb(ctx.app, *rIt)) {
                ++disc;
            } else {
                ++fail;
            }
        }
        g_selectedOfflineRowKeys.erase(key);
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Discarded %d row(s) (couldn't delete %d).", disc, fail);
    ArmOfflineQueuePanelStatus(ctx.d, buf);
}

// Routes a discard request: straight through for archived (dead) rows, via the confirm
// modal when any target row is still queued to replay (P2-M1).
static void RequestOfflineDiscard(OfflineDrawCtx& ctx, const std::vector<std::string>& keys) {
    if (keys.empty()) {
        return;
    }
    if (OfflineKeysContainPendingWork(ctx, keys)) {
        g_offlineDiscardConfirmKeys = keys;
        g_offlineDiscardConfirmRequested = true;
        return;
    }
    DiscardOfflineRowKeys(ctx, keys);
}

static void OnDiscardSelected(OfflineDrawCtx& ctx) {
    RequestOfflineDiscard(ctx,
                          std::vector<std::string>(g_selectedOfflineRowKeys.begin(), g_selectedOfflineRowKeys.end()));
}

static void OnClearArchivedDead(OfflineDrawCtx& ctx) {
    int del = 0;
    int fail = 0;
    for (const UnifiedOfflineRow& r : ctx.rows) {
        if (r.kind == UnifiedOfflineKind::DeadCreate || r.kind == UnifiedOfflineKind::DeadFieldEdit) {
            if (DeleteOfflineRowFromDb(ctx.app, r)) {
                ++del;
            } else {
                ++fail;
            }
        }
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Cleared failed rows: deleted %d (couldn't delete %d).", del, fail);
    ArmDeadLetterPanelStatus(ctx.d, buf);
}

static void DrawOfflineQueueToolbar(OfflineDrawCtx& ctx) {
    if (ImGui::Button("Copy selected##unifiedoff")) {
        CopyOfflineSelectionToClipboard(ctx, std::string());
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard selected##unifiedoff")) {
        OnDiscardSelected(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button("Retry creates now##unifiedoff")) {
        ctx.d.offlineQueuePanelStatus = "Triggering manual retry scan of pending Creates...";
        ctx.d.offlineQueuePanelStatusHasClearDeadline = false;
        ctx.app.TickOfflineCreates();
        ctx.app.TickOfflineFieldEdits();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear failed rows##unifiedoff")) {
        OnClearArchivedDead(ctx);
    }
}

enum OfflineCol {
    Col_Select = 0,
    Col_Action,
    Col_State,
    Col_Id,
    Col_OriginalId,
    Col_Issue,
    Col_Field,
    Col_Retries,
    Col_LastError,
    Col_ActivityTime,
    Col_Payload,
    Col_COUNT
};

static void DrawOfflineRowActionCell(const UnifiedOfflineRow& row) {
    if (row.kind == UnifiedOfflineKind::PendingCreate) {
        ImGui::TextDisabled("Queued create");
    } else if (row.kind == UnifiedOfflineKind::DeadCreate) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextUnformatted("Failed create - won't retry automatically");
        ImGui::PopStyleColor();
        // remove-global-project-key.md: surface a small badge for
        // rows the legacy-project sweep dead-lettered, so the user knows to restore
        // and pick a project rather than mistaking it for a transport failure.
        if (row.terminalReason == "legacy_missing_project") {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextUnformatted(SmatchetLocalization::T("offlineQueue.badge.missingProject", "missing project"));
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "%s", SmatchetLocalization::T("offlineQueue.badge.missingProject.tooltip",
                                                  "This queued create was missing a project after the "
                                                  "project-key migration. Restore and pick a project to retry."));
            }
        }
    } else if (row.kind == UnifiedOfflineKind::PendingFieldEdit) {
        if (row.hasMergeConflict) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.1f, 1.0f));
            ImGui::TextUnformatted("Conflict — edit suspended");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Queued edit");
        }
    } else if (row.kind == UnifiedOfflineKind::DeadFieldEdit) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextUnformatted("Failed edit - won't retry automatically");
        ImGui::PopStyleColor();
    }
}

// Builds the set of rows the row-context-menu acts on: the current selection if the
// right-clicked row is part of it, otherwise just the right-clicked row.
static std::vector<UnifiedOfflineRow> CollectOfflineContextPicks(const std::vector<UnifiedOfflineRow>& rows,
                                                                 const UnifiedOfflineRow& row) {
    std::vector<UnifiedOfflineRow> picks;
    if (g_selectedOfflineRowKeys.count(row.key) > 0) {
        std::copy_if(rows.begin(), rows.end(), std::back_inserter(picks),
                     [](const UnifiedOfflineRow& r) { return g_selectedOfflineRowKeys.count(r.key) > 0; });
    } else {
        picks.push_back(row);
    }
    return picks;
}

static void OnContextCopyTsv(const std::vector<UnifiedOfflineRow>& picks) {
    std::string out;
    for (const auto& p : picks) {
        if (!out.empty()) {
            out.push_back('\n');
        }
        out += UnifiedOfflineRowClipboardLine(p);
    }
    if (!out.empty()) {
        ImGui::SetClipboardText(out.c_str());
    }
}

static void OnContextOpenDraft(UiDrawSession& d, const std::vector<UnifiedOfflineRow>& picks) {
    const UnifiedOfflineRow* pick = nullptr;
    for (const auto& p : picks) {
        if (p.kind == UnifiedOfflineKind::PendingCreate || p.kind == UnifiedOfflineKind::DeadCreate) {
            pick = &p;
        }
    }
    if (pick) {
        if (pick->kind == UnifiedOfflineKind::PendingCreate) {
            OpenPendingCreateRowAsNewIssueDraft(d, UnifiedOfflineToPendingCreate(*pick));
        } else {
            OpenDeadLetterRowAsNewIssueDraft(d, UnifiedOfflineToDeadPendingCreate(*pick));
        }
    }
}

static void OnContextDiscard(OfflineDrawCtx& ctx, const std::vector<UnifiedOfflineRow>& picks) {
    std::vector<std::string> keys;
    keys.reserve(picks.size());
    for (const auto& p : picks) {
        keys.push_back(p.key);
    }
    RequestOfflineDiscard(ctx, keys);
}

static void OnContextRestoreDeadCreates(OfflineDrawCtx& ctx, const std::vector<UnifiedOfflineRow>& picks) {
    // Key-preserving restore (CR-951-1): route through AppController::RestoreDeadPendingCreates
    // so the row keeps its ORIGINAL backend_key — re-queueing via QueueCreateOffline would
    // re-stamp the focused context's key. The fresh-create scrub (ExistingIssueKey + issuekey/
    // key field values) lives inside LocalCacheManager::RestoreDeadPendingCreate's transaction.
    std::vector<std::int64_t> originalIds;
    for (const auto& p : picks) {
        if (p.kind != UnifiedOfflineKind::DeadCreate) {
            continue;
        }
        originalIds.push_back(p.originalId);
        g_selectedOfflineRowKeys.erase(p.key);
    }
    const AppController::DeadLetterRestoreSummary summary = ctx.app.RestoreDeadPendingCreates(originalIds);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Restored dead creates to offline queue: %d retrying, %d failed.", summary.Restored,
                  summary.Failed);
    ArmOfflineQueuePanelStatus(ctx.d, buf);
}

static void OnContextRestoreDeadEdits(OfflineDrawCtx& ctx, const std::vector<UnifiedOfflineRow>& picks) {
    // Key-preserving restore (CR-951-1): the field-edit twin — QueueFieldEditOffline would
    // re-stamp the focused context's key AND drop the merge bases; the dedicated restore
    // keeps both.
    std::vector<std::int64_t> originalIds;
    for (const auto& p : picks) {
        if (p.kind != UnifiedOfflineKind::DeadFieldEdit) {
            continue;
        }
        originalIds.push_back(p.originalId);
        g_selectedOfflineRowKeys.erase(p.key);
    }
    const AppController::DeadFieldEditRestoreSummary summary = ctx.app.RestoreDeadPendingFieldEdits(originalIds);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Restored dead edits to offline queue: %d retrying, %d failed.", summary.Restored,
                  summary.Failed);
    ArmOfflineQueuePanelStatus(ctx.d, buf);
}

static void DrawOfflineRowContextMenu(OfflineDrawCtx& ctx, const UnifiedOfflineRow& row) {
    UiDrawSession& d = ctx.d;
    std::vector<UnifiedOfflineRow> picks = CollectOfflineContextPicks(ctx.rows, row);

    if (ImGui::MenuItem("Copy fields to clipboard (TSV)")) {
        OnContextCopyTsv(picks);
    }

    const bool hasCreates = std::any_of(picks.begin(), picks.end(), [](const UnifiedOfflineRow& p) {
        return p.kind == UnifiedOfflineKind::PendingCreate || p.kind == UnifiedOfflineKind::DeadCreate;
    });
    if (hasCreates) {
        std::string act = (picks.size() == 1) ? "Open draft Issue Editor..." : "Open latest draft...";
        if (ImGui::MenuItem(act.c_str())) {
            OnContextOpenDraft(d, picks);
        }
    }

    // Conflict resolution — only for a single pending field edit with a conflict.
    const bool singleConflict =
        (picks.size() == 1 && picks[0].kind == UnifiedOfflineKind::PendingFieldEdit && picks[0].hasMergeConflict);
    if (singleConflict && ImGui::MenuItem("Resolve merge conflict...")) {
        d.conflictResolveDbId = picks[0].dbId;
        d.conflictContextJson = picks[0].conflictContextJson;
        d.showConflictResolveModal = true;
    }

    if (ImGui::MenuItem("Discard selected row(s)")) {
        OnContextDiscard(ctx, picks);
    }

    const bool hasDeadCreates = std::any_of(picks.begin(), picks.end(), [](const UnifiedOfflineRow& p) {
        return p.kind == UnifiedOfflineKind::DeadCreate;
    });
    if (hasDeadCreates && ImGui::MenuItem("Retry failed create(s)")) {
        OnContextRestoreDeadCreates(ctx, picks);
    }

    const bool hasDeadEdits = std::any_of(picks.begin(), picks.end(), [](const UnifiedOfflineRow& p) {
        return p.kind == UnifiedOfflineKind::DeadFieldEdit;
    });
    if (hasDeadEdits && ImGui::MenuItem("Retry failed edit(s)")) {
        OnContextRestoreDeadEdits(ctx, picks);
    }
}

static void DrawOfflineRowStateCell(OfflineDrawCtx& ctx, const UnifiedOfflineRow& row) {
    UiDrawSession& d = ctx.d;
    ImGui::Selectable(row.state.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
    if (ImGui::IsItemHovered()) {
        ctx.hoveredKey = row.key;
        if (ImGui::IsMouseDoubleClicked(0) && row.hasMergeConflict &&
            row.kind == UnifiedOfflineKind::PendingFieldEdit) {
            d.conflictResolveDbId = row.dbId;
            d.conflictContextJson = row.conflictContextJson;
            d.showConflictResolveModal = true;
        }
    }
    const bool rightClicked = ImGui::BeginPopupContextItem("offRowCtx", ImGuiPopupFlags_MouseButtonRight);
    if (rightClicked) {
        ctx.hoveredKey = row.key;
        DrawOfflineRowContextMenu(ctx, row);
        ImGui::EndPopup();
    }
}

static void DrawOfflineRowPayloadTooltip(const UnifiedOfflineRow& row) {
    std::string md;
    try {
        std::string parseErr;
        const nlohmann::json j = smatchet::json_safe::ParseBounded(row.payload, parseErr);
        if (parseErr.empty() && j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                const auto& val = it.value();
                if (val.is_object() && val.value("type", std::string()) == "doc") {
                    md = MarkdownConvert::AdfToMarkdown(val);
                    break;
                }
                if (val.is_string()) {
                    bool fell = false;
                    md = MarkdownConvert::HtmlSubsetToMarkdown(val.get<std::string>(), &fell);
                    if (fell)
                        md = val.get<std::string>();
                    break;
                }
            }
        }
    } catch (...) { // catch-all-ok: JSON parse on pending-queue metadata
    }
    if (md.empty())
        md = BuildPayloadPreview(row.payload, 600);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
    MarkdownPreviewRender::Options opts;
    opts.mode = MarkdownPreviewRender::Mode::Tooltip;
    opts.clickableLinks = false;
    opts.wrapWidth = ImGui::GetFontSize() * 48.0f;
    MarkdownPreviewRender::Render(md, opts);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

static void DrawOfflineQueueTableRow(OfflineDrawCtx& ctx, const UnifiedOfflineRow& row) {
    std::unordered_set<std::string>& selectedOfflineRowKeys = g_selectedOfflineRowKeys;

    if (ImGui::TableSetColumnIndex(Col_Select)) {
        bool sel = selectedOfflineRowKeys.count(row.key) > 0;
        if (ImGui::Checkbox("##check", &sel)) {
            if (sel) {
                selectedOfflineRowKeys.insert(row.key);
            } else {
                selectedOfflineRowKeys.erase(row.key);
            }
        }
        if (ImGui::IsItemHovered()) {
            ctx.hoveredKey = row.key;
        }
    }

    ImGui::TableSetColumnIndex(Col_Action);
    DrawOfflineRowActionCell(row);

    ImGui::TableSetColumnIndex(Col_State);
    DrawOfflineRowStateCell(ctx, row);

    ImGui::TableSetColumnIndex(Col_Id);
    ImGui::Text("%lld", static_cast<long long>(row.dbId));

    ImGui::TableSetColumnIndex(Col_OriginalId);
    if (row.originalId != 0) {
        ImGui::Text("%lld", static_cast<long long>(row.originalId));
    } else {
        ImGui::TextUnformatted("-");
    }
    ImGui::TableSetColumnIndex(Col_Issue);
    ImGui::TextUnformatted(row.issue.empty() ? "-" : row.issue.c_str());
    ImGui::TableSetColumnIndex(Col_Field);
    ImGui::TextUnformatted(row.field.empty() ? "-" : row.field.c_str());
    ImGui::TableSetColumnIndex(Col_Retries);
    ImGui::Text("%d", row.attempts);

    ImGui::TableSetColumnIndex(Col_LastError);
    {
        const std::string errShow = UnifiedOfflineRowLastErrorDisplay(row);
        const std::string errPreview = BuildPayloadPreview(errShow, 120);
        ImGui::TextUnformatted(errPreview.empty() ? "-" : errPreview.c_str());
        const std::string tip = UnifiedOfflineLastErrorTooltip(row);
        if (!tip.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }

    ImGui::TableSetColumnIndex(Col_ActivityTime);
    {
        const std::int64_t activityEpoch = row.archivedEpoch != 0 ? row.archivedEpoch : row.createdEpoch;
        ImGui::TextUnformatted(FormatEpochLocal(activityEpoch).c_str());
    }
    ImGui::TableSetColumnIndex(Col_Payload);
    {
        const std::string payloadPreview = BuildPayloadPreview(row.payload, 140);
        ImGui::TextUnformatted(payloadPreview.empty() ? "-" : payloadPreview.c_str());
        if (!row.payload.empty() && ImGui::IsItemHovered()) {
            DrawOfflineRowPayloadTooltip(row);
        }
    }
}

static void DrawOfflineQueueTable(OfflineDrawCtx& ctx) {
    std::vector<UnifiedOfflineRow>& rows = ctx.rows;

    const float tblH = OfflineAuxTableOuterHeight(rows.size());
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("offlineQueueTbl", Col_COUNT, flags, ImVec2(0.0f, tblH))) {
        ImGui::TableSetupColumn("Select", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Original Id", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Issue", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Retries", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Last Error Reason", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Activity Time", ImGuiTableColumnFlags_WidthFixed, 135.0f);
        ImGui::TableSetupColumn("Payload Preview", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();

        for (size_t ri = 0; ri < rows.size(); ++ri) {
            const UnifiedOfflineRow& row = rows[ri];
            ImGui::PushID(static_cast<int>(ri));
            ImGui::TableNextRow();
            DrawOfflineQueueTableRow(ctx, row);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static void HandleOfflineCopyShortcut(OfflineDrawCtx& ctx) {
    const bool copyShortcut = ImGuiEffectiveKeyCtrl() && ImGui::IsKeyPressed(ImGuiKey_C, false);
    if (copyShortcut && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        CopyOfflineSelectionToClipboard(ctx, ctx.hoveredKey);
    }
}

// Parsed view of conflict_context_json. `Kind` (text|scalar|unverified, ADR-0016) selects the
// modal branch; absent kind defaults to "text" for legacy rich rows. `Valid` is true ONLY when the
// JSON parsed as an object AND the resolved kind is one of the three known kinds — malformed JSON
// or an unknown kind yields Valid=false so the modal renders a NON-actionable read-only state
// with no Use Mine/Theirs/Force/Save buttons, never a resolvable text pane that could clobber the
// row with empty content. Pillar 3 graceful degradation rather than a destructive resolve.
struct ConflictModalCtx {
    std::string Kind = "text";
    std::string Mine;
    std::string Theirs;
    std::string Base;
    std::string RichKind = "adf";
    bool Valid = false;
};

static ConflictModalCtx ParseConflictModalCtx(const std::string& json) {
    ConflictModalCtx out;
    try {
        std::string parseErr;
        const nlohmann::json ctx = smatchet::json_safe::ParseBounded(json, parseErr);
        if (parseErr.empty() && ctx.is_object()) {
            out.Kind = ctx.value("kind", std::string("text"));
            out.Mine = ctx.value("mine", std::string());
            out.Theirs = ctx.value("theirs", std::string());
            out.Base = ctx.value("base", std::string());
            out.RichKind = ctx.value("richKind", std::string("adf"));
            out.Valid = (out.Kind == "text" || out.Kind == "scalar" || out.Kind == "unverified");
        }
    } catch (...) { // catch-all-ok: malformed conflict context → Valid stays false (safe read-only render)
    }
    return out;
}

// Clears modal state + closes the popup after a resolution / discard action.
static void FinishConflictModal(UiDrawSession& d, const char* statusMsg) {
    d.conflictResolveBuf.clear();
    d.conflictContextJson.clear();
    d.conflictResolveDbId = 0;
    ImGui::CloseCurrentPopup();
    if (statusMsg) {
        ArmOfflineQueuePanelStatus(d, statusMsg);
    }
}

// Rich-text 3-way merge pane (legacy `kind:"text"`) — unchanged behaviour. Uses the orthogonal
// `richKind` (adf|html) for reconversion at resolve.
static void DrawConflictPaneText(OfflineDrawCtx& octx, const ConflictModalCtx& cc) {
    AppController& app = octx.app;
    UiDrawSession& d = octx.d;

    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    const float halfW = vp.x * 0.28f;
    const float paneH = vp.y * 0.22f;
    const float resolvedH = vp.y * 0.22f;

    ImGui::TextDisabled("Offline edit conflict — both you and the server changed this field concurrently.");
    ImGui::Spacing();

    ImGui::BeginGroup();
    ImGui::Text("Your edit (mine)");
    ImGui::InputTextMultiline("##CRMine", const_cast<char*>(cc.Mine.c_str()), cc.Mine.size() + 1, ImVec2(halfW, paneH),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("Server version (theirs)");
    ImGui::InputTextMultiline("##CRTheirs", const_cast<char*>(cc.Theirs.c_str()), cc.Theirs.size() + 1,
                              ImVec2(halfW, paneH), ImGuiInputTextFlags_ReadOnly);
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Text("Resolved (edit to remove conflict markers):");
    if (d.conflictResolveBuf.empty()) {
        d.conflictResolveBuf.assign(64 * 1024, '\0');
    }
    ImGui::InputTextMultiline("##CRResolved", d.conflictResolveBuf.data(), d.conflictResolveBuf.size(),
                              ImVec2(-FLT_MIN, resolvedH), ImGuiInputTextFlags_AllowTabInput);

    const bool hasConflictMarkers = std::string(d.conflictResolveBuf.data()).find("<<<<<<<") != std::string::npos;
    if (hasConflictMarkers) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.1f, 1.0f));
        ImGui::TextUnformatted("Resolve all <<<<<<< markers before saving.");
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    auto doResolve = [&](const std::string& resolvedMd) {
        app.ResolveFieldEditConflict(d.conflictResolveDbId, resolvedMd, cc.RichKind, "text");
        FinishConflictModal(d, "Conflict resolved — edit re-queued for replay.");
    };

    if (ImGui::Button("Use Mine", ImVec2(110, 0))) {
        doResolve(cc.Mine);
    }
    ImGui::SameLine();
    if (ImGui::Button("Use Theirs", ImVec2(110, 0))) {
        doResolve(cc.Theirs);
    }
    ImGui::SameLine();
    const bool saveEnabled = !hasConflictMarkers;
    if (!saveEnabled)
        ImGui::BeginDisabled();
    if (ImGui::Button("Save resolved", ImVec2(130, 0))) {
        doResolve(std::string(d.conflictResolveBuf.data()));
    }
    if (!saveEnabled)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        d.conflictResolveBuf.clear();
        ImGui::CloseCurrentPopup();
    }
}

// Scalar conflict pane (`kind:"scalar"`): mine vs theirs + an editable value. Use Mine / Use
// Theirs / Save edited. Writes the chosen value verbatim (no Markdown reconversion).
static void DrawConflictPaneScalar(OfflineDrawCtx& octx, const ConflictModalCtx& cc) {
    AppController& app = octx.app;
    UiDrawSession& d = octx.d;

    const ImVec2 vp = ImGui::GetMainViewport()->Size;
    const float halfW = vp.x * 0.28f;

    ImGui::TextDisabled("Offline edit conflict — the server value changed since you edited this field.");
    ImGui::Spacing();

    ImGui::BeginGroup();
    ImGui::Text("Your value (mine)");
    ImGui::InputText("##SCMine", const_cast<char*>(cc.Mine.c_str()), cc.Mine.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, halfW * 0.05f);
    ImGui::BeginGroup();
    ImGui::Text("Server value (theirs)");
    ImGui::InputText("##SCTheirs", const_cast<char*>(cc.Theirs.c_str()), cc.Theirs.size() + 1,
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Text("Resolved value (edit, or pick a side below):");
    if (d.conflictResolveBuf.empty()) {
        d.conflictResolveBuf.assign(8 * 1024, '\0');
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##SCResolved", d.conflictResolveBuf.data(), d.conflictResolveBuf.size());
    ImGui::Spacing();

    auto doResolve = [&](const std::string& chosen) {
        app.ResolveFieldEditConflict(d.conflictResolveDbId, chosen, std::string(), "scalar");
        FinishConflictModal(d, "Conflict resolved — edit re-queued for replay.");
    };

    if (ImGui::Button("Use Mine", ImVec2(110, 0))) {
        doResolve(cc.Mine);
    }
    ImGui::SameLine();
    if (ImGui::Button("Use Theirs", ImVec2(110, 0))) {
        doResolve(cc.Theirs);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save value", ImVec2(130, 0))) {
        doResolve(std::string(d.conflictResolveBuf.data()));
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        d.conflictResolveBuf.clear();
        ImGui::CloseCurrentPopup();
    }
}

// Unverified pane for the `unverified` kind, where the server value could not be read. The
// Force Mine button replays the queued edit as-is; the Discard button hard-deletes the queue
// row and writes an audit entry.
static void DrawConflictPaneUnverified(OfflineDrawCtx& octx, const ConflictModalCtx& cc) {
    AppController& app = octx.app;
    UiDrawSession& d = octx.d;

    ImGui::TextDisabled("The server value for this field couldn't be read, so we can't tell whether it changed.");
    ImGui::Spacing();
    ImGui::Text("Your value (mine)");
    ImGui::InputText("##UVMine", const_cast<char*>(cc.Mine.c_str()), cc.Mine.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::Spacing();

    if (ImGui::Button("Force Mine", ImVec2(130, 0))) {
        // Replay the queued payload verbatim (no value change) and clear the conflict.
        app.ResolveFieldEditConflict(d.conflictResolveDbId, cc.Mine, std::string(), "unverified");
        FinishConflictModal(d, "Forcing your edit — re-queued for replay.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard my edit", ImVec2(150, 0))) {
        // Hard-delete the queue row; DeletePendingFieldEdits writes the audit entry (ADR-0016 —
        // dead-letter stays failures-only; the audit log carries discard traceability).
        app.DeletePendingFieldEdits({d.conflictResolveDbId});
        FinishConflictModal(d, "Edit discarded.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        d.conflictResolveBuf.clear();
        ImGui::CloseCurrentPopup();
    }
}

// Non-actionable pane for a malformed / unknown conflict_context_json (Valid==false). Renders a
// read-only explanation and offers ONLY Close + a hard Discard — never Use Mine/Theirs/Force/Save,
// which on a corrupt context could resolve the row to empty content and clobber the user's edit.
static void DrawConflictPaneUnknown(OfflineDrawCtx& octx) {
    UiDrawSession& d = octx.d;
    AppController& app = octx.app;

    ImGui::TextDisabled("Conflict details could not be read.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Close keeps the edit queued; Discard drops it.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("offline.conflict_unknown.help",
                               "This offline edit's conflict details could not be read (corrupt or stale data). "
                               "To avoid overwriting either version with empty content, this conflict can't be "
                               "resolved automatically. Close to leave the edit queued, or discard it to drop the "
                               "queued change.");
    ImGui::Spacing();

    if (ImGui::Button("Discard my edit", ImVec2(150, 0))) {
        // Hard-delete the queue row; DeletePendingFieldEdits writes the audit entry (ADR-0016).
        app.DeletePendingFieldEdits({d.conflictResolveDbId});
        FinishConflictModal(d, "Edit discarded.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(80, 0))) {
        d.conflictResolveBuf.clear();
        ImGui::CloseCurrentPopup();
    }
}

static void DrawOfflineConflictModal(OfflineDrawCtx& octx) {
    UiDrawSession& d = octx.d;

    // Merge-conflict resolution modal (PR-F / ADR-0016)
    if (d.showConflictResolveModal) {
        // Open + size the popup once per trigger.
        ImGui::OpenPopup("ResolveMergeConflict");
        d.showConflictResolveModal = false;

        // Seed the resolved buffer from the conflict context on open. Rich `text` seeds the
        // conflict-marker template; scalar seeds the editable value with "mine".
        const ConflictModalCtx seedCtx = ParseConflictModalCtx(d.conflictContextJson);
        if (!seedCtx.Valid) {
            // Malformed / unknown kind → non-actionable pane; no editable buffer to seed.
            d.conflictResolveBuf.clear();
        } else if (seedCtx.Kind == "scalar") {
            d.conflictResolveBuf.assign(8 * 1024, '\0');
            const size_t n = (std::min)(seedCtx.Mine.size(), static_cast<size_t>(8 * 1024 - 1));
            std::memcpy(d.conflictResolveBuf.data(), seedCtx.Mine.data(), n);
        } else if (seedCtx.Kind == "unverified") {
            d.conflictResolveBuf.clear();
        } else {
            const std::string seed =
                "<<<<<<< mine\n" + seedCtx.Mine + "\n=======\n" + seedCtx.Theirs + "\n>>>>>>> theirs";
            d.conflictResolveBuf.assign(64 * 1024, '\0');
            const size_t n = (std::min)(seed.size(), static_cast<size_t>(64 * 1024 - 1));
            std::memcpy(d.conflictResolveBuf.data(), seed.data(), n);
        }
    }

    if (ImGui::BeginPopupModal("ResolveMergeConflict", nullptr,
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        const ConflictModalCtx cc = ParseConflictModalCtx(d.conflictContextJson);
        if (!cc.Valid) {
            DrawConflictPaneUnknown(octx);
        } else if (cc.Kind == "scalar") {
            DrawConflictPaneScalar(octx, cc);
        } else if (cc.Kind == "unverified") {
            DrawConflictPaneUnverified(octx, cc);
        } else {
            DrawConflictPaneText(octx, cc);
        }
        ImGui::EndPopup();
    } else if (!d.conflictResolveBuf.empty() && d.conflictResolveDbId == 0) {
        d.conflictResolveBuf.clear();
    }
}

} // namespace

// P2-M1 confirm modal: opened from RequestOfflineDiscard when the discard set contains
// rows still queued to replay. Panel-level (outside the row context popup) so the modal
// survives the context menu closing on the click.
static void DrawOfflineDiscardConfirmModal(OfflineDrawCtx& ctx) {
    if (g_offlineDiscardConfirmRequested) {
        ImGui::OpenPopup("Discard queued work?###OfflineDiscardConfirm");
        g_offlineDiscardConfirmRequested = false;
    }
    if (!ImGui::BeginPopupModal("Discard queued work?###OfflineDiscardConfirm", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextWrapped("%s", SmatchetLocalization::Format(
                                 "offline.discard_confirm",
                                 "%d row(s) are still queued to replay to the tracker - they are the only copy "
                                 "of that offline work. Discard deletes them permanently.",
                                 static_cast<int>(g_offlineDiscardConfirmKeys.size())));
    ImGui::Spacing();
    if (ImGui::Button("Discard rows")) {
        DiscardOfflineRowKeys(ctx, g_offlineDiscardConfirmKeys);
        g_offlineDiscardConfirmKeys.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep")) {
        g_offlineDiscardConfirmKeys.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool DrawUnifiedOfflineQueuesPanel(AppController& app, UiDrawSession& d) {
    const OfflineQueueData data = FetchOfflineQueueData(app);
    if (data.total == 0) {
        return false;
    }

    std::vector<UnifiedOfflineRow> rows =
        BuildUnifiedOfflineRows(data.pendingCreates, data.deadCreates, data.pendingEdits, data.deadEdits);
    PruneOfflineSelectionToLiveRows(rows);
    ExpireOfflinePanelStatus(d);

    OfflineDrawCtx ctx{app, d, rows, std::string()};

    ImGui::PushID("unifiedOfflineQueues");
    DrawOfflineQueueHeader(ctx);
    DrawOfflineQueueToolbar(ctx);
    DrawOfflineQueueTable(ctx);
    HandleOfflineCopyShortcut(ctx);
    ImGui::PopID();

    DrawOfflineConflictModal(ctx);
    DrawOfflineDiscardConfirmModal(ctx);
    return true;
}
