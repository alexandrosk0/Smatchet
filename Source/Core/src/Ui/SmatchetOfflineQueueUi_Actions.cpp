#include "Ui/SmatchetOfflineQueueActionsUi.h"

#include "AiChatTimestamp.h"
#include "CacheBackendKeyPure.h"
#include "Interfaces/IAppPendingActions.h"
#include "PendingActionPolicyPure.h"
#include "PendingActionTypes.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/// One table row, built when the queue snapshot changes (never per frame).
struct ActionRowView {
    bool Dead = false;
    bool NeedsReview = false;
    std::int64_t Id = 0;     ///< pending_actions.id (queued) or pending_actions_dead.original_id (failed)
    std::int64_t DeadId = 0; ///< pending_actions_dead.dead_id (failed rows)
    std::string Kind;        ///< display name
    std::string IssueKey;
    std::string BackendKey; ///< the tracker site the action was written for
    std::string State;      ///< display text
    int Attempts = 0;
    std::string LastError;
    std::int64_t CreatedAtSec = 0;
    std::string Preview; ///< what the action says (a comment's first line)
};

std::shared_ptr<const PendingActionsSnapshot> s_viewSource;
std::vector<ActionRowView> s_views;
std::int64_t s_discardId = 0;           ///< queued row awaiting the discard confirmation, or 0
bool s_discardConfirmRequested = false; ///< open the confirmation at the popup's own ID scope
int s_heldRowsDrawn = 0;                ///< held rows drawn since the last test reset

const char* KindLabel(const std::string& wire) {
    PendingActionKind kind;
    if (!ParsePendingActionKind(wire, kind)) {
        return "Unknown action";
    }
    switch (kind) {
    case PendingActionKind::CommentAdd:
        return "Comment";
    case PendingActionKind::WorklogAdd:
        return "Worklog";
    case PendingActionKind::WatchAdd:
        return "Watch";
    }
    return "Unknown action";
}

const char* StateLabel(const std::string& state) {
    if (state == PendingActionState::kSending) {
        return "Sending";
    }
    if (state == PendingActionState::kAmbiguous) {
        return "Checking (response lost)";
    }
    if (state == PendingActionState::kNeedsReview) {
        return "Needs review";
    }
    return "Waiting to sync";
}

// `text` on one line, at most ~80 characters.
std::string OneLinePreview(const std::string& text) {
    const std::size_t kMaxChars = 80;
    std::string oneLine;
    for (const char c : text) {
        oneLine.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
        if (oneLine.size() >= kMaxChars) {
            oneLine += "...";
            break;
        }
    }
    return oneLine;
}

// What the action says: a comment's body, a worklog's time and description; empty for a watch.
std::string ActionPreview(const std::string& wire, const std::string& payloadJson) {
    PendingActionKind kind;
    if (!ParsePendingActionKind(wire, kind)) {
        return std::string();
    }
    if (kind == PendingActionKind::CommentAdd) {
        std::string body;
        std::int64_t queuedAt = 0;
        return smatchet::pendingaction::ParseCommentActionPayload(payloadJson, body, queuedAt) ? OneLinePreview(body)
                                                                                               : std::string();
    }
    smatchet::pendingaction::WorklogActionPayload worklog;
    if (kind == PendingActionKind::WorklogAdd &&
        smatchet::pendingaction::ParseWorklogActionPayload(payloadJson, worklog)) {
        return OneLinePreview(worklog.Description.empty() ? worklog.TimeSpent
                                                          : worklog.TimeSpent + " \xE2\x80\x94 " + worklog.Description);
    }
    return std::string();
}

ActionRowView MakeRowView(const PendingActionRecord& row) {
    ActionRowView v;
    v.Id = row.Id;
    v.Kind = KindLabel(row.Kind);
    v.IssueKey = row.IssueKey;
    v.BackendKey = row.BackendKey;
    v.Attempts = row.Attempts;
    v.LastError = row.LastError;
    v.CreatedAtSec = row.CreatedAtEpochSec;
    v.Preview = ActionPreview(row.Kind, row.PayloadJson);
    return v;
}

void RebuildViews(const std::shared_ptr<const PendingActionsSnapshot>& snap) {
    std::vector<ActionRowView> views;
    views.reserve(snap->Pending.size() + snap->Dead.size());
    for (const PendingActionRecord& row : snap->Pending) {
        ActionRowView v = MakeRowView(row);
        v.NeedsReview = row.State == PendingActionState::kNeedsReview;
        v.State = StateLabel(row.State);
        views.push_back(std::move(v));
    }
    for (const DeadPendingAction& dead : snap->Dead) {
        ActionRowView v = MakeRowView(dead.Row);
        v.Dead = true;
        v.DeadId = dead.DeadId;
        v.State = "Failed: " + dead.TerminalReason;
        views.push_back(std::move(v));
    }
    s_views = std::move(views);
    s_viewSource = snap;
}

void DrawRowActions(IAppPendingActions& app, const ActionRowView& v) {
    if (v.Dead) {
        if (ImGui::SmallButton("Retry")) {
            app.RestoreDeadPendingActions({v.Id});
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
            app.DeleteDeadPendingActions({v.DeadId});
        }
        return;
    }
    if (v.NeedsReview) {
        if (ImGui::SmallButton("Send again")) {
            app.SendPendingActionAgain(v.Id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Only if the issue does not already show it: the first send may have landed.");
        }
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Discard")) {
        s_discardId = v.Id;
        s_discardConfirmRequested = true; // opened by DrawDiscardConfirm: a row's ID scope differs
    }
}

void DrawRow(IAppPendingActions& app, const ActionRowView& v, bool held, std::int64_t nowMs) {
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v.Kind.c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v.IssueKey.c_str());
    ImGui::TableNextColumn();
    if (held) {
        ImGui::TextDisabled("%s", SmatchetOfflineQueueActionsUi::HeldStateLabel());
        if (ImGui::IsItemHovered()) {
            SmatchetOfflineQueueActionsUi::ShowHeldTooltip(v.BackendKey);
        }
        SmatchetOfflineQueueActionsUi::NoteHeldRowDrawn();
    } else if (v.Dead) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextUnformatted(v.State.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextUnformatted(v.State.c_str());
    }
    ImGui::TableNextColumn();
    ImGui::Text("%d", v.Attempts);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v.LastError.empty() ? "-" : v.LastError.c_str());
    if (!v.LastError.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", v.LastError.c_str());
    }
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(smatchet::ai::FormatRelativeTime(nowMs, v.CreatedAtSec * 1000).c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v.Preview.c_str());
    ImGui::TableNextColumn();
    DrawRowActions(app, v);
}

void DrawDiscardConfirm(IAppPendingActions& app) {
    if (s_discardConfirmRequested) {
        s_discardConfirmRequested = false;
        ImGui::OpenPopup("Discard queued change?###pendingActionDiscard");
    }
    if (!ImGui::BeginPopupModal("Discard queued change?###pendingActionDiscard", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("This change has not reached the tracker. Discarding deletes the only copy.");
    if (ImGui::Button("Discard")) {
        if (s_discardId != 0) {
            app.DiscardPendingActions({s_discardId});
        }
        s_discardId = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep")) {
        s_discardId = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

namespace SmatchetOfflineQueueActionsUi {

const char* HeldStateLabel() { return "Held"; }

void ShowHeldTooltip(const std::string& backendKey) {
    const std::string site = smatchet::cache_keys::DescribeCacheBackendKey(backendKey);
    ImGui::SetTooltip("Queued for %s.\nIt is sent once a grid for that site is open again; Discard deletes it.",
                      site.empty() ? "an unknown site" : site.c_str());
}

void NoteHeldRowDrawn() { ++s_heldRowsDrawn; }

int HeldRowsDrawnForTests() { return s_heldRowsDrawn; }

void ResetHeldRowsDrawnForTests() { s_heldRowsDrawn = 0; }

bool HasRows(const IAppPendingActions& app) {
    const std::shared_ptr<const PendingActionsSnapshot> snap = app.GetPendingActionsSnapshot();
    return !snap->Pending.empty() || !snap->Dead.empty();
}

void Draw(IAppPendingActions& app) {
    const std::shared_ptr<const PendingActionsSnapshot> snap = app.GetPendingActionsSnapshot();
    if (snap != s_viewSource) {
        RebuildViews(snap);
    }
    if (s_views.empty()) {
        return;
    }
    ImGui::PushID("pendingActions");
    ImGui::SeparatorText("Other queued changes");
    struct ColumnSpec {
        const char* Name;
        float Width; ///< 0 = stretch
    };
    static const ColumnSpec kColumns[] = {
        {"Kind", 80.0f},     {"Issue", 90.0f},  {"State", 170.0f},  {"Retries", 60.0f}, {"Last Error Reason", 180.0f},
        {"Created", 100.0f}, {"Content", 0.0f}, {"Actions", 150.0f}};
    const int columnCount = static_cast<int>(sizeof(kColumns) / sizeof(kColumns[0]));
    if (ImGui::BeginTable("pendingActionsTbl", columnCount,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_ScrollX | ImGuiTableFlags_NoSavedSettings)) {
        for (const ColumnSpec& column : kColumns) {
            ImGui::TableSetupColumn(column.Name,
                                    column.Width > 0.0f ? ImGuiTableColumnFlags_WidthFixed
                                                        : ImGuiTableColumnFlags_WidthStretch,
                                    column.Width);
        }
        ImGui::TableHeadersRow();
        const std::int64_t nowMs = smatchet::ai::NowUnixMs();
        // Held-ness follows the live panes, which change without a queue change: read once per draw.
        const std::vector<std::string> liveKeys = app.LiveCacheBackendKeys();
        for (size_t i = 0; i < s_views.size(); ++i) {
            const ActionRowView& v = s_views[i];
            const bool held = !v.Dead && smatchet::cache_keys::IsHeldCacheKey(v.BackendKey, liveKeys);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            DrawRow(app, v, held, nowMs);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawDiscardConfirm(app);
    ImGui::PopID();
}

} // namespace SmatchetOfflineQueueActionsUi
