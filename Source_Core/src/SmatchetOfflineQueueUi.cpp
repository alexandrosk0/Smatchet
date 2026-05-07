#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "TrackerHttpUtils.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h"
#include "StringUtil.h"

#include "imgui.h"
#include "imgui_internal.h"

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
        rows.push_back(std::move(u));
    }
    for (const DeadPendingCreate& row : deadCreates) {
        UnifiedOfflineRow u;
        u.kind = UnifiedOfflineKind::DeadCreate;
        u.key = MakeUnifiedOfflineRowKey(u.kind, row.DeadId);
        u.state = "Dead";
        u.type = "Issue create";
        u.dbId = row.DeadId;
        u.originalId = row.OriginalId;
        u.attempts = row.Attempts;
        u.terminalReason = row.TerminalReason;
        u.lastError = row.LastError;
        u.createdEpoch = row.CreatedAtEpochSec;
        u.archivedEpoch = row.ArchivedAtEpochSec;
        u.payload = row.Payload;
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
        rows.push_back(std::move(u));
    }
    for (const DeadPendingFieldEdit& row : deadEdits) {
        UnifiedOfflineRow u;
        u.kind = UnifiedOfflineKind::DeadFieldEdit;
        u.key = MakeUnifiedOfflineRowKey(u.kind, row.DeadId);
        u.state = "Dead";
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

} // namespace

bool DrawUnifiedOfflineQueuesPanel(AppController& app, UiDrawSession& d) {
    const std::vector<PendingCreate> pendingCreates = app.GetPendingCreates();
    const std::vector<DeadPendingCreate> deadCreates = app.GetDeadPendingCreates();
    const std::vector<PendingFieldEditRecord> pendingEdits = app.GetPendingFieldEdits();
    const std::vector<DeadPendingFieldEdit> deadEdits = app.GetDeadPendingFieldEdits();

    const size_t nPc = pendingCreates.size();
    const size_t nDc = deadCreates.size();
    const size_t nPf = pendingEdits.size();
    const size_t nDf = deadEdits.size();
    if (nPc + nDc + nPf + nDf == 0) {
        return false;
    }

    static std::unordered_set<std::string> selectedOfflineRowKeys;
    std::vector<UnifiedOfflineRow> rows = BuildUnifiedOfflineRows(pendingCreates, deadCreates, pendingEdits, deadEdits);

    std::unordered_set<std::string> liveKeys;
    liveKeys.reserve(rows.size());
    for (const UnifiedOfflineRow& r : rows) {
        liveKeys.insert(r.key);
    }
    for (auto it = selectedOfflineRowKeys.begin(); it != selectedOfflineRowKeys.end();) {
        if (liveKeys.count(*it) == 0) {
            it = selectedOfflineRowKeys.erase(it);
        } else {
            ++it;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (d.offlineQueuePanelStatusHasClearDeadline && now >= d.offlineQueuePanelStatusClearAt) {
        d.offlineQueuePanelStatus.clear();
        d.offlineQueuePanelStatusHasClearDeadline = false;
    }
    if (d.deadLetterPanelStatusHasClearDeadline && now >= d.deadLetterPanelStatusClearAt) {
        d.deadLetterPanelStatus.clear();
        d.deadLetterPanelStatusHasClearDeadline = false;
    }

    ImGui::PushID("unifiedOfflineQueues");
    ImGui::SeparatorText("Offline Queue");

    if (!d.offlineQueuePanelStatus.empty()) {
        ImGui::TextWrapped("%s", d.offlineQueuePanelStatus.c_str());
    }
    if (!d.deadLetterPanelStatus.empty()) {
        ImGui::TextWrapped("%s", d.deadLetterPanelStatus.c_str());
    }

    ImGui::TextDisabled(
        "Queued rows replay when Tracker is reachable. Dead rows are archived after retries or validation failure. "
        "Retry applies only to failed issue creates. Discard removes rows locally only.");

    auto copySelectionToClipboard = [&](const std::string& fallbackKey) {
        std::string out;
        if (!selectedOfflineRowKeys.empty()) {
            for (const UnifiedOfflineRow& r : rows) {
                if (selectedOfflineRowKeys.count(r.key) == 0) {
                    continue;
                }
                if (!out.empty()) {
                    out.push_back('\n');
                }
                out += UnifiedOfflineRowClipboardLine(r);
            }
        } else if (!fallbackKey.empty()) {
            for (const UnifiedOfflineRow& r : rows) {
                if (r.key == fallbackKey) {
                    out = UnifiedOfflineRowClipboardLine(r);
                    break;
                }
            }
        }
        if (out.empty()) {
            return false;
        }
        ImGui::SetClipboardText(out.c_str());
        return true;
    };

    if (ImGui::Button("Copy selected##unifiedoff")) {
        copySelectionToClipboard(std::string());
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard selected##unifiedoff")) {
        int disc = 0;
        int fail = 0;
        for (const std::string& key : selectedOfflineRowKeys) {
            for (const auto& r : rows) {
                if (r.key == key) {
                    if (r.kind == UnifiedOfflineKind::PendingCreate) {
                        if (app.DeletePendingCreates({r.dbId}).Deleted > 0) {
                            ++disc;
                        } else {
                            ++fail;
                        }
                    } else if (r.kind == UnifiedOfflineKind::DeadCreate) {
                        if (app.DeleteDeadPendingCreates({r.dbId}).Deleted > 0) {
                            ++disc;
                        } else {
                            ++fail;
                        }
                    } else if (r.kind == UnifiedOfflineKind::PendingFieldEdit) {
                        if (app.DeletePendingFieldEdits({r.dbId}).Deleted > 0) {
                            ++disc;
                        } else {
                            ++fail;
                        }
                    } else if (r.kind == UnifiedOfflineKind::DeadFieldEdit) {
                        if (app.DeleteDeadPendingFieldEdits({r.dbId}).Deleted > 0) {
                            ++disc;
                        } else {
                            ++fail;
                        }
                    }
                    break;
                }
            }
        }
        selectedOfflineRowKeys.clear();
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Discard offline edits: removed %d rows from DB (failed %d).", disc, fail);
        ArmOfflineQueuePanelStatus(d, buf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Retry creates now##unifiedoff")) {
        d.offlineQueuePanelStatus = "Triggering manual retry scan of pending Creates...";
        d.offlineQueuePanelStatusHasClearDeadline = false;
        app.TickOfflineCreates();
        app.TickOfflineFieldEdits();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear archived dead rows##unifiedoff")) {
        int del = 0;
        int fail = 0;
        for (const auto& r : rows) {
            if (r.kind == UnifiedOfflineKind::DeadCreate) {
                if (app.DeleteDeadPendingCreates({r.dbId}).Deleted > 0) {
                    ++del;
                } else {
                    ++fail;
                }
            } else if (r.kind == UnifiedOfflineKind::DeadFieldEdit) {
                if (app.DeleteDeadPendingFieldEdits({r.dbId}).Deleted > 0) {
                    ++del;
                } else {
                    ++fail;
                }
            }
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Cleared archived dead rows: deleted %d rows (failed %d).", del, fail);
        ArmDeadLetterPanelStatus(d, buf);
    }

    std::string hoveredOfflineKey;
    const float tblH = OfflineAuxTableOuterHeight(rows.size());
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("offlineQueueTbl", 11, flags, ImVec2(0.0f, tblH))) {
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

            if (ImGui::TableSetColumnIndex(0)) {
                bool sel = selectedOfflineRowKeys.count(row.key) > 0;
                if (ImGui::Checkbox("##check", &sel)) {
                    if (sel) {
                        selectedOfflineRowKeys.insert(row.key);
                    } else {
                        selectedOfflineRowKeys.erase(row.key);
                    }
                }
                if (ImGui::IsItemHovered()) {
                    hoveredOfflineKey = row.key;
                }
            }

            ImGui::TableSetColumnIndex(1);
            if (row.kind == UnifiedOfflineKind::PendingCreate) {
                ImGui::TextDisabled("Queued create");
            } else if (row.kind == UnifiedOfflineKind::DeadCreate) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                ImGui::TextUnformatted("Dead create");
                ImGui::PopStyleColor();
            } else if (row.kind == UnifiedOfflineKind::PendingFieldEdit) {
                ImGui::TextDisabled("Queued edit");
            } else if (row.kind == UnifiedOfflineKind::DeadFieldEdit) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                ImGui::TextUnformatted("Dead edit");
                ImGui::PopStyleColor();
            }

            ImGui::TableSetColumnIndex(2);
            {
                ImGui::Selectable(row.state.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                if (ImGui::IsItemHovered()) {
                    hoveredOfflineKey = row.key;
                }
                const bool rightClicked = ImGui::BeginPopupContextItem("offRowCtx", ImGuiPopupFlags_MouseButtonRight);
                if (rightClicked) {
                    hoveredOfflineKey = row.key;
                    std::vector<UnifiedOfflineRow> picks;
                    if (selectedOfflineRowKeys.count(row.key) > 0) {
                        for (const auto& r : rows) {
                            if (selectedOfflineRowKeys.count(r.key) > 0) {
                                picks.push_back(r);
                            }
                        }
                    } else {
                        picks.push_back(row);
                    }

                    if (ImGui::MenuItem("Copy fields to clipboard (TSV)")) {
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

                    const bool hasCreates = std::any_of(picks.begin(), picks.end(), [](const UnifiedOfflineRow& p) {
                        return p.kind == UnifiedOfflineKind::PendingCreate || p.kind == UnifiedOfflineKind::DeadCreate;
                    });
                    if (hasCreates) {
                        std::string act = (picks.size() == 1) ? "Open draft Issue Editor..." : "Open latest draft...";
                        if (ImGui::MenuItem(act.c_str())) {
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
                    }

                    if (ImGui::MenuItem("Discard record(s) from DB")) {
                        int del = 0;
                        int failed = 0;
                        for (const auto& p : picks) {
                            if (p.kind == UnifiedOfflineKind::PendingCreate) {
                                if (app.DeletePendingCreates({p.dbId}).Deleted > 0) {
                                    ++del;
                                } else {
                                    ++failed;
                                }
                            } else if (p.kind == UnifiedOfflineKind::DeadCreate) {
                                if (app.DeleteDeadPendingCreates({p.dbId}).Deleted > 0) {
                                    ++del;
                                } else {
                                    ++failed;
                                }
                            } else if (p.kind == UnifiedOfflineKind::PendingFieldEdit) {
                                if (app.DeletePendingFieldEdits({p.dbId}).Deleted > 0) {
                                    ++del;
                                } else {
                                    ++failed;
                                }
                            } else if (p.kind == UnifiedOfflineKind::DeadFieldEdit) {
                                if (app.DeleteDeadPendingFieldEdits({p.dbId}).Deleted > 0) {
                                    ++del;
                                } else {
                                    ++failed;
                                }
                            }
                            selectedOfflineRowKeys.erase(p.key);
                        }
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "Discard offline context: successfully deleted %d rows (failed %d).",
                                      del, failed);
                        ArmOfflineQueuePanelStatus(d, buf);
                    }

                    const bool hasDeadCreates = std::any_of(picks.begin(), picks.end(), [](const UnifiedOfflineRow& p) {
                        return p.kind == UnifiedOfflineKind::DeadCreate;
                    });
                    if (hasDeadCreates && ImGui::MenuItem("Move dead create(s) back to retry queue")) {
                        int del = 0;
                        int failed = 0;
                        for (const auto& p : picks) {
                            if (p.kind != UnifiedOfflineKind::DeadCreate) {
                                continue;
                            }
                            std::string qerr;
                            IssueDraft draft;
                            if (IssueDraftHelpers::FromJson(p.payload, draft, qerr)) {
                                draft.ExistingIssueKey.clear();
                                draft.FieldValues.erase("issuekey");
                                draft.FieldValues.erase("key");
                                if (app.QueueCreateOffline(draft) > 0) {
                                    (void)app.DeleteDeadPendingCreates({p.dbId});
                                    ++del;
                                } else {
                                    ++failed;
                                }
                            } else {
                                ++failed;
                            }
                            selectedOfflineRowKeys.erase(p.key);
                        }
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "Restored dead creates to offline queue: %d retrying, %d failed.",
                                      del, failed);
                        ArmOfflineQueuePanelStatus(d, buf);
                    }

                    const bool hasDeadEdits = std::any_of(picks.begin(), picks.end(), [](const UnifiedOfflineRow& p) {
                        return p.kind == UnifiedOfflineKind::DeadFieldEdit;
                    });
                    if (hasDeadEdits && ImGui::MenuItem("Move dead edit(s) back to retry queue")) {
                        int del = 0;
                        int failed = 0;
                        for (const auto& p : picks) {
                            if (p.kind != UnifiedOfflineKind::DeadFieldEdit) {
                                continue;
                            }
                            std::string qerr;
                            if (app.QueueFieldEditOffline(p.issue, p.field, p.payload, qerr) > 0) {
                                (void)app.DeleteDeadPendingFieldEdits({p.dbId});
                                ++del;
                            } else {
                                ++failed;
                            }
                            selectedOfflineRowKeys.erase(p.key);
                        }
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "Restored dead edits to offline queue: %d retrying, %d failed.",
                                      del, failed);
                        ArmOfflineQueuePanelStatus(d, buf);
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%lld", static_cast<long long>(row.dbId));

            ImGui::TableSetColumnIndex(4);
            if (row.originalId != 0) {
                ImGui::Text("%lld", static_cast<long long>(row.originalId));
            } else {
                ImGui::TextUnformatted("-");
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(row.issue.empty() ? "-" : row.issue.c_str());
            ImGui::TableSetColumnIndex(6);
            ImGui::TextUnformatted(row.field.empty() ? "-" : row.field.c_str());
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%d", row.attempts);

            ImGui::TableSetColumnIndex(8);
            {
                const std::string errShow = UnifiedOfflineRowLastErrorDisplay(row);
                const std::string errPreview = BuildPayloadPreview(errShow, 120);
                ImGui::TextUnformatted(errPreview.empty() ? "-" : errPreview.c_str());
                const std::string tip = UnifiedOfflineLastErrorTooltip(row);
                if (!tip.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }

            ImGui::TableSetColumnIndex(9);
            ImGui::TextUnformatted(FormatEpochLocal(row.createdEpoch).c_str());
            ImGui::TableSetColumnIndex(10);
            if (row.archivedEpoch != 0) {
                ImGui::TextUnformatted(FormatEpochLocal(row.archivedEpoch).c_str());
            } else {
                ImGui::TextUnformatted("-");
            }
            ImGui::TableSetColumnIndex(11); // Wait, we have 11 columns set up. 0 to 10 inclusive is 11 columns!
            {
                const std::string payloadPreview = BuildPayloadPreview(row.payload, 140);
                ImGui::TextUnformatted(payloadPreview.empty() ? "-" : payloadPreview.c_str());
                if (!row.payload.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", row.payload.c_str());
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    const bool copyShortcut = ImGuiEffectiveKeyCtrl() && ImGui::IsKeyPressed(ImGuiKey_C, false);
    if (copyShortcut && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
        copySelectionToClipboard(hoveredOfflineKey);
    }

    ImGui::PopID();
    return true;
}
