#include "SmatchetAgentHandoffUi.h"

#include "AgenticHandoffController.h"
#include "AppController.h"
#include "HarnessRunState.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace SmatchetAgentHandoffUi {

namespace {

// Refresh cadence — 1 Hz polling matches the T6 proposals UI. State
// transitions for handoffs arrive at human + network timescale; per-frame
// snapshots would copy under the controller mutex for zero perceptible
// benefit.
const auto kRefreshInterval = std::chrono::milliseconds(1000);

using smatchet::agentic::AgenticHandoffController;

// TU-static cache + last-refresh timestamp. Lives at TU scope (not
// UiDrawSession) because the panel owns its own polling cadence — a stray
// Render path with no open window must not write back into d.* state. The
// selected-id and per-handoff reply scratch buffers also live here so they
// persist across the open / close lifecycle of the window.
std::vector<AgenticHandoffController::ActiveHandoff> g_handoffsCache;
std::chrono::steady_clock::time_point g_lastRefreshAt{};
bool g_initialFetchDone = false;

// Selected handoff id. Zero means "no row selected" — the detail pane shows
// a hint string in that case.
std::int64_t g_selectedId = 0;

// Per-handoff clarification reply scratch buffer. ImGui's InputTextMultiline
// needs a stable backing buffer across frames; keyed by proposalId so each
// row has its own draft that persists while the user types + scrolls + checks
// other rows. Cleared on Submit success (next refresh) — see SubmitClarification.
std::unordered_map<std::int64_t, std::vector<char>> g_replyBufs;

// Per-handoff transient error/info status surfaced under the buttons. Cleared
// the next time the user clicks a button on the same row.
std::unordered_map<std::int64_t, std::string> g_perRowStatus;

// Initial size for a freshly-allocated reply buffer. ~4 KB matches the offline-
// queue free-text editor's pattern; ImGui's InputTextMultiline ResizeCallback
// path grows the vector when the user types past the cap.
constexpr size_t kReplyBufInitialSize = 4096;

std::vector<char>& EnsureReplyBuf(std::int64_t id) {
    auto& buf = g_replyBufs[id];
    if (buf.empty()) {
        buf.assign(kReplyBufInitialSize, '\0');
    }
    return buf;
}

bool ShouldRefresh() {
    if (!g_initialFetchDone) {
        if (g_lastRefreshAt == std::chrono::steady_clock::time_point{}) {
            return true;
        }
        return (std::chrono::steady_clock::now() - g_lastRefreshAt) >= kRefreshInterval;
    }
    return (std::chrono::steady_clock::now() - g_lastRefreshAt) >= kRefreshInterval;
}

void RefreshFromController(AppController& app) {
    auto* controller = app.GetAgenticHandoffController();
    if (controller == nullptr) {
        g_handoffsCache.clear();
        g_lastRefreshAt = std::chrono::steady_clock::now();
        return;
    }
    g_handoffsCache = controller->SnapshotActive();
    // Stable sort by startedAtSec ascending — oldest first so a long-running
    // handoff stays anchored at the top while newer ones slot in below. The
    // UI table renders rows in vector order.
    std::sort(g_handoffsCache.begin(), g_handoffsCache.end(),
              [](const AgenticHandoffController::ActiveHandoff& a,
                 const AgenticHandoffController::ActiveHandoff& b) { return a.startedAtSec < b.startedAtSec; });
    g_lastRefreshAt = std::chrono::steady_clock::now();
    g_initialFetchDone = true;
}

// Resolve the cached handoff row for the selected id. Returns nullptr if the
// id has dropped out of the active set (handoff reached a terminal state and
// was filtered out by SnapshotActive). The caller renders a "selected
// handoff is no longer active" hint in that branch.
const AgenticHandoffController::ActiveHandoff* FindSelectedInCache() {
    if (g_selectedId == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < g_handoffsCache.size(); ++i) {
        if (g_handoffsCache[i].proposalId == g_selectedId) {
            return &g_handoffsCache[i];
        }
    }
    return nullptr;
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
#endif

// Cross-platform "open this URL / directory in the user's default handler".
// Mirrors the precedent in BlameAnalysisUi_Launch.cpp for `ShellExecuteW` on
// Windows; non-Windows falls back to LOG_INFO so the dual-target build still
// links (DX12 builds Source_Core/ but the panel only renders Standalone — the
// SmatchetCore_DX12 compile tripwire just needs a symbol to exist).
bool OpenInShell(const std::string& target) {
#ifdef _WIN32
    const std::wstring wide = Utf8ToWide(target);
    if (wide.empty()) {
        return false;
    }
    SetLastError(0);
    const INT_PTR r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) {
        LOG_WARN("SmatchetAgentHandoffUi::OpenInShell: ShellExecuteW failed, result=%lld GetLastError=%lu",
                 static_cast<long long>(r), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    return true;
#else
    LOG_INFO("SmatchetAgentHandoffUi::OpenInShell (non-Windows stub): would open %s", target.c_str());
    return false;
#endif
}

// Localised display string for each RunState. Wraps RunStateToString (stable
// English literal — the FSM contract) in a SmatchetLocalization lookup so
// fr-FR users see translated copy. The English fallback matches the FSM
// literal verbatim.
const char* RunStateDisplay(CodingHarness::RunState s) {
    switch (s) {
    case CodingHarness::RunState::Pending:
        return SmatchetLocalization::T("agent.handoffs.state.pending", "Pending");
    case CodingHarness::RunState::Spawning:
        return SmatchetLocalization::T("agent.handoffs.state.spawning", "Spawning");
    case CodingHarness::RunState::Running:
        return SmatchetLocalization::T("agent.handoffs.state.running", "Running");
    case CodingHarness::RunState::AwaitingUser:
        return SmatchetLocalization::T("agent.handoffs.state.awaitingUser", "AwaitingUser");
    case CodingHarness::RunState::PrOpen:
        return SmatchetLocalization::T("agent.handoffs.state.prOpen", "PrOpen");
    case CodingHarness::RunState::Iterating:
        return SmatchetLocalization::T("agent.handoffs.state.iterating", "Iterating");
    case CodingHarness::RunState::Complete:
        return SmatchetLocalization::T("agent.handoffs.state.complete", "Complete");
    case CodingHarness::RunState::Failed:
        return SmatchetLocalization::T("agent.handoffs.state.failed", "Failed");
    case CodingHarness::RunState::Cancelled:
        return SmatchetLocalization::T("agent.handoffs.state.cancelled", "Cancelled");
    }
    return "Unknown";
}

// Cancellable predicate — Running and AwaitingUser are the FSM states that
// support a Cancel transition. Terminal states (Complete / Failed / Cancelled)
// are filtered out of SnapshotActive entirely; the remaining non-cancellable
// active states (Pending / Spawning) reject Cancel at the FSM boundary, so we
// disable the button locally to avoid surfacing a 'transition rejected' error.
bool IsCancellable(CodingHarness::RunState s) {
    return s == CodingHarness::RunState::Running || s == CodingHarness::RunState::AwaitingUser ||
           s == CodingHarness::RunState::PrOpen || s == CodingHarness::RunState::Iterating;
}

void SubmitClarification(AppController& app, std::int64_t id, const char* reply) {
    auto* controller = app.GetAgenticHandoffController();
    if (controller == nullptr) {
        g_perRowStatus[id] = SmatchetLocalization::T("agent.handoffs.error.controllerUnavailable",
                                                    "Handoff controller unavailable.");
        return;
    }
    std::string err;
    if (!controller->ProvideClarification(id, reply ? reply : "", err)) {
        LOG_WARN("SmatchetAgentHandoffUi: ProvideClarification id=%lld failed: %s", static_cast<long long>(id),
                 err.c_str());
        g_perRowStatus[id] = err;
        return;
    }
    LOG_INFO("SmatchetAgentHandoffUi: clarification submitted for handoff id=%lld", static_cast<long long>(id));
    g_perRowStatus[id].clear();
    // Reset the per-row reply scratch and force a refresh next frame so the
    // detail pane reflects the post-AwaitingUser state without waiting for
    // the 1 Hz tick.
    auto it = g_replyBufs.find(id);
    if (it != g_replyBufs.end()) {
        std::fill(it->second.begin(), it->second.end(), '\0');
    }
    g_initialFetchDone = false;
}

void CancelHandoff(AppController& app, std::int64_t id) {
    auto* controller = app.GetAgenticHandoffController();
    if (controller == nullptr) {
        g_perRowStatus[id] = SmatchetLocalization::T("agent.handoffs.error.controllerUnavailable",
                                                    "Handoff controller unavailable.");
        return;
    }
    std::string err;
    if (!controller->Cancel(id, err)) {
        LOG_WARN("SmatchetAgentHandoffUi: Cancel id=%lld failed: %s", static_cast<long long>(id), err.c_str());
        g_perRowStatus[id] = err;
        return;
    }
    LOG_INFO("SmatchetAgentHandoffUi: cancel issued for handoff id=%lld", static_cast<long long>(id));
    g_perRowStatus[id].clear();
    g_initialFetchDone = false;
}

void RenderTable(AppController& app) {
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingStretchProp;
    // Reserve room below the table for the detail pane. The outer window
    // already constrains height; the inner table takes the upper ~half.
    const ImVec2 outerSize(0.0f, ImGui::GetContentRegionAvail().y * 0.50f);
    if (!ImGui::BeginTable("##HandoffsTable", 5, tableFlags, outerSize)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(SmatchetLocalization::T("agent.handoffs.table.id", "ID"),
                            ImGuiTableColumnFlags_WidthFixed, 64.0f);
    ImGui::TableSetupColumn(SmatchetLocalization::T("agent.handoffs.table.issue", "Issue"),
                            ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn(SmatchetLocalization::T("agent.handoffs.table.state", "State"),
                            ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn(SmatchetLocalization::T("agent.handoffs.table.prUrl", "PR URL"),
                            ImGuiTableColumnFlags_WidthStretch, 2.5f);
    ImGui::TableSetupColumn(SmatchetLocalization::T("agent.handoffs.table.actions", "Actions"),
                            ImGuiTableColumnFlags_WidthFixed, 220.0f);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < g_handoffsCache.size(); ++i) {
        const AgenticHandoffController::ActiveHandoff& row = g_handoffsCache[i];
        ImGui::PushID(static_cast<int>(row.proposalId));
        ImGui::TableNextRow();

        // ID column — Selectable spans the row so click-anywhere selects it.
        ImGui::TableNextColumn();
        const bool isSelected = (g_selectedId == row.proposalId);
        char idLabel[32];
        std::snprintf(idLabel, sizeof(idLabel), "%lld", static_cast<long long>(row.proposalId));
        if (ImGui::Selectable(idLabel, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
            g_selectedId = row.proposalId;
        }

        // Issue column.
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.issueKey.empty() ? "(no issue key)" : row.issueKey.c_str());

        // State column.
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(RunStateDisplay(row.state));

        // PR URL column.
        ImGui::TableNextColumn();
        if (row.prUrl.empty()) {
            ImGui::TextDisabled("--");
        } else {
            ImGui::TextUnformatted(row.prUrl.c_str());
        }

        // Actions column — per-row Cancel / Open-PR / Open-worktree buttons.
        // The Cancel button is gated by IsCancellable so terminal states +
        // pre-spawn states don't render a disabled-looking control that the
        // FSM would reject anyway. Open PR is gated on a non-empty prUrl.
        ImGui::TableNextColumn();
        if (IsCancellable(row.state)) {
            if (ImGui::SmallButton(SmatchetLocalization::T("agent.handoffs.actions.cancel", "Cancel"))) {
                CancelHandoff(app, row.proposalId);
            }
            ImGui::SameLine();
        }
        if (!row.prUrl.empty()) {
            if (ImGui::SmallButton(SmatchetLocalization::T("agent.handoffs.actions.open", "Open PR"))) {
                OpenInShell(row.prUrl);
            }
            ImGui::SameLine();
        }
        if (!row.worktreeDir.empty()) {
            if (ImGui::SmallButton(SmatchetLocalization::T("agent.handoffs.actions.openWorktree", "Open WT"))) {
                OpenInShell(row.worktreeDir);
            }
        }

        ImGui::PopID();
    }
    ImGui::EndTable();
}

void RenderDetailPane(AppController& app) {
    ImGui::Separator();
    const AgenticHandoffController::ActiveHandoff* sel = FindSelectedInCache();
    if (sel == nullptr) {
        if (g_selectedId == 0) {
            ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.handoffs.detail.selectHint",
                                                              "Select a handoff row to see details."));
        } else {
            ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.handoffs.detail.selectedGone",
                                                              "Selected handoff is no longer active."));
        }
        return;
    }

    ImGui::Text("%s #%lld (%s)",
                SmatchetLocalization::T("agent.handoffs.detail.heading", "Handoff"),
                static_cast<long long>(sel->proposalId), RunStateDisplay(sel->state));

    // Per-row error / info status (cleared by next button click).
    {
        auto statusIt = g_perRowStatus.find(sel->proposalId);
        if (statusIt != g_perRowStatus.end() && !statusIt->second.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.55f, 1.0f), "%s", statusIt->second.c_str());
        }
    }

    // AwaitingUser → render the clarification reply box + submit. Other
    // states show the question text (when populated) but no input box.
    const bool isAwaiting = sel->state == CodingHarness::RunState::AwaitingUser;
    if (!sel->lastClarificationQuestion.empty()) {
        ImGui::TextDisabled("%s",
                            SmatchetLocalization::T("agent.handoffs.detail.question", "Agent question:"));
        ImGui::Indent(16.0f);
        ImGui::TextWrapped("%s", sel->lastClarificationQuestion.c_str());
        ImGui::Unindent(16.0f);
    }

    if (isAwaiting) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s",
                            SmatchetLocalization::T("agent.handoffs.detail.replyLabel", "Your reply:"));
        std::vector<char>& buf = EnsureReplyBuf(sel->proposalId);
        ImGui::InputTextMultiline("##handoffReply", buf.data(), buf.size(),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f),
                                  ImGuiInputTextFlags_AllowTabInput);
        if (ImGui::Button(SmatchetLocalization::T("agent.handoffs.detail.submit", "Submit clarification"))) {
            SubmitClarification(app, sel->proposalId, buf.data());
        }
    }

    if (!sel->lastError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.55f, 1.0f), "%s: %s",
                           SmatchetLocalization::T("agent.handoffs.detail.lastError", "Last error"),
                           sel->lastError.c_str());
    }

    // Compact metadata block — worktree, branch, started-at (unix-second
    // → display via ctime), iteration count. Useful for "is the watcher
    // making progress?" inspection without opening the log file.
    ImGui::Spacing();
    if (!sel->worktreeDir.empty()) {
        ImGui::TextDisabled("worktree: %s", sel->worktreeDir.c_str());
    }
    if (!sel->branchName.empty()) {
        ImGui::TextDisabled("branch: %s", sel->branchName.c_str());
    }
    if (sel->startedAtSec > 0) {
        const std::time_t t = static_cast<std::time_t>(sel->startedAtSec);
        char timeBuf[64];
        // std::strftime is C++14-safe; std::gmtime is single-threaded-only by
        // the standard, but the panel runs UI-thread-only so concurrent access
        // is not possible. Render UTC to dodge a localtime DST surprise.
        if (std::tm* tm = std::gmtime(&t)) {
            std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S UTC", tm);
            ImGui::TextDisabled("started: %s", timeBuf);
        }
    }
    if (sel->iterationCount > 0) {
        ImGui::TextDisabled("PR iterations: %d", sel->iterationCount);
    }
}

} // namespace

void Render(AppController& app, UiDrawSession& d) {
    if (!d.showAgentHandoffs) {
        return;
    }

    if (ShouldRefresh()) {
        RefreshFromController(app);
    }

    ImGui::SetNextWindowSize(ImVec2(720.0f, 520.0f), ImGuiCond_FirstUseEver);
    const char* title = SmatchetLocalization::T("agent.handoffs.windowTitle", "Agent handoffs");
    if (!ImGui::Begin(title, &d.showAgentHandoffs)) {
        ImGui::End();
        return;
    }

    // Header — count + manual refresh button.
    {
        const char* activeFmt = SmatchetLocalization::T("agent.handoffs.activeCount", "%d active");
        ImGui::Text(activeFmt, static_cast<int>(g_handoffsCache.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton(SmatchetLocalization::T("agent.handoffs.refresh", "Refresh"))) {
            RefreshFromController(app);
        }
    }

    if (g_handoffsCache.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "%s", SmatchetLocalization::T("agent.handoffs.empty",
                                          "No active handoffs. Approve an ImplementIssue proposal to start one."));
        ImGui::End();
        return;
    }

    RenderTable(app);
    RenderDetailPane(app);
    ImGui::End();
}

} // namespace SmatchetAgentHandoffUi
