#include "SmatchetAgentProposalsUi.h"

#include "SmatchetAgentProposalsUiPure.h"

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticInferenceClientPure.h"
#include "AppController.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace SmatchetAgentProposalsUi {

namespace {

// Refresh cadence — 1 Hz polling is fine here; humans react to triage at
// human timescale, not 144 Hz. SQLite::Database::exec on the proposal-store
// path runs in << 1 ms but per-frame Query() at 144 Hz is wasted work.
const auto kRefreshInterval = std::chrono::milliseconds(1000);

// Truncation threshold for the rationale preview line. The full text lives in
// the CollapsingHeader's "Rationale" body; the header just shows the first
// kRationalePreviewBytes for at-a-glance scan.
using SmatchetAgentProposalsUiPure::kRationalePreviewBytes;
using SmatchetAgentProposalsUiPure::TruncatePreview;

// TU-static cache + last-refresh timestamp. Lives at TU scope (not UiDrawSession)
// because the panel owns its own polling cadence — a stray Render path with no
// open window must not write back into d.* state. The cache is only populated
// while the window is open; a closed window leaves last contents in place so
// reopening is instant.
std::vector<AgentProposal> g_proposalsCache;
std::chrono::steady_clock::time_point g_lastRefreshAt{};
std::string g_lastRefreshError;
bool g_initialFetchDone = false;

void RefreshFromStore(AppController& app) {
    auto* store = app.GetAgentProposalStore();
    if (store == nullptr) {
        g_proposalsCache.clear();
        g_lastRefreshError = "Agent proposal store is not available.";
        g_lastRefreshAt = std::chrono::steady_clock::now();
        g_initialFetchDone = true;
        return;
    }
    AgentProposalStore::Filter filter;
    filter.states.push_back(AgentProposalState::Pending);
    std::string err;
    std::vector<AgentProposal> rows;
    if (!store->Query(filter, rows, err)) {
        LOG_WARN("SmatchetAgentProposalsUi: Query failed: %s", err.c_str());
        g_lastRefreshError = err;
    } else {
        g_lastRefreshError.clear();
        // Newest-first by createdAtSec — Query returns in insertion order; sort
        // here so the UI never depends on undocumented store-side ordering.
        std::sort(rows.begin(), rows.end(),
                  [](const AgentProposal& a, const AgentProposal& b) { return a.createdAtSec > b.createdAtSec; });
        g_proposalsCache = std::move(rows);
    }
    g_lastRefreshAt = std::chrono::steady_clock::now();
    g_initialFetchDone = true;
}

bool ShouldRefresh() {
    if (!g_initialFetchDone) {
        return true;
    }
    return (std::chrono::steady_clock::now() - g_lastRefreshAt) >= kRefreshInterval;
}

void TransitionAndToast(AppController& app, AgentProposal& row, AgentProposalState target) {
    auto* store = app.GetAgentProposalStore();
    if (store == nullptr) {
        SmatchetToastManager::Instance().Push("Agent proposals", "Proposal store unavailable.", ToastType::Error, 4000);
        return;
    }
    std::string err;
    if (!store->Transition(row.id, target, std::string(), err)) {
        LOG_WARN("SmatchetAgentProposalsUi: Transition id=%lld -> %s failed: %s", static_cast<long long>(row.id),
                 AgentProposalStateToString(target), err.c_str());
        SmatchetToastManager::Instance().Push("Agent proposals", err, ToastType::Error, 5000);
        return;
    }
    LOG_INFO("SmatchetAgentProposalsUi: proposal id=%lld transitioned to %s", static_cast<long long>(row.id),
             AgentProposalStateToString(target));
    // Force a refresh on the next frame so the row disappears immediately
    // rather than waiting for the 1 Hz tick.
    g_initialFetchDone = false;
}

} // namespace

void Render(AppController& app, UiDrawSession& d) {
    if (!d.showAgentProposals) {
        return;
    }

    if (ShouldRefresh()) {
        RefreshFromStore(app);
    }

    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);
    const char* title = SmatchetLocalization::T("agent.proposals.windowTitle", "Agent proposals");
    if (!ImGui::Begin(title, &d.showAgentProposals)) {
        ImGui::End();
        return;
    }

    // Header line — count + last-refresh error (if any).
    ImGui::Text("%s: %d",
                SmatchetLocalization::T("agent.proposals.totalPending", "Total Pending"),
                static_cast<int>(g_proposalsCache.size()));
    if (!g_lastRefreshError.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.55f, 1.0f), "  (refresh error: %s)", g_lastRefreshError.c_str());
    }

    if (ImGui::SmallButton(SmatchetLocalization::T("agent.proposals.refresh", "Refresh"))) {
        RefreshFromStore(app);
    }

    ImGui::Separator();

    if (g_proposalsCache.empty()) {
        ImGui::TextWrapped("%s",
                           SmatchetLocalization::T(
                               "agent.proposals.empty",
                               "No pending proposals. Run `agent.triage.run` to generate proposals."));
        ImGui::End();
        return;
    }

    // Scrollable child so the per-row content can be tall without ballooning
    // the outer window. Reserve room for nothing else under it — the panel is
    // dead simple: header strip + list.
    ImGui::BeginChild("##AgentProposalsList", ImVec2(0.0f, 0.0f), false);
    for (size_t i = 0; i < g_proposalsCache.size(); ++i) {
        AgentProposal& row = g_proposalsCache[i];
        ImGui::PushID(static_cast<int>(row.id));

        // Header — issueKey + bracketed action name. Issue title is deferred
        // (T7 may add it via FetchIssueBody-on-insert) so we show only the
        // canonical keys + the stable action enum string.
        const char* actionStr = AgenticInferenceClientPure::ActionToString(row.action);
        const std::string headerLabel = row.issueKey + std::string("  [") + actionStr + std::string("]");
        const bool headerOpen = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

        if (headerOpen) {
            // Rationale — preview + collapsible full text.
            ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.proposals.rationale", "Rationale:"));
            ImGui::Indent(16.0f);
            if (row.rationale.size() > kRationalePreviewBytes) {
                if (ImGui::TreeNode("##rationale", "%s %s", TruncatePreview(row.rationale).c_str(),
                                    SmatchetLocalization::T("agent.proposals.expand", "[click to expand]"))) {
                    ImGui::TextWrapped("%s", row.rationale.c_str());
                    ImGui::TreePop();
                }
            } else if (row.rationale.empty()) {
                ImGui::TextDisabled("(empty)");
            } else {
                ImGui::TextWrapped("%s", row.rationale.c_str());
            }
            ImGui::Unindent(16.0f);

            // Payload — JSON pretty-print under a CollapsingHeader (collapsed
            // by default; payloads can be multi-line).
            if (ImGui::TreeNode("##payload", "%s",
                                SmatchetLocalization::T("agent.proposals.payload", "Payload:"))) {
                std::string payloadDump;
                try {
                    payloadDump = row.payload.is_null() ? std::string("{}") : row.payload.dump(2);
                } catch (const std::exception& ex) {
                    payloadDump = std::string("<dump failed: ") + ex.what() + std::string(">");
                }
                ImGui::TextWrapped("%s", payloadDump.c_str());
                ImGui::TreePop();
            }

            // Buttons. Green Approve, red Reject. Push/Pop the colour state
            // around each so the rest of the panel stays on the standard
            // theme. The buttons drive AgentProposalStore::Transition on the
            // UI thread — sub-ms SQLite UPDATE makes the inline call safe.
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.36f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.48f, 0.26f, 1.0f));
            if (ImGui::Button(SmatchetLocalization::T("agent.proposals.approve", "Approve"))) {
                TransitionAndToast(app, row, AgentProposalState::Approved);
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.22f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.18f, 0.18f, 1.0f));
            if (ImGui::Button(SmatchetLocalization::T("agent.proposals.reject", "Reject"))) {
                TransitionAndToast(app, row, AgentProposalState::Rejected);
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::PopID();
        ImGui::Separator();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace SmatchetAgentProposalsUi
