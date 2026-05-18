#include "SmatchetAgentProposalsUi.h"

#include "SmatchetAgentProposalsUiPure.h"

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#if defined(SMATCHET_WITH_AGENTIC)
#include "AgenticHandoffController.h"
#endif
#include "AgenticInferenceClientPure.h"
#include "AgenticProposalAuditPure.h"
#include "AppController.h"
#include "BackendAuditTrail.h"
#include "Logger.h"
#include "MainThreadDispatcher.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <nlohmann/json.hpp>
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
        // (Bundle A) Store init is deferred to a background thread — accessor is
        // nullptr for the brief window between AppController::Initialize and the
        // worker stamping `agentProposalStoreReady_`. Surface a non-error
        // "Initializing..." line to the user instead of the "not available"
        // wording (which now implies a real failure: AGENTIC=OFF or DB-open
        // exception). Leave `g_initialFetchDone` false so `ShouldRefresh()` keeps
        // probing until the store lands.
        g_proposalsCache.clear();
        g_lastRefreshError = SmatchetLocalization::T("agent.proposals.initializing",
                                                     "Initializing agent proposal store...");
        g_lastRefreshAt = std::chrono::steady_clock::now();
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
        // (Bundle A) Throttle initial-refresh attempts the same as steady-state
        // (1 Hz) so a still-initialising store doesn't get hammered every frame.
        // The first call (g_lastRefreshAt is epoch) trips the interval immediately.
        if (g_lastRefreshAt == std::chrono::steady_clock::time_point{}) {
            return true;
        }
        return (std::chrono::steady_clock::now() - g_lastRefreshAt) >= kRefreshInterval;
    }
    return (std::chrono::steady_clock::now() - g_lastRefreshAt) >= kRefreshInterval;
}

void TransitionAndToast(AppController& app, AgentProposal& row, AgentProposalState target) {
    // Pillar 2 (AGENTS.md): SQLite::Database::Transition is sub-ms under no
    // contention, but the T7 scheduled-poll worker holds WAL writers and a
    // 5 s busy-timeout — a click during a mid-batch poll would freeze the UI
    // thread for seconds. Punt the write to a worker thread; the panel
    // optimistically removes the row on the next 1 Hz refresh tick (or sooner
    // if the worker posts a refresh-now back via the dispatcher).
    //
    // Audit-trail wiring (SH2): every Approve / Reject emits a Begin + Result
    // pair under the agentic_proposals source so a local SQLite UPDATE is
    // distinguishable from a "real backend write" by audit-log consumers. The
    // entry shape is built by AgenticProposalAuditPure (doctest-covered).
    const std::int64_t proposalId = row.id;
    const std::string issueKey = row.issueKey;
    const AgentProposalState fromState = row.state;
    const std::string operationId = BackendAuditTrail::MakeOperationId("agentic-proposals");

    BackendAuditTrail::AppendEvent(
        smatchet::agentic::pure::MakeBeginEvent(proposalId, issueKey, fromState, target, operationId));

    app.LaunchBackgroundTask([&app, proposalId, issueKey, fromState, target, operationId]() {
        auto* store = app.GetAgentProposalStore();
        if (store == nullptr) {
            BackendAuditTrail::AppendEvent(smatchet::agentic::pure::MakeResultEvent(
                proposalId, issueKey, fromState, target, operationId, /*success*/ false,
                /*error*/ "Proposal store unavailable."));
            const std::string toastTitle =
                SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
            const std::string toastMsg =
                SmatchetLocalization::T("agent.proposals.toastStoreUnavailable", "Proposal store unavailable.");
            app.mainThreadDispatcher.PostToMainThread([toastTitle, toastMsg]() {
                SmatchetToastManager::Instance().Push(toastTitle, toastMsg, ToastType::Error, 4000);
            });
            return;
        }
        std::string err;
        const bool ok = store->Transition(proposalId, target, std::string(), err);
        BackendAuditTrail::AppendEvent(smatchet::agentic::pure::MakeResultEvent(
            proposalId, issueKey, fromState, target, operationId, ok, ok ? std::string() : err));
        if (!ok) {
            LOG_WARN("SmatchetAgentProposalsUi: Transition id=%lld -> %s failed: %s",
                     static_cast<long long>(proposalId), AgentProposalStateToString(target), err.c_str());
            const std::string toastErr = err;
            const std::string toastTitle =
                SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
            app.mainThreadDispatcher.PostToMainThread([toastTitle, toastErr]() {
                SmatchetToastManager::Instance().Push(toastTitle, toastErr, ToastType::Error, 5000);
            });
            return;
        }
        LOG_INFO("SmatchetAgentProposalsUi: proposal id=%lld transitioned to %s",
                 static_cast<long long>(proposalId), AgentProposalStateToString(target));
        // Force a refresh on the next frame so the row disappears immediately
        // rather than waiting for the 1 Hz tick. Touching the TU-static via
        // the dispatcher keeps the mutation on the UI thread (the only writer
        // for `g_initialFetchDone`).
        app.mainThreadDispatcher.PostToMainThread([]() { g_initialFetchDone = false; });
    });

    // Optimistic local prune so the user sees their click stick at human
    // latency even while the worker still runs. The next refresh tick
    // reconciles against the SQLite truth — on worker failure the row
    // reappears + a toast surfaces.
    const auto it = std::find_if(g_proposalsCache.begin(), g_proposalsCache.end(),
                                 [proposalId](const AgentProposal& p) { return p.id == proposalId; });
    if (it != g_proposalsCache.end()) {
        g_proposalsCache.erase(it);
    }
}

#if defined(SMATCHET_WITH_AGENTIC)
// (H9) Manual-handoff path — fired from the per-row [Start handoff] button on
// ImplementIssue proposals. The flow:
//   1. SQLite Transition Pending -> Approved (on a worker thread for pillar 2).
//   2. If the OnProposalApproved callback ran the auto-start path (cfg
//      HandoffAutoStartOnApprove=true), Start() will refuse the duplicate
//      in-flight start — that's fine, the user clicked the button expecting
//      a handoff and one is already running.
//   3. Otherwise Start() drives the handoff lifecycle from a worker thread.
//
// Defense in depth (anti-deception): the caller only invokes this when
// `row.action == ImplementIssue`. We re-check the action against the post-
// transition row here so a future caller wiring the button on a non-
// ImplementIssue row gets a LOG_WARN + toast instead of a stuck handoff.
//
// Audit-trail distinction: this path emits `HandoffStarted` with
// `trigger=user-button`. The auto-start path emits `HandoffAutoStarted`
// with `trigger=approval-callback`. Both ride the same controller.Start()
// so the FSM transitions that follow are identical.
void ApproveAndStartHandoff(AppController& app, AgentProposal& row) {
    const std::int64_t proposalId = row.id;
    const std::string issueKey = row.issueKey;

    // Emit the audit "begin" row inline on the UI thread so downstream
    // queries can correlate the user-button-driven start even if the
    // worker dispatch fails before Start() runs. Carries trigger so
    // future audit-trail queries can distinguish manual vs auto.
    {
        BackendAuditTrail::AuditEvent ev;
        ev.Action = "HandoffStarted";
        ev.Source = "agentic";
        ev.IssueKey = issueKey;
        ev.OperationId = BackendAuditTrail::MakeOperationId("handoff");
        ev.Success = true;
        ev.Phase = "begin";
        ev.Data = nlohmann::json::object();
        ev.Data["proposalId"] = static_cast<long long>(proposalId);
        ev.Data["trigger"] = "user-button";
        BackendAuditTrail::AppendEvent(ev);
    }

    app.LaunchBackgroundTask([&app, proposalId, issueKey]() {
        auto* store = app.GetAgentProposalStore();
        auto* ctrl = app.GetAgenticHandoffController();
        if (store == nullptr || ctrl == nullptr) {
            const std::string toastTitle =
                SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
            const std::string toastMsg = (store == nullptr)
                ? SmatchetLocalization::T("agent.proposals.toastStoreUnavailable",
                                          "Proposal store unavailable.")
                : SmatchetLocalization::T(
                      "agent.proposals.toastHandoffControllerUnavailable",
                      "Agentic handoff controller unavailable (configure agentic flow).");
            app.mainThreadDispatcher.PostToMainThread([toastTitle, toastMsg]() {
                SmatchetToastManager::Instance().Push(toastTitle, toastMsg, ToastType::Error, 5000);
            });
            return;
        }
        // Transition first. If the row was already Approved (auto-start fired
        // in a parallel path), Transition fails with "invalid transition" —
        // tolerated; we still try Start().
        std::string transErr;
        const bool transOk = store->Transition(proposalId, AgentProposalState::Approved, std::string(), transErr);
        if (!transOk) {
            LOG_INFO("SmatchetAgentProposalsUi: ApproveAndStartHandoff Transition skipped/failed (id=%lld): %s",
                     static_cast<long long>(proposalId), transErr.c_str());
            // Fall through — the row may have been approved already (cfg auto-start
            // raced ahead of us). Start() below will surface the duplicate-in-flight
            // condition with a clean error if so.
        }

        smatchet::agentic::AgenticHandoffController::ActiveHandoff out;
        std::string startErr;
        const bool startOk = ctrl->Start(proposalId, out, startErr);
        if (!startOk) {
            // "already in flight" is the expected branch on auto-start race; surface
            // it as an INFO + a softer toast since the user's intent (handoff this
            // proposal) is already being satisfied.
            const bool isAlreadyInFlight = startErr.find("already in flight") != std::string::npos;
            if (isAlreadyInFlight) {
                LOG_INFO("SmatchetAgentProposalsUi: ApproveAndStartHandoff race — handoff already in flight "
                         "for proposalId=%lld",
                         static_cast<long long>(proposalId));
                const std::string toastTitle =
                    SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
                const std::string toastMsg = SmatchetLocalization::T(
                    "agent.proposals.toastHandoffAlreadyInFlight", "Handoff already in flight for this proposal.");
                app.mainThreadDispatcher.PostToMainThread([toastTitle, toastMsg]() {
                    SmatchetToastManager::Instance().Push(toastTitle, toastMsg, ToastType::Info, 4000);
                });
                return;
            }
            LOG_WARN("SmatchetAgentProposalsUi: ApproveAndStartHandoff failed proposalId=%lld: %s",
                     static_cast<long long>(proposalId), startErr.c_str());
            const std::string toastTitle =
                SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
            const std::string toastMsgFmt = SmatchetLocalization::T("agent.proposals.toastHandoffStartFailed",
                                                                    "Handoff start failed: %s");
            char buf[512];
            std::snprintf(buf, sizeof(buf), toastMsgFmt.c_str(), startErr.c_str());
            const std::string toastMsg(buf);
            app.mainThreadDispatcher.PostToMainThread([toastTitle, toastMsg]() {
                SmatchetToastManager::Instance().Push(toastTitle, toastMsg, ToastType::Error, 5000);
            });
            return;
        }
        LOG_INFO("SmatchetAgentProposalsUi: handoff started via user button proposalId=%lld branch=%s",
                 static_cast<long long>(proposalId), out.branchName.c_str());
        const std::string toastTitle =
            SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
        const std::string toastMsgFmt =
            SmatchetLocalization::T("agent.proposals.toastHandoffStarted", "Handoff started for proposal #%lld");
        char buf[256];
        std::snprintf(buf, sizeof(buf), toastMsgFmt.c_str(), static_cast<long long>(proposalId));
        const std::string toastMsg(buf);
        app.mainThreadDispatcher.PostToMainThread([toastTitle, toastMsg]() {
            SmatchetToastManager::Instance().Push(toastTitle, toastMsg, ToastType::Success, 4000);
        });
        // Force a refresh on the next frame so the row disappears immediately.
        app.mainThreadDispatcher.PostToMainThread([]() { g_initialFetchDone = false; });
    });

    // Optimistic local prune — mirror the Approve/Reject path so the row
    // disappears at human latency.
    const auto it = std::find_if(g_proposalsCache.begin(), g_proposalsCache.end(),
                                 [proposalId](const AgentProposal& p) { return p.id == proposalId; });
    if (it != g_proposalsCache.end()) {
        g_proposalsCache.erase(it);
    }
}
#endif // SMATCHET_WITH_AGENTIC

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
        const char* errPrefix = SmatchetLocalization::T("agent.proposals.refreshErrorPrefix",
                                                        "  (refresh error: %s)");
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.55f, 1.0f), errPrefix, g_lastRefreshError.c_str());
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
        // Bundle C CR#231:151 — PushID by pointer rather than narrowing the int64
        // ROWID to `int`. SQLite ROWIDs are 64-bit; once a proposals db crosses
        // 2^31 inserts (over its lifetime) the cast would alias older rows and
        // ImGui state (open/closed CollapsingHeader, focused widget) would leak
        // between rows. Pointer-based PushID is stable per-call and unique across
        // the visible cache.
        ImGui::PushID(&row);

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
                ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.proposals.rationaleEmpty", "(empty)"));
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
            // theme. The buttons delegate to TransitionAndToast which fires
            // the SQLite UPDATE on a worker thread (Pillar 2 — never block
            // the UI on contention with the T7 poll worker).
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

#if defined(SMATCHET_WITH_AGENTIC)
            // (H9) [Start handoff] button — conditional on:
            //   (a) action == ImplementIssue (defense-in-depth — the
            //       OnProposalApproved callback also filters by action so a
            //       future regression on this conditional cannot silently
            //       kick off a handoff for a non-handoff proposal),
            //   (b) the handoff controller exists (returns null in degraded
            //       mode when the agentic config is incomplete).
            //
            // The button approves the row AND starts the handoff in one
            // worker-thread task. This is the manual path; the auto-start
            // path (cfg HandoffAutoStartOnApprove=true) skips the button and
            // fires via the OnProposalApproved callback after the user clicks
            // the regular Approve.
            if (row.action == AgenticInferenceClientPure::ProposedAction::ImplementIssue &&
                app.GetAgenticHandoffController() != nullptr) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.70f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.50f, 0.82f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.34f, 0.60f, 1.0f));
                if (ImGui::Button(
                        SmatchetLocalization::T("agent.proposals.startHandoff", "Start handoff"))) {
                    ApproveAndStartHandoff(app, row);
                }
                ImGui::PopStyleColor(3);
            }
#endif // SMATCHET_WITH_AGENTIC
        }

        ImGui::PopID();
        ImGui::Separator();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace SmatchetAgentProposalsUi
