#include "SmatchetUI.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "JiraGridFieldDisplay.h"
#include "JiraHttpUtils.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetInputModifierBridge.h"
#include "SmatchetUiSession.h"
#include "SmatchetTheme.h"
#include "SmatchetToast.h"
#include "StringUtil.h"
#include "TicketFieldEditor.h"
#include "TicketGridModel.h"
#include "UiPerfMonitor.h"

#include "imgui.h"
#include "imgui_internal.h"
#include <ghc/filesystem.hpp>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace {

// Unreal plugin: ImGui io.Key* can lag OS keyboard state (input vs draw thread).
#if defined(_WIN32)
static bool ImGuiEffectiveKeyCtrl() {
    const ImGuiIO& io = ImGui::GetIO();
    const bool pushed = SmatchetInput_GetPluginModifiersPushedCtrl();
    const bool async =
        ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
    return io.KeyCtrl || pushed || async;
}
static bool ImGuiEffectiveKeyShift() {
    const ImGuiIO& io = ImGui::GetIO();
    const bool pushed = SmatchetInput_GetPluginModifiersPushedShift();
    const bool async =
        ((::GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);
    return io.KeyShift || pushed || async;
}
#else
static bool ImGuiEffectiveKeyCtrl() { return ImGui::GetIO().KeyCtrl; }
static bool ImGuiEffectiveKeyShift() { return ImGui::GetIO().KeyShift; }
#endif

constexpr std::chrono::milliseconds kViewStateSaveDebounceLocal{300};

void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory) {
    ConfigManager::Save(d.cfg);
    if (pushHistory)
        d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
    app.SyncWithBackend(&d.cfg, &store);
}


/** When vertically at top/bottom (or no vertical scroll), map mouse wheel to horizontal scroll; first N wheel ticks at
 * each end ignored (configured by GridEndWheelSwallowsBeforeHorizontal). */
static void RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGuiTable* table, UiDrawSession& d) {
    const int endWheelSwallowsBeforeHorizontal = (std::max)(0, d.cfg.GridEndWheelSwallowsBeforeHorizontal);

    if (!table) {
        return;
    }
    ImGuiWindow* inner = table->InnerWindow;
    ImGuiWindow* outer = table->OuterWindow;
    ImGuiIO& io = ImGui::GetIO();

    if (!inner || std::abs(io.MouseWheel) <= 0.0f || std::abs(io.MouseWheelH) >= 0.0001f ||
        inner->ScrollMax.x <= 0.0f) {
        return;
    }

    const float eps = 1.0f;
    const bool atBottom = inner->Scroll.y >= inner->ScrollMax.y - eps;
    const bool atTop = inner->Scroll.y <= eps;
    const bool noVerticalScroll = inner->ScrollMax.y <= eps;

    if (outer) {
        const ImRect tableRect = outer->Rect();
        if (!ImGui::IsMouseHoveringRect(tableRect.Min, tableRect.Max, false)) {
            return;
        }
    }

    if (!atBottom) {
        d.gridBottomHorizontalWheelSwallowsRemaining = 0;
    }
    if (!atTop) {
        d.gridTopHorizontalWheelSwallowsRemaining = 0;
    }

    bool allowRoute = false;
    if (noVerticalScroll) {
        allowRoute = true;
    } else if (atBottom) {
        if (d.gridBottomHorizontalWheelSwallowsRemaining < endWheelSwallowsBeforeHorizontal) {
            ++d.gridBottomHorizontalWheelSwallowsRemaining;
            return;
        }
        allowRoute = true;
    } else if (atTop) {
        if (d.gridTopHorizontalWheelSwallowsRemaining < endWheelSwallowsBeforeHorizontal) {
            ++d.gridTopHorizontalWheelSwallowsRemaining;
            return;
        }
        allowRoute = true;
    }

    if (!allowRoute) {
        return;
    }

    const float wheelToScrollX = -io.MouseWheel * (ImGui::GetTextLineHeightWithSpacing() * 3.0f);
    float targetX = inner->Scroll.x + wheelToScrollX;
    if (targetX < 0.0f) {
        targetX = 0.0f;
    }
    if (targetX > inner->ScrollMax.x) {
        targetX = inner->ScrollMax.x;
    }
    ImGui::SetScrollX(inner, targetX);
}

std::string BuildCellKey(const std::string& issueId, const std::string& fieldId) { return issueId + "|" + fieldId; }

void PrepareNewIssueDraftForSubmit(UiDrawSession& d) {
    for (auto& kv : d.newIssueDraftEditBufs) {
        d.newIssueDraft.FieldValues[kv.first] = std::string(kv.second.data());
    }
    // Sync ParentKey from parent cell after flush; empty cell clears inherited ParentKey.
    auto parentIt = d.newIssueDraft.FieldValues.find("parent");
    if (parentIt != d.newIssueDraft.FieldValues.end() && !parentIt->second.empty()) {
        std::string parentKey = parentIt->second;
        const size_t sep = parentKey.find(" - ");
        if (sep != std::string::npos)
            parentKey = parentKey.substr(0, sep);
        d.newIssueDraft.ParentKey = parentKey;
        d.newIssueDraft.FieldValues.erase(parentIt);
    } else {
        if (parentIt != d.newIssueDraft.FieldValues.end()) {
            d.newIssueDraft.FieldValues.erase(parentIt);
        }
        d.newIssueDraft.ParentKey.clear();
    }
}

bool QueueNewIssueDraftOffline(AppController& app, UiDrawSession& d, const std::string& okMessage) {
    PrepareNewIssueDraftForSubmit(d);
    const std::int64_t qid = app.QueueCreateOffline(d.newIssueDraft);
    if (qid <= 0) {
        d.gridEditSuccess.clear();
        d.gridEditError = "Failed to queue offline.";
        d.newIssueQueueFallbackVisible = false;
        d.newIssueQueueFallbackError.clear();
        return false;
    }
    d.gridEditError.clear();
    d.gridEditSuccess = okMessage.empty() ? "Queued offline." : okMessage;
    d.newIssueDraftActive = false;
    d.newIssueDraft = IssueDraft{};
    d.newIssueDraftEditBufs.clear();
    d.newIssueMissingFieldIds.clear();
    d.newIssueQueueFallbackVisible = false;
    d.newIssueQueueFallbackError.clear();
    return true;
}

bool IsLikelyOfflineCreateError(const std::string& error) { return IsJiraTransportErrorText(error); }

/**
 * Ensure d.newIssueDraftEditBufs has a sized InputText buffer for `fieldId`,
 * seeded from `seed`. Buffer is large enough for summary-ish lines; the
 * description column gets a bigger allocation via the caller's override.
 * Description uses ImGui::InputText (single-line); Jira often returns multiline
 * text — normalize CR/LF to spaces in the initial seed so the control stays one line.
 */
static std::vector<char>& EnsureDraftEditBuf(UiDrawSession& d, const std::string& fieldId, const std::string& seed,
                                             size_t minSize = 512) {
    auto& buf = d.newIssueDraftEditBufs[fieldId];
    if (buf.empty()) {
        buf.resize(std::max(minSize, seed.size() + 64), '\0');
        std::memcpy(buf.data(), seed.data(), std::min(buf.size() - 1, seed.size()));
        if (fieldId == "description") {
            for (size_t i = 0; i < buf.size() && buf[i] != '\0'; ++i) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    buf[i] = ' ';
                }
            }
        }
    }
    return buf;
}

static std::string FormatBytesUi(std::uint64_t n) {
    if (n < 1024) {
        return std::to_string(n) + " B";
    }
    const char* suffix = "KB";
    double v = static_cast<double>(n) / 1024.0;
    if (v >= 1024.0) {
        v /= 1024.0;
        suffix = "MB";
    }
    if (v >= 1024.0) {
        v /= 1024.0;
        suffix = "GB";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, suffix);
    return std::string(buf);
}

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

/** Outer_size.y for offline queue tables with ScrollY: header + up to 10 data rows (+ small border fudge). */
static float OfflineAuxTableOuterHeight(size_t rowCount) {
    const int visibleDataRows = static_cast<int>((std::min)(rowCount, size_t(10)));
    const ImGuiStyle& st = ImGui::GetStyle();
    const float headerH = ImGui::GetFrameHeightWithSpacing();
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    return headerH + static_cast<float>(visibleDataRows) * rowH + st.CellPadding.y * 2.0f + 2.0f;
}

static std::string SanitizeClipboardCell(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
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
    // Queued JSON can disagree (e.g. root issueTypeId=Bug but fields.issuetype=Subtask). Prefer struct fields.
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

static bool DrawUnifiedOfflineQueuesPanel(AppController& app, UiDrawSession& d) {
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

    char header[288];
    std::snprintf(header, sizeof(header),
                  "Offline queue (queued creates %zu, queued edits %zu, dead creates %zu, dead edits %zu)", nPc, nPf,
                  nDc, nDf);
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        return true;
    }

    ImGui::PushID("unified_offline_queue_panel");
    if (!d.offlineQueuePanelStatus.empty()) {
        ImGui::TextWrapped("%s", d.offlineQueuePanelStatus.c_str());
    }
    if (!d.deadLetterPanelStatus.empty()) {
        ImGui::TextWrapped("%s", d.deadLetterPanelStatus.c_str());
    }

    ImGui::TextDisabled(
        "Queued rows replay when Jira is reachable. Dead rows are archived after retries or validation failure. "
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
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemTooltip(
            "Copies selected rows as TSV (mixed kinds).\nRows follow current table order (newest activity first).");
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard selected##unifiedoff")) {
        if (selectedOfflineRowKeys.empty()) {
            ArmOfflineQueuePanelStatus(d, "Discard: no rows selected.");
        } else {
            std::vector<std::int64_t> pcIds;
            std::vector<std::int64_t> dcDeadIds;
            std::vector<std::int64_t> pfIds;
            std::vector<std::int64_t> dfDeadIds;
            for (const UnifiedOfflineRow& r : rows) {
                if (selectedOfflineRowKeys.count(r.key) == 0) {
                    continue;
                }
                switch (r.kind) {
                case UnifiedOfflineKind::PendingCreate:
                    pcIds.push_back(r.dbId);
                    break;
                case UnifiedOfflineKind::DeadCreate:
                    dcDeadIds.push_back(r.dbId);
                    break;
                case UnifiedOfflineKind::PendingFieldEdit:
                    pfIds.push_back(r.dbId);
                    break;
                case UnifiedOfflineKind::DeadFieldEdit:
                    dfDeadIds.push_back(r.dbId);
                    break;
                }
            }
            int delPc = 0, failPc = 0, delDc = 0, failDc = 0, delPf = 0, failPf = 0, delDf = 0, failDf = 0;
            if (!pcIds.empty()) {
                const AppController::PendingQueueDeleteSummary s = app.DeletePendingCreates(pcIds);
                delPc = s.Deleted;
                failPc = s.Failed;
            }
            if (!dcDeadIds.empty()) {
                const AppController::DeadLetterDeleteSummary s = app.DeleteDeadPendingCreates(dcDeadIds);
                delDc = s.Deleted;
                failDc = s.Failed;
            }
            if (!pfIds.empty()) {
                const AppController::PendingFieldEditDeleteSummary s = app.DeletePendingFieldEdits(pfIds);
                delPf = s.Deleted;
                failPf = s.Failed;
            }
            if (!dfDeadIds.empty()) {
                const AppController::DeadFieldEditDeleteSummary s = app.DeleteDeadPendingFieldEdits(dfDeadIds);
                delDf = s.Deleted;
                failDf = s.Failed;
            }
            selectedOfflineRowKeys.clear();
            LOG_INFO("Unified offline discard selected: pending_creates deleted=%d failed=%d; dead_creates deleted=%d "
                     "failed=%d; pending_field_edits deleted=%d failed=%d; dead_field_edits deleted=%d failed=%d",
                     delPc, failPc, delDc, failDc, delPf, failPf, delDf, failDf);
            std::string discardMsg = "Discard selected:";
            if (!pcIds.empty()) {
                discardMsg += "\n· Pending creates: deleted ";
                discardMsg += std::to_string(delPc);
                discardMsg += ", failed ";
                discardMsg += std::to_string(failPc);
            }
            if (!dcDeadIds.empty()) {
                discardMsg += "\n· Dead creates: deleted ";
                discardMsg += std::to_string(delDc);
                discardMsg += ", failed ";
                discardMsg += std::to_string(failDc);
            }
            if (!pfIds.empty()) {
                discardMsg += "\n· Pending field edits: deleted ";
                discardMsg += std::to_string(delPf);
                discardMsg += ", failed ";
                discardMsg += std::to_string(failPf);
            }
            if (!dfDeadIds.empty()) {
                discardMsg += "\n· Dead field edits: deleted ";
                discardMsg += std::to_string(delDf);
                discardMsg += ", failed ";
                discardMsg += std::to_string(failDf);
            }
            ArmOfflineQueuePanelStatus(d, discardMsg);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemTooltip("Remove selected rows from local queues/archives.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard all queued##unifiedoff")) {
        std::vector<std::int64_t> pcIds;
        std::vector<std::int64_t> pfIds;
        pcIds.reserve(nPc);
        pfIds.reserve(nPf);
        for (const UnifiedOfflineRow& r : rows) {
            if (r.kind == UnifiedOfflineKind::PendingCreate) {
                pcIds.push_back(r.dbId);
            } else if (r.kind == UnifiedOfflineKind::PendingFieldEdit) {
                pfIds.push_back(r.dbId);
            }
        }
        int delPc = 0, failPc = 0, delPf = 0, failPf = 0;
        if (!pcIds.empty()) {
            const AppController::PendingQueueDeleteSummary s = app.DeletePendingCreates(pcIds);
            delPc = s.Deleted;
            failPc = s.Failed;
        }
        if (!pfIds.empty()) {
            const AppController::PendingFieldEditDeleteSummary s = app.DeletePendingFieldEdits(pfIds);
            delPf = s.Deleted;
            failPf = s.Failed;
        }
        selectedOfflineRowKeys.clear();
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Discard all queued: creates %d/%d, field edits %d/%d.", delPc, failPc, delPf,
                      failPf);
        ArmOfflineQueuePanelStatus(d, buf);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemTooltip("Delete every queued issue create and queued field edit in this list.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard all dead##unifiedoff")) {
        std::vector<std::int64_t> dcIds;
        std::vector<std::int64_t> dfIds;
        dcIds.reserve(nDc);
        dfIds.reserve(nDf);
        for (const UnifiedOfflineRow& r : rows) {
            if (r.kind == UnifiedOfflineKind::DeadCreate) {
                dcIds.push_back(r.dbId);
            } else if (r.kind == UnifiedOfflineKind::DeadFieldEdit) {
                dfIds.push_back(r.dbId);
            }
        }
        int delDc = 0, failDc = 0, delDf = 0, failDf = 0;
        if (!dcIds.empty()) {
            const AppController::DeadLetterDeleteSummary s = app.DeleteDeadPendingCreates(dcIds);
            delDc = s.Deleted;
            failDc = s.Failed;
        }
        if (!dfIds.empty()) {
            const AppController::DeadFieldEditDeleteSummary s = app.DeleteDeadPendingFieldEdits(dfIds);
            delDf = s.Deleted;
            failDf = s.Failed;
        }
        selectedOfflineRowKeys.clear();
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Discard all dead: creates %d/%d, field edits %d/%d.", delDc, failDc, delDf,
                      failDf);
        ArmDeadLetterPanelStatus(d, buf);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemTooltip("Delete every dead-letter row in this list.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Retry selected##unifiedoff")) {
        bool onlyDeadCreates = true;
        for (const std::string& k : selectedOfflineRowKeys) {
            const auto it =
                std::find_if(rows.begin(), rows.end(), [&](const UnifiedOfflineRow& r) { return r.key == k; });
            if (it == rows.end() || it->kind != UnifiedOfflineKind::DeadCreate) {
                onlyDeadCreates = false;
                break;
            }
        }
        if (selectedOfflineRowKeys.empty()) {
            ArmDeadLetterPanelStatus(d, "Retry: no rows selected.");
        } else if (!onlyDeadCreates) {
            ArmDeadLetterPanelStatus(d, "Retry applies only to failed issue creates.");
        } else {
            std::unordered_set<std::int64_t> seenOrig;
            std::vector<std::int64_t> originalIds;
            for (const UnifiedOfflineRow& r : rows) {
                if (selectedOfflineRowKeys.count(r.key) == 0 || r.kind != UnifiedOfflineKind::DeadCreate) {
                    continue;
                }
                if (seenOrig.insert(r.originalId).second) {
                    originalIds.push_back(r.originalId);
                }
            }
            const AppController::DeadLetterRestoreSummary rs = app.RestoreDeadPendingCreates(originalIds);
            selectedOfflineRowKeys.clear();
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "Retry finished: restored=%d failed=%d (restored rows re-enter offline queue).", rs.Restored,
                          rs.Failed);
            ArmDeadLetterPanelStatus(d, buf);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemTooltip("Re-queue selected dead issue creates for offline retry.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Retry all dead creates##unifiedoff")) {
        std::unordered_set<std::int64_t> seenOrig;
        std::vector<std::int64_t> originalIds;
        for (const UnifiedOfflineRow& r : rows) {
            if (r.kind != UnifiedOfflineKind::DeadCreate) {
                continue;
            }
            if (seenOrig.insert(r.originalId).second) {
                originalIds.push_back(r.originalId);
            }
        }
        const AppController::DeadLetterRestoreSummary rs = app.RestoreDeadPendingCreates(originalIds);
        selectedOfflineRowKeys.clear();
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Retry all dead creates: restored=%d failed=%d.", rs.Restored, rs.Failed);
        ArmDeadLetterPanelStatus(d, buf);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemTooltip("Re-queue every dead issue create shown in this list.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open as new draft##unifiedoff")) {
        if (selectedOfflineRowKeys.size() != 1) {
            ArmOfflineQueuePanelStatus(d, "Open as draft: select exactly one issue-create row.");
        } else {
            const std::string onlyKey = *selectedOfflineRowKeys.begin();
            const UnifiedOfflineRow* pick = nullptr;
            for (const UnifiedOfflineRow& r : rows) {
                if (r.key == onlyKey) {
                    pick = &r;
                    break;
                }
            }
            if (pick == nullptr) {
                ArmOfflineQueuePanelStatus(d, "Open as draft: selected row not found.");
            } else if (pick->kind == UnifiedOfflineKind::PendingCreate) {
                OpenPendingCreateRowAsNewIssueDraft(d, UnifiedOfflineToPendingCreate(*pick));
            } else if (pick->kind == UnifiedOfflineKind::DeadCreate) {
                OpenDeadLetterRowAsNewIssueDraft(d, UnifiedOfflineToDeadPendingCreate(*pick));
            } else {
                ArmOfflineQueuePanelStatus(d, "Open as draft: select an issue create row (queued or dead).");
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemTooltip("Load one selected issue create into the \"+ New issue\" draft.");
    }

    ImGui::TextDisabled("Copy uses current table sort order.");
    ImGui::TextDisabled("Tip: Click Db id to select; Ctrl+Click toggles; Ctrl+C copies selected rows.");

    std::string hoveredOfflineKey;
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("UnifiedOfflineQueueTable", 11, flags,
                          ImVec2(0.0f, OfflineAuxTableOuterHeight(rows.size())))) {
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("Db id", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Orig", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Issue", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Att", ImGuiTableColumnFlags_WidthFixed, 36.0f);
        ImGui::TableSetupColumn("Last error", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Created", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Archived", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        for (const UnifiedOfflineRow& row : rows) {
            ImGui::TableNextRow();
            ImGui::PushID(row.key.c_str());
            const bool rowSelected = (selectedOfflineRowKeys.count(row.key) != 0);

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.state.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(row.type.c_str());

            ImGui::TableSetColumnIndex(2);
            const bool pressed = ImGui::Selectable(std::to_string(static_cast<long long>(row.dbId)).c_str(),
                                                   rowSelected, ImGuiSelectableFlags_None);
            if (ImGui::IsItemHovered()) {
                hoveredOfflineKey = row.key;
            }
            if (pressed) {
                const bool ctrl = ImGuiEffectiveKeyCtrl();
                if (!ctrl) {
                    selectedOfflineRowKeys.clear();
                    selectedOfflineRowKeys.insert(row.key);
                } else if (rowSelected) {
                    selectedOfflineRowKeys.erase(row.key);
                } else {
                    selectedOfflineRowKeys.insert(row.key);
                }
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::Selectable("Copy row##unifiedctx")) {
                    ImGui::SetClipboardText(UnifiedOfflineRowClipboardLine(row).c_str());
                }
                if (ImGui::Selectable("Copy selected rows##unifiedctx")) {
                    copySelectionToClipboard(row.key);
                }
                if (row.kind == UnifiedOfflineKind::PendingCreate) {
                    if (ImGui::Selectable("Discard row##unifiedctx")) {
                        const AppController::PendingQueueDeleteSummary del = app.DeletePendingCreates({row.dbId});
                        selectedOfflineRowKeys.erase(row.key);
                        char buf[120];
                        std::snprintf(buf, sizeof(buf), "Discard row id=%lld: deleted=%d failed=%d",
                                      static_cast<long long>(row.dbId), del.Deleted, del.Failed);
                        ArmOfflineQueuePanelStatus(d, buf);
                    }
                    if (ImGui::Selectable("Open as new draft##unifiedctx")) {
                        OpenPendingCreateRowAsNewIssueDraft(d, UnifiedOfflineToPendingCreate(row));
                        selectedOfflineRowKeys.erase(row.key);
                    }
                } else if (row.kind == UnifiedOfflineKind::DeadCreate) {
                    if (ImGui::Selectable("Retry row##unifiedctx")) {
                        const AppController::DeadLetterRestoreSummary r =
                            app.RestoreDeadPendingCreates({row.originalId});
                        selectedOfflineRowKeys.erase(row.key);
                        char buf[160];
                        std::snprintf(buf, sizeof(buf), "Retry row orig=%lld: restored=%d failed=%d",
                                      static_cast<long long>(row.originalId), r.Restored, r.Failed);
                        ArmDeadLetterPanelStatus(d, buf);
                    }
                    if (ImGui::Selectable("Open as new draft##unifiedctx")) {
                        OpenDeadLetterRowAsNewIssueDraft(d, UnifiedOfflineToDeadPendingCreate(row));
                        selectedOfflineRowKeys.erase(row.key);
                    }
                    if (ImGui::Selectable("Discard row##unifiedctx")) {
                        const AppController::DeadLetterDeleteSummary del = app.DeleteDeadPendingCreates({row.dbId});
                        selectedOfflineRowKeys.erase(row.key);
                        char buf[120];
                        std::snprintf(buf, sizeof(buf), "Discard row dead_id=%lld: deleted=%d failed=%d",
                                      static_cast<long long>(row.dbId), del.Deleted, del.Failed);
                        ArmDeadLetterPanelStatus(d, buf);
                    }
                } else if (row.kind == UnifiedOfflineKind::PendingFieldEdit) {
                    if (ImGui::Selectable("Discard row##unifiedctx")) {
                        const AppController::PendingFieldEditDeleteSummary del =
                            app.DeletePendingFieldEdits({row.dbId});
                        selectedOfflineRowKeys.erase(row.key);
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "Discard field edit id=%lld: deleted=%d failed=%d",
                                      static_cast<long long>(row.dbId), del.Deleted, del.Failed);
                        ArmOfflineQueuePanelStatus(d, buf);
                    }
                } else if (row.kind == UnifiedOfflineKind::DeadFieldEdit) {
                    if (ImGui::Selectable("Discard row##unifiedctx")) {
                        const AppController::DeadFieldEditDeleteSummary del =
                            app.DeleteDeadPendingFieldEdits({row.dbId});
                        selectedOfflineRowKeys.erase(row.key);
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "Discard dead field edit dead_id=%lld: deleted=%d failed=%d",
                                      static_cast<long long>(row.dbId), del.Deleted, del.Failed);
                        ArmDeadLetterPanelStatus(d, buf);
                    }
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(3);
            if (row.originalId != 0) {
                ImGui::Text("%lld", static_cast<long long>(row.originalId));
            } else {
                ImGui::TextUnformatted("-");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(row.issue.empty() ? "-" : row.issue.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(row.field.empty() ? "-" : row.field.c_str());
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%d", row.attempts);

            ImGui::TableSetColumnIndex(7);
            {
                const std::string errShow = UnifiedOfflineRowLastErrorDisplay(row);
                const std::string errPreview = BuildPayloadPreview(errShow, 120);
                ImGui::TextUnformatted(errPreview.empty() ? "-" : errPreview.c_str());
                const std::string tip = UnifiedOfflineLastErrorTooltip(row);
                if (!tip.empty() && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }

            ImGui::TableSetColumnIndex(8);
            ImGui::TextUnformatted(FormatEpochLocal(row.createdEpoch).c_str());
            ImGui::TableSetColumnIndex(9);
            if (row.archivedEpoch != 0) {
                ImGui::TextUnformatted(FormatEpochLocal(row.archivedEpoch).c_str());
            } else {
                ImGui::TextUnformatted("-");
            }
            ImGui::TableSetColumnIndex(10);
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

static bool DraftHasStagedPath(const IssueDraft& draft, const ghc::filesystem::path& absPath) {
    std::error_code ec;
    for (const auto& a : draft.StagedAttachments) {
        if (a.AbsPath.empty()) {
            continue;
        }
        if (ghc::filesystem::equivalent(absPath, ghc::filesystem::path(a.AbsPath), ec)) {
            return true;
        }
        ec.clear();
    }
    return false;
}

static bool TryAppendStagedFromAbsPath(IssueDraft& draft, const std::string& absUtf8) {
    namespace fs = ghc::filesystem;
    std::error_code ec;
    const fs::path p(absUtf8);
    if (!fs::is_regular_file(p, ec)) {
        return false;
    }
    if (DraftHasStagedPath(draft, p)) {
        return false;
    }
    StagedAttachment sa;
    sa.AbsPath = absUtf8;
    sa.FileName = p.filename().string();
    sa.SizeBytes = fs::file_size(p, ec);
    draft.StagedAttachments.push_back(std::move(sa));
    return true;
}

static void RenderNewIssueDraftRow(AppController& app, UiDrawSession& d, const std::vector<TicketGridColumn>& columns,
                                   const JiraConfig& cfg, const CachedTicket* lastVisibleTicket) {
    const auto& catalog = app.GetAvailableFields();

    if (d.newIssueCreateInFlight && d.newIssueCreateFuture.valid() &&
        d.newIssueCreateFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        IssueCreateResult r = d.newIssueCreateFuture.get();
        d.newIssueCreateInFlight = false;
        if (d.newIssueDiscardAsyncCreateResult) {
            d.newIssueDiscardAsyncCreateResult = false;
        } else if (r.Ok) {
            d.newIssueMissingFieldIds.clear();
            d.newIssueDraftActive = false;
            d.newIssueDraft = IssueDraft{};
            d.newIssueDraftEditBufs.clear();
            d.newIssueQueueFallbackVisible = false;
            d.newIssueQueueFallbackError.clear();
            d.gridEditError.clear();
            d.gridEditSuccess = "Created " + r.IssueKey + ".";
            // Reload server-truth fields for the created issue and merge into cache.
            app.PrefetchIssueTicketsForKeys({r.IssueKey}, true);
        } else {
            d.newIssueMissingFieldIds = r.MissingFieldIds;
            if (IsLikelyOfflineCreateError(r.Error)) {
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                if (!QueueNewIssueDraftOffline(app, d, "Offline detected; queued offline.")) {
                    d.gridEditSuccess.clear();
                    d.gridEditError = "Create failed (" + r.Error + ") and queue offline failed.";
                    d.newIssueQueueFallbackVisible = true;
                    d.newIssueQueueFallbackError = r.Error;
                }
            } else {
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                d.gridEditSuccess.clear();
                std::string banner = r.Error;
                const std::string lower = ToLowerAsciiCopy(banner);
                if (lower.find("create issue failed") == std::string::npos) {
                    banner.insert(0, "Create issue failed: ");
                }
                d.gridEditError = std::move(banner);
            }
        }
    }

    ImGui::TableNextRow();

    if (!d.newIssueDraftActive) {
        ImGui::TableSetColumnIndex(0);
        if (ImGui::SmallButton("+ New issue")) {
            if (lastVisibleTicket) {
                d.newIssueDraft = IssueDraftHelpers::FromCachedTicket(
                    *lastVisibleTicket, app.GetAvailableFields(), cfg.ProjectKey, cfg.DefaultIssueTypeId,
                    cfg.DefaultIssueTypeName, cfg.NewIssueInheritFieldIds);
            } else {
                d.newIssueDraft = app.BuildDraftFromLastTicket(cfg);
            }
            if (!d.newIssueDraft.ParentKey.empty()) {
                d.newIssueDraft.FieldValues["parent"] = d.newIssueDraft.ParentKey;
            }
            d.newIssueDraftActive = true;
            d.newIssueScrollDraftRowIntoViewPending = true;
            d.newIssueDraftEditBufs.clear();
            d.newIssueMissingFieldIds.clear();
            d.newIssueQueueFallbackVisible = false;
            d.newIssueQueueFallbackError.clear();
            d.gridEditError.clear();
            d.gridEditSuccess.clear();
        }
        return;
    }

    // Active draft: ID column gets Create/Cancel; other columns get per-field inputs.
    const std::unordered_set<std::string> missing(d.newIssueMissingFieldIds.begin(), d.newIssueMissingFieldIds.end());

    for (int colIndex = 0; colIndex < static_cast<int>(columns.size()); ++colIndex) {
        const auto& column = columns[static_cast<size_t>(colIndex)];
        ImGui::TableSetColumnIndex(colIndex);

        if (column.ColumnKind == TicketGridColumn::Kind::Id) {
            ImGui::PushID("newissue_draft_id");
            ImGui::BeginGroup();
            ImGui::TextDisabled("[new]");

            const bool disabled = d.newIssueCreateInFlight;
            const auto runAttachmentPicker = [&]() {
                app.RequestOpenFilePaths(true, d.cfg.LastImportDirectory, [&d](std::vector<std::string> paths) {
                    for (const auto& path : paths) {
                        TryAppendStagedFromAbsPath(d.newIssueDraft, path);
                    }
                });
            };

            const ImVec2 draftActionBtn(ImGui::GetContentRegionAvail().x, 0.0f);
            if (disabled)
                ImGui::BeginDisabled();
            if (ImGui::Button("Create", draftActionBtn)) {
                PrepareNewIssueDraftForSubmit(d);
                d.gridEditError.clear();
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                d.newIssueMissingFieldIds = IssueDraftHelpers::MissingRequiredFields(
                    d.newIssueDraft, app.GetRequiredFieldSet(d.newIssueDraft.ProjectKey, d.newIssueDraft.IssueTypeId,
                                                             d.newIssueDraft.IssueTypeName));
                if (!d.newIssueMissingFieldIds.empty()) {
                    d.gridEditSuccess.clear();
                    d.gridEditError = "Missing required fields.";
                } else {
                    d.newIssueCreateFuture = app.CreateIssueAsync(d.newIssueDraft);
                    d.newIssueCreateInFlight = true;
                }
            }
            const bool canOfferFallbackQueue = !disabled && d.newIssueQueueFallbackVisible;
            if (canOfferFallbackQueue) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.35f, 1.0f));
                ImGui::TextUnformatted("Create failed. Queue instead?");
                ImGui::PopStyleColor();
            }
            if (canOfferFallbackQueue && ImGui::Button("Queue offline", draftActionBtn)) {
                if (!QueueNewIssueDraftOffline(app, d, "Queued offline after create failure.")) {
                    d.gridEditSuccess.clear();
                    d.gridEditError = "Create failed (" + d.newIssueQueueFallbackError + ") and queue offline failed.";
                }
            }
            if (ImGui::Button("Cancel", draftActionBtn)) {
                d.newIssueDraftActive = false;
                d.newIssueDraft = IssueDraft{};
                d.newIssueDraftEditBufs.clear();
                d.newIssueMissingFieldIds.clear();
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                d.gridEditError.clear();
                d.gridEditSuccess.clear();
            }
            if (ImGui::Button("Add attachment...", draftActionBtn)) {
                runAttachmentPicker();
            }
            if (!d.newIssueDraft.StagedAttachments.empty()) {
                if (ImGui::Button("Clear all##clratt", draftActionBtn)) {
                    d.newIssueDraft.StagedAttachments.clear();
                }
            }
            if (disabled)
                ImGui::EndDisabled();

            if (!d.newIssueDraft.StagedAttachments.empty()) {
                if (disabled)
                    ImGui::BeginDisabled();
                constexpr float kChipStripH = 54.0f;
                ImGui::BeginChild("##newissueattstrip", ImVec2(0.0f, kChipStripH), true,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                for (size_t i = 0; i < d.newIssueDraft.StagedAttachments.size();) {
                    const auto& att = d.newIssueDraft.StagedAttachments[i];
                    ImGui::PushID(static_cast<int>(i));
                    std::string label = att.FileName;
                    if (label.size() > 36) {
                        label = label.substr(0, 16) + "..." + label.substr(label.size() - 16);
                    }
                    const std::string chip = label + " (" + FormatBytesUi(att.SizeBytes) + ")";
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(chip.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x##rma")) {
                        d.newIssueDraft.StagedAttachments.erase(d.newIssueDraft.StagedAttachments.begin() +
                                                                static_cast<std::ptrdiff_t>(i));
                        ImGui::PopID();
                        continue;
                    }
                    ImGui::SameLine();
                    ImGui::Dummy(ImVec2(8.0f, 1.0f));
                    ImGui::SameLine();
                    ImGui::PopID();
                    ++i;
                }
                ImGui::EndChild();
                if (disabled)
                    ImGui::EndDisabled();
            }

            ImGui::EndGroup();
            if (d.newIssueScrollDraftRowIntoViewPending) {
                // Anchor scroll to ID column (tall); end-of-row cursor is last column (short).
                ImGui::SetScrollHereY(1.0f);
                d.newIssueScrollDraftRowIntoViewPending = false;
            }
            if (ImGui::BeginPopupContextItem("newissue_draft_att_ctx")) {
                if (ImGui::MenuItem("Add attachment...")) {
                    if (!d.newIssueCreateInFlight) {
                        runAttachmentPicker();
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            continue;
        }

        const std::string& fieldId = column.FieldId;
        if (fieldId == "id" || (IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) && fieldId != "status")) {
            ImGui::TextDisabled("-");
            continue;
        }

        const TrackerField* field = nullptr;
        for (const auto& f : catalog) {
            if (f.Id == fieldId) {
                field = &f;
                break;
            }
        }

        const bool isMissing = missing.count(fieldId) > 0 || (fieldId == "summary" && missing.count("summary") > 0) ||
                               (fieldId == "parent" && missing.count("__parent__") > 0);
        if (isMissing) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.15f, 0.15f, 0.45f));
        }

        ImGui::PushID(fieldId.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);

        const std::string& current = d.newIssueDraft.FieldValues[fieldId];

        // Project/IssueType dropdowns come from catalog option sets when available.
        if (fieldId == "issuetype" || fieldId == "project" ||
            (field && !field->IsArray && !field->AllowedValueOptions.empty() &&
             (field->Family == TrackerFieldFamily::SelectSingle ||
              field->Family == TrackerFieldFamily::StructuredSingle || field->Family == TrackerFieldFamily::IssueType ||
              field->Family == TrackerFieldFamily::Status || field->Family == TrackerFieldFamily::Sprint ||
              field->IsUserType))) {
            std::string preview = current;
            if (fieldId == "project" && preview.empty())
                preview = d.newIssueDraft.ProjectKey;
            if (fieldId == "issuetype" && preview.empty())
                preview = d.newIssueDraft.IssueTypeName;
            if (field && !field->AllowedValueOptions.empty()) {
                for (const auto& opt : field->AllowedValueOptions) {
                    if (opt.Id == current || opt.Value == current) {
                        preview = opt.Value;
                        break;
                    }
                }
            }
            if (ImGui::BeginCombo("##combo", preview.empty() ? "(choose)" : preview.c_str())) {
                if (field) {
                    for (const auto& opt : field->AllowedValueOptions) {
                        const bool selected = (opt.Id == current || opt.Value == current);
                        if (ImGui::Selectable(opt.Value.c_str(), selected)) {
                            d.newIssueQueueFallbackVisible = false;
                            d.newIssueQueueFallbackError.clear();
                            if (fieldId == "issuetype") {
                                d.newIssueDraft.IssueTypeId = opt.Id;
                                d.newIssueDraft.IssueTypeName = opt.Value;
                                // Keep FieldValues in sync: combo preview/selection read `current` from here.
                                d.newIssueDraft.FieldValues[fieldId] = opt.Id.empty() ? opt.Value : opt.Id;
                            } else if (fieldId == "project") {
                                d.newIssueDraft.ProjectKey = opt.Id.empty() ? opt.Value : opt.Id;
                                d.newIssueDraft.FieldValues[fieldId] = d.newIssueDraft.ProjectKey;
                            } else {
                                // Store option id (e.g. accountId for users) so create payload matches grid edits.
                                d.newIssueDraft.FieldValues[fieldId] = opt.Id.empty() ? opt.Value : opt.Id;
                            }
                        }
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            const size_t minBuf = (fieldId == "description") ? size_t(4096) : size_t(512);
            auto& buf = EnsureDraftEditBuf(d, fieldId, current, minBuf);
            if (ImGui::InputText("##input", buf.data(), buf.size())) {
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                d.newIssueDraft.FieldValues[fieldId] = std::string(buf.data());
            }
        }
        ImGui::PopID();

        if (isMissing) {
            ImGui::PopStyleColor();
        }
    }

    if (d.newIssueScrollDraftRowIntoViewPending) {
        // Pending scroll is handled after ID column EndGroup; if no Id column, clear stale flag.
        d.newIssueScrollDraftRowIntoViewPending = false;
    }
}

} // namespace

void SmatchetUI::drawActiveProjectWindow(AppController& app, UiDrawSession& d) {
    ImGui::Begin("Smatchet - Active Project");
    const JiraConnectivityBannerForUi jiraBanner = app.GetJiraConnectivityBannerForUi(nullptr);
    const bool readOnlyMode = (jiraBanner.Kind == JiraConnectivityBannerForUi::Level::Error);

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:header");
        ImGui::Separator();
        const ViewDefinition* activeView = ViewState.GetActiveView();
        if (activeView) {
            ImGui::Text("View: %s", activeView->Name.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Refresh View")) {
                d.cfg.JqlQuery = activeView->Jql;
                d.cfg.SelectedFields = activeView->Fields;
                SyncWithCurrentView(app, d, ViewState.GetStore(), true);
            }
        }

        // Quick Filter UI moved to header next to Open Views
        ImGui::SameLine(0, 30.0f);
        ImGui::SetNextItemWidth(250.0f);
        if (ImGui::InputTextWithHint("##GridFilter", "Filter...", d.gridFilterBuf, sizeof(d.gridFilterBuf))) {
            // Filter changed
        }
        if (d.gridFilterBuf[0] != '\0') {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) { d.gridFilterBuf[0] = '\0'; }
        }

#if defined(SMATCHET_WITH_MCP)
        ImGui::SameLine();
        {
            const char* mcpLabel = d.cfg.McpEnabled ? "● MCP LIVE" : "● MCP DISABLED";
            float mcpWidth = ImGui::CalcTextSize(mcpLabel).x + 40.0f; 
            if (d.cfg.McpEnabled) mcpWidth += 50.0f; // extra for port
            
            float targetX = ImGui::GetWindowContentRegionMax().x - mcpWidth;
            if (targetX > ImGui::GetCursorPosX()) {
                ImGui::SetCursorPosX(targetX);
            }

            if (d.cfg.McpEnabled) {
                if (d.cfg.McpAllowRemote) {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "● MCP LIVE: %d (LAN)", d.cfg.McpPort);
                } else {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "● MCP LIVE: %d", d.cfg.McpPort);
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.25f, 1.0f), "● MCP DISABLED");
            }
            if (ImGui::IsItemHovered()) {
                if (d.cfg.McpEnabled) {
                    ImGui::SetTooltip("MCP on port %d. %s Auth: %s.", d.cfg.McpPort,
                                      d.cfg.McpAllowRemote ? "Bound on all interfaces." : "Localhost only.",
                                      d.cfg.McpAuthToken.empty() ? "loopback only (no token)."
                                                                 : "X-Smatchet-Token required.");
                } else {
                    ImGui::SetTooltip("MCP server is disabled. Enable it under Settings → Preferences → Integrations.");
                }
            }
        }
#endif

        ImGui::Separator();
        if (jiraBanner.Kind == JiraConnectivityBannerForUi::Level::Error) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Grid edits and quick comment actions stay disabled until Jira is reachable.");
            ImGui::Separator();
        } else if (jiraBanner.Kind == JiraConnectivityBannerForUi::Level::Warning) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
    }

    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;

    TrackerFieldCatalogIndex catalogIndex(app.GetAvailableFields());
    const ViewDefinition* activeViewForGrid = ViewState.GetActiveView();
    const std::vector<TicketGridColumn> columns =
        activeViewForGrid ? TicketGridColumnsBuilder::Build(*activeViewForGrid, catalogIndex)
                          : std::vector<TicketGridColumn>();

    const bool viewChanged = activeViewForGrid && (activeViewForGrid->Id != d.lastGridActiveViewId);
    if (viewChanged && activeViewForGrid) {
        d.lastGridActiveViewId = activeViewForGrid->Id;
    }
    if (!activeViewForGrid) {
        d.lastGridActiveViewId.clear();
    }

    const std::string gridContextSignature = BuildGridContextSignature(activeViewForGrid, d.cfg.JqlQuery);
    const bool gridContextChanged =
        !d.lastGridContextSignature.empty() && gridContextSignature != d.lastGridContextSignature;
    if (gridContextChanged) {
        CancelUnfinishedNewIssueForGridChange(d);
    }
    d.lastGridContextSignature = gridContextSignature;

    const bool gridSortEnvironmentChanged = viewChanged || gridContextChanged;

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:prep");
        if (!d.gridEditError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", d.gridEditError.c_str());
            ImGui::PopStyleColor();
        } else if (!d.gridEditSuccess.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
            ImGui::TextWrapped("%s", d.gridEditSuccess.c_str());
            ImGui::PopStyleColor();
        }
    }

    const bool drewOfflineSection = DrawUnifiedOfflineQueuesPanel(app, d);
    if (drewOfflineSection) {
        ImGui::Spacing();
        ImGui::SeparatorText("Ticket grid");
        ImGui::Spacing();
    }

    std::vector<PendingFieldEdit> pendingEdits;
    // Hoisted above the BeginTable scope so the post-EndTable block can read it
    // when deciding whether an out-of-selection click should clear the rect.
    bool rectCellClickedThisFrame = false;
    // Left click landed inside ticket table hitbox (OuterRect ∪ InnerClipRect)
    // but no cell set rectCellClickedThisFrame — blocks false outside-clear (H5).
    bool ticketGridLeftClickInsideTableHit = false;
    std::uint64_t gridSortSig = 0;
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Sortable |
                                       ImGuiTableFlags_SortMulti | ImGuiTableFlags_NoSavedSettings;
    
    ImGui::Separator();

    if (!columns.empty() && ImGui::BeginTable("TicketGrid", static_cast<int>(columns.size()), tableFlags)) {
        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.setup");
            for (const auto& column : columns) {
                const float persistedWidth = activeViewForGrid ? (activeViewForGrid->ColumnWidths.count(column.Key)
                                                                      ? activeViewForGrid->ColumnWidths.at(column.Key)
                                                                      : 0.0f)
                                                               : 0.0f;
                const float defaultWidth = (column.ColumnKind == TicketGridColumn::Kind::Id) ? 90.0f : 180.0f;
                const float width = persistedWidth > 0.0f ? persistedWidth : defaultWidth;
                if (column.ColumnKind == TicketGridColumn::Kind::Id) {
                    ImGui::TableSetupColumn(column.Label.c_str(), ImGuiTableColumnFlags_WidthFixed, width);
                } else {
                    ImGui::TableSetupColumn(column.Label.c_str(), ImGuiTableColumnFlags_WidthFixed, width);
                }
            }
            ImGui::TableSetupScrollFreeze(1, 1);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int hci = 0; hci < static_cast<int>(columns.size()); ++hci) {
                ImGui::TableSetColumnIndex(hci);
                const TicketGridColumn& hcol = columns[static_cast<size_t>(hci)];
                ImGui::PushID(hci);
                ImGui::TableHeader(hcol.Label.c_str());
                const TrackerField* hdrMeta =
                    (hcol.ColumnKind == TicketGridColumn::Kind::Id) ? nullptr : catalogIndex.Find(hcol.FieldId);
                DrawTicketGridHeaderContextMenu(hcol, hdrMeta);
                ImGui::PopID();
            }

            // Apply persisted sort from view when ImGui has no sort yet, or when we just switched view.
            if (activeViewForGrid && !activeViewForGrid->SortSpecs.empty()) {
                ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
                if (specs && (specs->SpecsCount == 0 || gridSortEnvironmentChanged)) {
                    for (size_t i = 0; i < activeViewForGrid->SortSpecs.size(); ++i) {
                        const ViewSortSpec& vs = activeViewForGrid->SortSpecs[i];
                        if (vs.Direction == 0)
                            continue;
                        int colIndex = -1;
                        for (size_t c = 0; c < columns.size(); ++c) {
                            if (columns[c].Key == vs.ColumnKey) {
                                colIndex = static_cast<int>(c);
                                break;
                            }
                        }
                        if (colIndex >= 0) {
                            ImGui::TableSetColumnSortDirection(colIndex, static_cast<ImGuiSortDirection>(vs.Direction),
                                                               (i > 0));
                        }
                    }
                }
            }
        }

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.sort");
            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (gridSortEnvironmentChanged) {
                d.cachedSortValid = false;
            }
            if (sortSpecs && sortSpecs->SpecsDirty) {
                d.cachedSortValid = false;
                sortSpecs->SpecsDirty = false;
            }
            const std::uint64_t activeTicketsRevision = app.GetActiveTicketsRevision();
            if (activeTicketsRevision != d.cachedSortTicketsRevision) {
                d.cachedSortValid = false;
            }
            const std::uint64_t catalogRevision = app.GetFieldCatalogRevision();
            if (catalogRevision != d.cachedSortCatalogRevision) {
                d.cachedSortValid = false;
            }

            if (sortSpecs && sortSpecs->SpecsCount > 0 && sortSpecs->Specs != nullptr) {
                std::string fingerprint;
                fingerprint.reserve(static_cast<size_t>(sortSpecs->SpecsCount) * 48);
                for (int s = 0; s < sortSpecs->SpecsCount; ++s) {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[s];
                    fingerprint += std::to_string(spec.ColumnIndex);
                    fingerprint.push_back(':');
                    fingerprint += std::to_string(static_cast<int>(spec.SortDirection));
                    fingerprint.push_back(':');
                    fingerprint += std::to_string(static_cast<int>(spec.SortOrder));
                    fingerprint.push_back('|');
                }

                if (!d.cachedSortValid || d.cachedSortFingerprint != fingerprint ||
                    d.cachedSortedIndices.size() != tickets.size()) {
                    d.cachedSortedIndices.resize(tickets.size());
                    for (size_t i = 0; i < tickets.size(); ++i)
                        d.cachedSortedIndices[i] = i;
                    const std::vector<CachedTicket>* ticketsPtr = &tickets;
                    const std::vector<TicketGridColumn>* columnsPtr = &columns;
                    const TrackerFieldCatalogIndex* catalogPtr = &catalogIndex;
                    std::stable_sort(d.cachedSortedIndices.begin(), d.cachedSortedIndices.end(),
                                     [ticketsPtr, columnsPtr, catalogPtr, sortSpecs](size_t ia, size_t ib) {
                                         const auto& tix = *ticketsPtr;
                                         const auto& cols = *columnsPtr;
                                         for (int s = 0; s < sortSpecs->SpecsCount; ++s) {
                                             const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[s];
                                             const int colIndex = spec.ColumnIndex;
                                             if (colIndex < 0 || colIndex >= static_cast<int>(cols.size()))
                                                 continue;
                                             const TicketGridColumn& col = cols[static_cast<size_t>(colIndex)];
                                             const int dir =
                                                 (spec.SortDirection == ImGuiSortDirection_Ascending) ? 1 : -1;
                                             if (col.ColumnKind == TicketGridColumn::Kind::Id) {
                                                 const bool less = CompareIssueKeyNatural(tix[ia].id, tix[ib].id);
                                                 if (less)
                                                     return dir > 0;
                                                 if (!CompareIssueKeyNatural(tix[ib].id, tix[ia].id))
                                                     continue;
                                                 return dir < 0;
                                             }
                                             const std::string aVal = tix[ia].GetFieldValue(col.FieldId);
                                             const std::string bVal = tix[ib].GetFieldValue(col.FieldId);
                                             const TrackerField* fieldMeta = catalogPtr->Find(col.FieldId);
                                             const int cmp =
                                                 CompareFieldValuesForSort(col.FieldId, fieldMeta, aVal, bVal, dir);
                                             if (cmp != 0)
                                                 return (cmp * dir) < 0;
                                         }
                                         return ia < ib;
                                     });
                    d.cachedSortFingerprint = std::move(fingerprint);
                    d.cachedSortValid = true;
                    d.cachedSortTicketsRevision = activeTicketsRevision;
                    d.cachedSortCatalogRevision = catalogRevision;
                }
            }
        }

        // Apply Filter
        static thread_local char lastFilter[128]{};
        static thread_local size_t lastTicketCount = 0;
        bool filterChanged = (std::strcmp(lastFilter, d.gridFilterBuf) != 0);
        if (filterChanged) {
            d.gridState.RectSel.ClearAll();
        }
        if (filterChanged || !d.cachedSortValid || tickets.size() != lastTicketCount) {
            d.filteredIndices.clear();
            const std::vector<size_t>& baseIndices = d.cachedSortedIndices.empty() ? std::vector<size_t>() : d.cachedSortedIndices;
            
            auto checkMatch = [&](size_t idx) {
                if (d.gridFilterBuf[0] == '\0') return true;
                const auto& t = tickets[idx];
                if (ContainsCaseInsensitive(t.id, d.gridFilterBuf)) return true;
                if (ContainsCaseInsensitive(t.GetFieldValue("summary"), d.gridFilterBuf)) return true;
                return false;
            };

            if (baseIndices.empty()) {
                for (size_t i = 0; i < tickets.size(); ++i) {
                    if (checkMatch(i)) d.filteredIndices.push_back(i);
                }
            } else {
                for (size_t idx : baseIndices) {
                    if (checkMatch(idx)) d.filteredIndices.push_back(idx);
                }
            }
            std::strncpy(lastFilter, d.gridFilterBuf, sizeof(lastFilter));
            lastTicketCount = tickets.size();
        }

        // Rectangular selection invalidation: anchor/extent are expressed in
        // current sort-order row indices, so any change to sort order or ticket
        // set must clear the selection to avoid nonsense highlights / copies.
        gridSortSig = ComputeGridSortSignature(d.cachedSortFingerprint, app.GetActiveTicketsRevision(), tickets.size());
        if (d.gridState.RectSel.HasAnySelection() && d.gridState.RectSel.SortSignature != gridSortSig) {
            d.gridState.RectSel.ClearAll();
        }

        // Shared per-cell hit-test + overlay helper. Implements Google-Sheets-
        // style selection gestures:
        //   - Key (ID) column: plain click selects a single row; drag selects a
        //     contiguous range; Shift+click extends from the anchor; Ctrl+click
        //     toggles individual rows (non-contiguous selections).
        //   - Data cells: click activates the row and clears row selections.
        // Dragging continues to update while the mouse stays pressed even if
        // the user releases the modifier key mid-drag.
        auto handleCellRectSel = [&](int rowIdx, int colIdx, const ImVec2& cellOrigin, float cellWidth,
                                     const ImVec2& groupMin, const ImVec2& groupMax, bool isIdCol,
                                     const std::string& issueId, bool activeIssueWasThisRow) {
            const bool shiftHeld = ImGuiEffectiveKeyShift();
            const bool ctrlHeld = ImGuiEffectiveKeyCtrl();
            const ImVec2 hitMin(cellOrigin.x, groupMin.y);
            const ImVec2 hitMax(cellOrigin.x + cellWidth, groupMax.y);
            auto& sel = d.gridState.RectSel;
            const bool hovering = ImGui::IsMouseHoveringRect(hitMin, hitMax, false);
            const bool mouseClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

            if (hovering && mouseClick && sel.Contains(rowIdx, colIdx)) {
                rectCellClickedThisFrame = true;
            }

            if (hovering && mouseClick) {
                if (!issueId.empty()) {
                    d.gridState.SetActiveIssue(issueId);
                }
                if (isIdCol) {
                    // Row-header gestures on the key column.
                    if (shiftHeld) {
                        const int anchorRow = (sel.PrimaryRow >= 0) ? sel.PrimaryRow : rowIdx;
                        std::set<int> baseBefore = sel.Rows;
                        const int lo = (anchorRow < rowIdx) ? anchorRow : rowIdx;
                        const int hi = (anchorRow > rowIdx) ? anchorRow : rowIdx;
                        for (int i = lo; i <= hi; ++i)
                            sel.Rows.insert(i);
                        sel.PrimaryRow = anchorRow;
                        sel.DragRowMode = true;
                        sel.DragStartRow = anchorRow;
                        sel.DragBaseRows = std::move(baseBefore);
                        sel.Active = false;
                        sel.Dragging = true;
                    } else if (ctrlHeld) {
                        auto it = sel.Rows.find(rowIdx);
                        const bool wasSelected = (it != sel.Rows.end());
                        if (!wasSelected) {
                            sel.Rows.insert(rowIdx);
                        } else {
                            sel.Rows.erase(it);
                            if (activeIssueWasThisRow) {
                                d.gridState.ActiveIssueId.clear();
                            }
                        }
                        sel.PrimaryRow = rowIdx;
                        sel.DragRowMode = false;
                        sel.DragStartRow = -1;
                        sel.DragBaseRows.clear();
                        sel.Active = false;
                        sel.Dragging = false;
                    } else {
                        sel.Rows.clear();
                        sel.Rows.insert(rowIdx);
                        sel.PrimaryRow = rowIdx;
                        sel.DragRowMode = true;
                        sel.DragStartRow = rowIdx;
                        sel.DragBaseRows.clear();
                        sel.Active = false;
                        sel.Dragging = true;
                    }
                    sel.SortSignature = gridSortSig;
                    rectCellClickedThisFrame = true;
                } else {
                    sel.ClearAll();
                    sel.SortSignature = gridSortSig;
                    rectCellClickedThisFrame = true;
                }
            } else if (sel.Dragging && hovering && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // Drag update: extend the active gesture while the mouse is held.
                if (sel.DragRowMode) {
                    const int lo = (sel.DragStartRow < rowIdx) ? sel.DragStartRow : rowIdx;
                    const int hi = (sel.DragStartRow > rowIdx) ? sel.DragStartRow : rowIdx;
                    sel.Rows = sel.DragBaseRows;
                    for (int i = lo; i <= hi; ++i)
                        sel.Rows.insert(i);
                } else {
                    sel.ExtentRow = rowIdx;
                    sel.ExtentCol = colIdx;
                }
            }

            if (sel.Contains(rowIdx, colIdx)) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(hitMin, hitMax, IM_COL32(66, 135, 245, 60));
                dl->AddRect(hitMin, hitMax, IM_COL32(66, 135, 245, 180));
            }
        };

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.rows");
            const std::vector<size_t>& indicesToUse = d.filteredIndices;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(indicesToUse.size()));
            while (clipper.Step()) {
                for (int clippedRow = clipper.DisplayStart; clippedRow < clipper.DisplayEnd; ++clippedRow) {
                    const size_t r = static_cast<size_t>(clippedRow);
                    const size_t ticketIndex = indicesToUse[r];
                    const CachedTicket& ticket = tickets[ticketIndex];
                    bool isActiveIssue = (d.gridState.ActiveIssueId == ticket.id);
                    const bool idKeySelectableSelected = d.gridState.RectSel.RowSelected(clippedRow);
                    const bool activeIssueWasThisRow = isActiveIssue;
                    if (isActiveIssue && !readOnlyMode) {
                        app.WarmIssueEditMetaAsync(ticket.id);
                    }
                    thread_local char rowPerfLabel[288];
                    const char* rowPerfScopeName = nullptr;
                    if (isActiveIssue) {
                        std::snprintf(rowPerfLabel, sizeof(rowPerfLabel), "activeProject:row[%zu] %s", r,
                                      ticket.id.c_str());
                        rowPerfLabel[sizeof(rowPerfLabel) - 1] = '\0';
                        rowPerfScopeName = rowPerfLabel;
                    }
                    SMATCHET_UI_PERF_SCOPE(rowPerfScopeName);
                    ImGui::TableNextRow();

                    // Status-based Row Highlighting
                    const std::string statusRaw = ticket.GetFieldValue("status");
                    std::string statusLower = ToLowerAsciiCopy(statusRaw);
                    ImVec4 statusColor = ImVec4(0, 0, 0, 0);
                    if (statusLower.find("done") != std::string::npos || statusLower.find("resolved") != std::string::npos) 
                        statusColor = SmatchetTheme::Colors::StatusDone;
                    else if (statusLower.find("progress") != std::string::npos) 
                        statusColor = SmatchetTheme::Colors::StatusInProgress;
                    else if (statusLower.find("todo") != std::string::npos || statusLower.find("open") != std::string::npos || statusLower.find("backlog") != std::string::npos)
                        statusColor = SmatchetTheme::Colors::StatusToDo;
                    else if (statusLower.find("block") != std::string::npos)
                        statusColor = SmatchetTheme::Colors::StatusBlocked;

                    if (statusColor.w > 0.0f) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(statusColor.x, statusColor.y, statusColor.z, 0.12f)));
                    }

                    for (int colIndex = 0; colIndex < static_cast<int>(columns.size()); ++colIndex) {
                        const auto& column = columns[static_cast<size_t>(colIndex)];
                        ImGui::TableSetColumnIndex(colIndex);

                        // Captured before any widget advances the cursor so rect-
                        // select hit boxes cover the entire column cell area
                        // (not just the rendered text / widget extent).
                        const ImVec2 cellOriginForSel = ImGui::GetCursorScreenPos();
                        const float cellWidthForSel = ImGui::GetContentRegionAvail().x;
                        ImVec2 cellGroupMin(0.0f, 0.0f);
                        ImVec2 cellGroupMax(0.0f, 0.0f);

                        if (column.ColumnKind == TicketGridColumn::Kind::Id) {
                            ImGui::BeginGroup();
                            if (ImGui::Selectable(ticket.id.c_str(), idKeySelectableSelected,
                                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                                if (!ImGuiEffectiveKeyCtrl()) {
                                    d.gridState.SetActiveIssue(ticket.id);
                                }
                                if (ImGui::IsMouseDoubleClicked(0)) {
                                    const std::string url = app.BuildIssueBrowseUrl(d.cfg, ticket.id);
                                    app.OpenUrl(url);
                                }
                            }
                            ImGui::EndGroup();
                            cellGroupMin = ImGui::GetItemRectMin();
                            cellGroupMax = ImGui::GetItemRectMax();
                            DrawGridCellRightClickPopups(BuildCellKey(ticket.id, "id"), ticket.id, std::string(),
                                                         column.Label, ticket.id, &app, &d, readOnlyMode);
                            handleCellRectSel(clippedRow, colIndex, cellOriginForSel, cellWidthForSel, cellGroupMin,
                                              cellGroupMax, true, ticket.id, activeIssueWasThisRow);
                            continue;
                        }

                        const std::string currentValue = ticket.GetFieldValue(column.FieldId);
                        const TrackerField* fieldMeta = catalogIndex.Find(column.FieldId);
                        const float cellStartY = ImGui::GetCursorScreenPos().y;
                        const float cellRightX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                        const float valueAvailWidth = cellRightX - ImGui::GetCursorScreenPos().x;
                        const std::string cellKey = BuildCellKey(ticket.id, column.FieldId);
                        const auto feedbackIt = d.cellFeedbackByKey.find(cellKey);
                        const bool isSavingThisCell = feedbackIt != d.cellFeedbackByKey.end() &&
                                                      feedbackIt->second.State == CellWriteState::Saving;

                        bool showBadge = false;
                        ImVec4 badgeColor(1.0f, 1.0f, 1.0f, 1.0f);
                        std::string badgeText;
                        std::string badgeTooltip;
                        if (feedbackIt != d.cellFeedbackByKey.end() &&
                            feedbackIt->second.State == CellWriteState::Error && !feedbackIt->second.Message.empty()) {
                            showBadge = true;
                            badgeColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                            badgeText = "!";
                            badgeTooltip = feedbackIt->second.Message;
                        } else if (feedbackIt != d.cellFeedbackByKey.end() &&
                                   feedbackIt->second.State == CellWriteState::Success) {
                            showBadge = true;
                            badgeColor = ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
                            badgeText = "OK";
                            badgeTooltip = "Saved";
                        }

                        if (isSavingThisCell) {
                            showBadge = true;
                            badgeColor = ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
                            badgeText = "...";
                            badgeTooltip = "Saving...";
                            ImGui::BeginGroup();
                            const std::string saveDisplay =
                                DisplayValueForTrackerDateField(column.FieldId, fieldMeta, currentValue);
                            const std::string* saveTip =
                                IsTrackerDateOrDateTimeField(column.FieldId, fieldMeta) ? &currentValue : nullptr;
                            RenderClippedFieldText(saveDisplay, valueAvailWidth, d.cfg.EnableFieldOverflowTooltips,
                                                   true, saveTip);
                            ImGui::EndGroup();
                            cellGroupMin = ImGui::GetItemRectMin();
                            cellGroupMax = ImGui::GetItemRectMax();
                            DrawGridCellRightClickPopups(cellKey, ticket.id, column.FieldId, column.Label, currentValue,
                                                         nullptr, nullptr, readOnlyMode);
                        } else {
                            const bool allowEditsForCell =
                                !readOnlyMode && column.NeedsAllowEditsCheck &&
                                app.CanEditFieldForIssue(ticket.id, column.FieldId, fieldMeta);
                            ImGui::BeginGroup();
                            TicketFieldEditor::RenderFieldCell(app, ticket, column, fieldMeta, currentValue,
                                                               valueAvailWidth, d.cfg.EnableFieldOverflowTooltips,
                                                               allowEditsForCell, d.gridState, pendingEdits,
                                                               d.jiraGridAsync);
                            ImGui::EndGroup();
                            cellGroupMin = ImGui::GetItemRectMin();
                            cellGroupMax = ImGui::GetItemRectMax();
                            DrawGridCellRightClickPopups(cellKey, ticket.id, column.FieldId, column.Label, currentValue,
                                                         nullptr, nullptr, readOnlyMode);
                        }

                        if (showBadge) {
                            const ImVec2 textSize = ImGui::CalcTextSize(badgeText.c_str());
                            const float badgeX = (std::max)(ImGui::GetCursorScreenPos().x, cellRightX - textSize.x);
                            ImGui::SetCursorScreenPos(ImVec2(badgeX, cellStartY));
                            ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
                            ImGui::TextUnformatted(badgeText.c_str());
                            ImGui::PopStyleColor();
                            if (!badgeTooltip.empty() && ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", badgeTooltip.c_str());
                            }
                        }

                        handleCellRectSel(clippedRow, colIndex, cellOriginForSel, cellWidthForSel, cellGroupMin,
                                          cellGroupMax, false, ticket.id, false);
                    }
                }
            }
        }

        if (!readOnlyMode) {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.newIssue");
            const CachedTicket* lastVisibleTicket = nullptr;
            if (!tickets.empty()) {
                size_t lastIndex = tickets.size() - 1;
                if (!d.filteredIndices.empty()) {
                    lastIndex = d.filteredIndices.back();
                }
                if (lastIndex < tickets.size()) {
                    lastVisibleTicket = &tickets[lastIndex];
                }
            }
            RenderNewIssueDraftRow(app, d, columns, d.cfg, lastVisibleTicket);
        }

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.post");
            RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGui::GetCurrentTable(), d);

            // Persist column widths and sort specs back to the active view.
            if (activeViewForGrid) {
                ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
                if (mutableActive) {
                    bool metaChanged = false;
                    ImGuiTable* table = ImGui::GetCurrentTable();
                    if (table) {
                        // Persist column widths only; column order is controlled via the Views dashboard.
                        for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
                            const std::string& key = columns[static_cast<size_t>(i)].Key;
                            const float width = (i < table->ColumnsCount) ? table->Columns[i].WidthGiven : 0.0f;
                            const auto oldIt = mutableActive->ColumnWidths.find(key);
                            const float oldWidth = (oldIt == mutableActive->ColumnWidths.end()) ? 0.0f : oldIt->second;
                            if (std::abs(oldWidth - width) > 0.5f) {
                                mutableActive->ColumnWidths[key] = width;
                                metaChanged = true;
                            }
                        }
                    }
                    // Re-fetch sort specs from the table right before persisting so we use current state.
                    ImGuiTableSortSpecs* currentSortSpecs = ImGui::TableGetSortSpecs();
                    if (currentSortSpecs && currentSortSpecs->SpecsCount > 0 && currentSortSpecs->Specs != nullptr) {
                        std::vector<ViewSortSpec> newSortSpecs;
                        for (int s = 0; s < currentSortSpecs->SpecsCount; ++s) {
                            const int colIndex = currentSortSpecs->Specs[s].ColumnIndex;
                            if (colIndex >= 0 && colIndex < static_cast<int>(columns.size()) &&
                                currentSortSpecs->Specs[s].SortDirection != ImGuiSortDirection_None) {
                                ViewSortSpec vs;
                                vs.ColumnKey = columns[static_cast<size_t>(colIndex)].Key;
                                vs.Direction = static_cast<int>(currentSortSpecs->Specs[s].SortDirection);
                                newSortSpecs.push_back(vs);
                            }
                        }
                        if (newSortSpecs != mutableActive->SortSpecs) {
                            mutableActive->SortSpecs = std::move(newSortSpecs);
                            metaChanged = true;
                        }
                    } else {
                        // No active sort in the table; clear stored sort so it doesn't persist stale state.
                        if (!mutableActive->SortSpecs.empty()) {
                            mutableActive->SortSpecs.clear();
                            metaChanged = true;
                        }
                    }
                    if (metaChanged) {
                        const auto now = std::chrono::steady_clock::now();
                        const auto deadline = now + kViewStateSaveDebounceLocal;
                        d.pendingViewStateSave = true;
                        d.pendingViewStateSaveAt = std::max(d.pendingViewStateSaveAt, deadline);
                    }
                }
            }
        }
        // After full table layout: union outer + inner clip so empty scroll body
        // and padding still count as "inside grid" (OuterRect alone missed H5).
        if (ImGuiContext* g = ImGui::GetCurrentContext()) {
            if (ImGuiTable* tb = g->CurrentTable) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImRect hit(tb->OuterRect);
                    hit.Add(tb->InnerClipRect);
                    if (hit.Contains(ImGui::GetIO().MousePos)) {
                        ticketGridLeftClickInsideTableHit = true;
                        rectCellClickedThisFrame = true;
                    }
                }
            }
        }
        ImGui::EndTable();
    }

    // Google-Sheets-style selection: end drag on mouse release, clear when the
    // user clicks outside the current selection, and service Ctrl+C (copy as
    // TSV), Escape (clear), and Shift+Space (select whole row of active cell)
    // while the Active Project window is focused. Only runs when the grid was
    // drawn this frame (otherwise there is no valid sort signature).
    if (!columns.empty()) {
        SMATCHET_UI_PERF_SCOPE("activeProject:grid.rectSel.keys");
        auto& sel = d.gridState.RectSel;
        if (sel.Dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            sel.Dragging = false;
            sel.DragRowMode = false;
            sel.DragBaseRows.clear();
        }
        const ImGuiIO& io = ImGui::GetIO();
        const bool effShift = ImGuiEffectiveKeyShift();
        const bool effCtrl = ImGuiEffectiveKeyCtrl();
        const bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                                          ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        // Click outside any currently-selected cell (without Shift/Ctrl) clears
        // the entire selection. Ctrl preserves selection for toggling; Shift
        // preserves it for range extension.
        if ((sel.HasAnySelection() || !d.gridState.ActiveIssueId.empty()) && !effShift && !effCtrl && windowHovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !rectCellClickedThisFrame &&
            !ticketGridLeftClickInsideTableHit) {
            sel.ClearAll();
            d.gridState.ActiveIssueId.clear();
        }
        const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (windowFocused && sel.HasAnySelection()) {
            if (!io.WantTextInput && effCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                CopyGridRectAsTsv(tickets, d.filteredIndices, columns, catalogIndex, sel);
            }
            if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                sel.ClearAll();
            }
        }
        // Shift+Space: promote the row containing the active cell to a whole-
        // row selection (in addition to whatever is already selected).
        if (windowFocused && !io.WantTextInput && effShift && !effCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            int activeRow = -1;
            if (sel.PrimaryRow >= 0) {
                activeRow = sel.PrimaryRow;
            } else if (sel.Active) {
                activeRow = sel.AnchorRow;
            } else if (!d.gridState.ActiveIssueId.empty()) {
                const auto& indices = d.filteredIndices;
                for (size_t i = 0; i < indices.size(); ++i) {
                    const size_t ti = indices[i];
                    if (ti < tickets.size() && tickets[ti].id == d.gridState.ActiveIssueId) {
                        activeRow = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (activeRow >= 0) {
                sel.Rows.insert(activeRow);
                sel.PrimaryRow = activeRow;
                sel.Active = false;
                sel.SortSignature = gridSortSig;
            }
        }
    }

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:edits.queue");
        // Keep queued edits latest-per-cell (drop older queued item for same cell).
        if (!readOnlyMode) {
            for (const auto& edit : pendingEdits) {
                const std::string editKey = BuildCellKey(edit.IssueId, edit.Field.Id);
                for (auto it = d.queuedFieldEdits.begin(); it != d.queuedFieldEdits.end();) {
                    if (BuildCellKey(it->IssueId, it->Field.Id) == editKey) {
                        it = d.queuedFieldEdits.erase(it);
                    } else {
                        ++it;
                    }
                }
                d.queuedFieldEdits.push_back(edit);
            }
        } else if (!pendingEdits.empty()) {
            d.gridEditSuccess.clear();
            d.gridEditError = "Edit skipped: Jira is in read-only mode.";
        }

        if (readOnlyMode) {
            d.queuedFieldEdits.clear();
        }
    }

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:edits.inFlight");
        if (!readOnlyMode && !d.hasInFlightEdit && !d.queuedFieldEdits.empty()) {
            d.inFlightEdit = d.queuedFieldEdits.front();
            d.queuedFieldEdits.pop_front();
            d.inFlightOriginalEstimateSnapshot.clear();
            d.inFlightRemainingEstimateSnapshot.clear();
            d.inFlightIssueTypeKeySnapshot.clear();
            const auto snapshotIt = std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) {
                return ticket.id == d.inFlightEdit.IssueId;
            });
            if (snapshotIt != tickets.end()) {
                d.inFlightOriginalEstimateSnapshot = snapshotIt->GetFieldValue("timeoriginalestimate");
                d.inFlightRemainingEstimateSnapshot = snapshotIt->GetFieldValue("timeestimate");
                d.inFlightIssueTypeKeySnapshot = ToLowerAsciiCopy(TrimCopy(snapshotIt->GetFieldValue("issuetype")));
            }
            d.hasInFlightEdit = true;
            d.inFlightCommitStarted = false;
            d.inFlightDelayFrames = 1;
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Saving;
            feedback.Message = "Saving to Jira...";
            feedback.FramesRemaining = 0;
            d.cellFeedbackByKey[BuildCellKey(d.inFlightEdit.IssueId, d.inFlightEdit.Field.Id)] = feedback;
        }

        if (d.hasInFlightEdit) {
            if (d.inFlightDelayFrames > 0) {
                --d.inFlightDelayFrames;
            } else if (!d.inFlightCommitStarted) {
                const PendingFieldEdit edit = d.inFlightEdit;
                const std::string originalEstimateSnapshot = d.inFlightOriginalEstimateSnapshot;
                const std::string remainingEstimateSnapshot = d.inFlightRemainingEstimateSnapshot;
                const std::string issueTypeKeySnapshot = d.inFlightIssueTypeKeySnapshot;
                d.inFlightCommitFuture =
                    std::async(std::launch::async, [&app, edit, originalEstimateSnapshot, remainingEstimateSnapshot,
                                                    issueTypeKeySnapshot]() {
                        FieldEditCommitResult result;
                        result.CommitKind = FieldEditCommitResult::Kind::Failed;
                        if (app.SubmitFieldEditNetworkOnly(edit.IssueId, edit.Field, edit.Values,
                                                           originalEstimateSnapshot, remainingEstimateSnapshot,
                                                           issueTypeKeySnapshot, result.ApplyResult)) {
                            result.Ok = true;
                            result.CommitKind = FieldEditCommitResult::Kind::SavedOnline;
                            result.Error.clear();
                            return result;
                        }
                        result.Error = result.ApplyResult.Error;
                        if (IsJiraTransportErrorText(result.ApplyResult.Error) &&
                            AppController::FieldEditSupportsOfflineQueue(edit.Field)) {
                            AppController::FieldEditResult prepared;
                            std::string payloadJson;
                            std::string prepErr;
                            if (app.TryPrepareOfflineFieldEdit(edit.IssueId, edit.Field, edit.Values,
                                                               originalEstimateSnapshot, remainingEstimateSnapshot,
                                                               issueTypeKeySnapshot, prepared, payloadJson, prepErr)) {
                                result.ApplyResult = std::move(prepared);
                                result.QueuedFieldsPayloadJson = std::move(payloadJson);
                                result.CommitKind = FieldEditCommitResult::Kind::QueuedOffline;
                                result.Ok = true;
                                result.Error.clear();
                                return result;
                            }
                            if (!prepErr.empty()) {
                                result.Error = prepErr;
                            }
                        }
                        result.Ok = false;
                        result.CommitKind = FieldEditCommitResult::Kind::Failed;
                        return result;
                    });
                d.inFlightCommitStarted = true;
            } else {
                const std::string editKey = BuildCellKey(d.inFlightEdit.IssueId, d.inFlightEdit.Field.Id);
                if (d.inFlightCommitFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                    // Async Jira commit still running; keep UI responsive.
                } else {
                    const FieldEditCommitResult result = d.inFlightCommitFuture.get();
                    std::string applyError;
                    if (result.CommitKind == FieldEditCommitResult::Kind::QueuedOffline) {
                        std::string qerr;
                        const std::int64_t qid = app.QueueFieldEditOffline(
                            d.inFlightEdit.IssueId, d.inFlightEdit.Field.Id, result.QueuedFieldsPayloadJson, qerr);
                        if (qid <= 0) {
                            SmatchetToastManager::Instance().Push("Offline Error", qerr.empty() ? "Failed to queue offline field edit." : qerr, ToastType::Error);
                            CellWriteFeedback feedback;
                            feedback.State = CellWriteState::Error;
                            feedback.Message = qerr;
                            feedback.FramesRemaining = 0;
                            d.cellFeedbackByKey[editKey] = feedback;
                        } else if (!app.ApplyFieldEditResult(d.inFlightEdit.IssueId, result.ApplyResult, applyError)) {
                            SmatchetToastManager::Instance().Push("Apply Error", applyError.empty() ? "Failed to apply queued field edit." : applyError, ToastType::Error);
                            CellWriteFeedback feedback;
                            feedback.State = CellWriteState::Error;
                            feedback.Message = applyError;
                            feedback.FramesRemaining = 0;
                            d.cellFeedbackByKey[editKey] = feedback;
                        } else {
                            SmatchetToastManager::Instance().Push("Queued Offline", "Field edit will sync when Jira is reachable.", ToastType::Info);
                            CellWriteFeedback feedback;
                            feedback.State = CellWriteState::Success;
                            feedback.Message = "Queued";
                            feedback.FramesRemaining = 240;
                            d.cellFeedbackByKey[editKey] = feedback;
                        }
                    } else if (result.CommitKind == FieldEditCommitResult::Kind::SavedOnline) {
                        const bool applied =
                            app.ApplyFieldEditResult(d.inFlightEdit.IssueId, result.ApplyResult, applyError);
                        if (!applied) {
                            SmatchetToastManager::Instance().Push("Save Error", applyError.empty() ? "Failed to apply saved field update." : applyError, ToastType::Error);
                            CellWriteFeedback feedback;
                            feedback.State = CellWriteState::Error;
                            feedback.Message = applyError;
                            feedback.FramesRemaining = 0;
                            d.cellFeedbackByKey[editKey] = feedback;
                        } else {
                            SmatchetToastManager::Instance().Push("Success", "Field update saved to Jira.", ToastType::Success);
                            CellWriteFeedback feedback;
                            feedback.State = CellWriteState::Success;
                            feedback.Message = "Saved";
                            feedback.FramesRemaining = 180;
                            d.cellFeedbackByKey[editKey] = feedback;
                        }
                    } else {
                        d.gridEditError =
                            result.Error.empty() ? std::string("Failed to save Jira field update.") : result.Error;
                        d.gridEditSuccess.clear();
                        CellWriteFeedback feedback;
                        feedback.State = CellWriteState::Error;
                        feedback.Message = d.gridEditError;
                        feedback.FramesRemaining = 0;
                        d.cellFeedbackByKey[editKey] = feedback;
                    }
                    d.hasInFlightEdit = false;
                    d.inFlightCommitStarted = false;
                }
            }
        }
    }

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:cellFeedback");
        for (auto it = d.cellFeedbackByKey.begin(); it != d.cellFeedbackByKey.end();) {
            if (it->second.State == CellWriteState::Success && it->second.FramesRemaining > 0) {
                --it->second.FramesRemaining;
            }

            if (it->second.State == CellWriteState::Success && it->second.FramesRemaining <= 0) {
                it = d.cellFeedbackByKey.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!pendingEdits.empty()) {
        d.gridEditError.clear();
    }
    ImGui::End();
}
