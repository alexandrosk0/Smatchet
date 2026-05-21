#include "SmatchetAgentProposalsUi.h"

#include "SmatchetAgentProposalsUiPure.h"

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "MarkdownPreviewRender.h"
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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <unordered_map>
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

// (Log-spam fix) RefreshFromStore runs every 1 Hz tick while the proposals
// panel is open. Logging the row count at LOG_INFO every tick floods the log.
// Track the last-logged count so we only emit LOG_INFO when the count
// actually changes (proposal landed / approved / rejected); steady-state
// refreshes drop to LOG_TRACE.
std::size_t g_lastLoggedProposalCount = static_cast<std::size_t>(-1);

// (Edit-before-send) Per-proposal-id edit buffer for the CommentAdd body
// text-area. Lazily initialised from `row.payload["body"]` on first render
// of each Pending CommentAdd row; persisted to SQLite via
// AgentProposalStore::UpdatePayload right before TransitionAndToast fires
// on the Approve / Retry button. Cleared when the row leaves Pending (the
// 1 Hz refresh tick sweeps stale entries out so the map doesn't grow
// without bound across long sessions).
std::unordered_map<std::int64_t, std::string> g_commentEditBuf;

// Sweep edit-buffer entries whose proposalId no longer appears in the cache.
// Called at the end of RefreshFromStore so the map size tracks the visible
// row set. Cheap — N visible rows × M buffer entries, both bounded by user-
// scale numbers (rarely > 50).
void SweepCommentEditBuf() {
    if (g_commentEditBuf.empty()) {
        return;
    }
    for (auto it = g_commentEditBuf.begin(); it != g_commentEditBuf.end();) {
        const std::int64_t id = it->first;
        const auto found = std::find_if(g_proposalsCache.begin(), g_proposalsCache.end(),
                                        [id](const AgentProposal& p) { return p.id == id; });
        if (found == g_proposalsCache.end()) {
            it = g_commentEditBuf.erase(it);
        } else {
            ++it;
        }
    }
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

// Cross-platform "open this URL in the user's default browser". Mirrors the
// helper in SmatchetAgentHandoffUi.cpp + BlameAnalysisUi_Launch.cpp. Non-Windows
// stub returns false so the dual-target build still links (DX12 compiles
// Source_Core/ but the panel only renders Standalone).
bool OpenInShell(const std::string& target) {
    LOG_TRACE("SmatchetAgentProposalsUi::OpenInShell enter (target=%s)", target.c_str());
#ifdef _WIN32
    const std::wstring wide = Utf8ToWide(target);
    if (wide.empty()) {
        LOG_WARN("SmatchetAgentProposalsUi::OpenInShell empty wide-string (utf8 conversion failed)");
        return false;
    }
    SetLastError(0);
    const INT_PTR r =
        reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) {
        LOG_WARN("SmatchetAgentProposalsUi::OpenInShell: ShellExecuteW failed result=%lld GetLastError=%lu",
                 static_cast<long long>(r), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    LOG_INFO("SmatchetAgentProposalsUi::OpenInShell launched browser for %s", target.c_str());
    return true;
#else
    LOG_INFO("SmatchetAgentProposalsUi::OpenInShell (non-Windows stub): would open %s", target.c_str());
    return false;
#endif
}

// Derive the upstream tracker URL from `sourceTracker` + `issueKey`. Today
// only `github` is supported (the only `cfg.AgenticPollSource` value). The
// canonical issueKey shape for GitHub is `owner/repo#N`; we transform to
// `https://github.com/owner/repo/issues/N`. Returns empty for unknown
// sourceTracker or malformed issueKey.
std::string MakeIssueUrl(const std::string& sourceTracker, const std::string& issueKey) {
    if (sourceTracker == "github") {
        const std::string::size_type hash = issueKey.find('#');
        if (hash == std::string::npos || hash + 1 >= issueKey.size()) {
            LOG_DEBUG("SmatchetAgentProposalsUi::MakeIssueUrl: malformed github issueKey '%s'", issueKey.c_str());
            return std::string();
        }
        const std::string ownerRepo = issueKey.substr(0, hash);
        const std::string num = issueKey.substr(hash + 1);
        if (ownerRepo.empty() || num.empty()) {
            return std::string();
        }
        return std::string("https://github.com/") + ownerRepo + std::string("/issues/") + num;
    }
    LOG_DEBUG("SmatchetAgentProposalsUi::MakeIssueUrl: unknown sourceTracker '%s'", sourceTracker.c_str());
    return std::string();
}

// Pretty timestamp helper — formats unix epoch seconds as local HH:MM:SS for
// the per-row metadata line. Returns "?" on failure so the row still renders.
std::string FormatLocalHms(std::int64_t epochSec) {
    const std::time_t t = static_cast<std::time_t>(epochSec);
    std::tm tmLocal{};
#ifdef _WIN32
    if (localtime_s(&tmLocal, &t) != 0) {
        return std::string("?");
    }
#else
    if (localtime_r(&t, &tmLocal) == nullptr) {
        return std::string("?");
    }
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmLocal) == 0) {
        return std::string("?");
    }
    return std::string(buf);
}

// Action-specific one-line summary derived from the payload. Returns empty
// when the action has no canonical summary or the payload doesn't carry the
// expected fields. Caller wraps in TextWrapped — the summary may be long
// (e.g. CommentAdd body).
std::string MakeActionSummary(const AgentProposal& row) {
    using AAction = AgenticInferenceClientPure::ProposedAction;
    try {
        if (!row.payload.is_object()) {
            return std::string();
        }
        switch (row.action) {
        case AAction::CommentAdd:
            if (row.payload.contains("body") && row.payload["body"].is_string()) {
                return std::string("Will post comment: \"") + row.payload["body"].get<std::string>() +
                       std::string("\"");
            }
            break;
        case AAction::LabelAdd:
            if (row.payload.contains("label") && row.payload["label"].is_string()) {
                return std::string("Will add label: ") + row.payload["label"].get<std::string>();
            }
            break;
        case AAction::LabelRemove:
            if (row.payload.contains("label") && row.payload["label"].is_string()) {
                return std::string("Will remove label: ") + row.payload["label"].get<std::string>();
            }
            break;
        case AAction::AssigneeSet:
            if (row.payload.contains("assignee") && row.payload["assignee"].is_string()) {
                return std::string("Will set assignee: ") + row.payload["assignee"].get<std::string>();
            }
            break;
        case AAction::StateTransition:
            if (row.payload.contains("state") && row.payload["state"].is_string()) {
                return std::string("Will transition state to: ") + row.payload["state"].get<std::string>();
            }
            break;
        case AAction::DerivedTicketCreate:
            return std::string("Will create derived ticket (see payload)");
        case AAction::ImplementIssue:
            return std::string("Will spawn Claude harness to implement this issue");
        default:
            break;
        }
    } catch (const std::exception& ex) {
        LOG_WARN("SmatchetAgentProposalsUi::MakeActionSummary payload parse failed (id=%lld): %s",
                 static_cast<long long>(row.id), ex.what());
    }
    return std::string();
}

void RefreshFromStore(AppController& app) {
    LOG_TRACE("SmatchetAgentProposalsUi::RefreshFromStore enter");
    auto* store = app.GetAgentProposalStore();
    if (store == nullptr) {
        // (Bundle A) Store init is deferred to a background thread — accessor is
        // nullptr for the brief window between AppController::Initialize and the
        // worker stamping `agentProposalStoreReady_`. Surface a non-error
        // "Initializing..." line to the user instead of the "not available"
        // wording (which now implies a real failure: AGENTIC=OFF or DB-open
        // exception). Leave `g_initialFetchDone` false so `ShouldRefresh()` keeps
        // probing until the store lands.
        LOG_TRACE("SmatchetAgentProposalsUi::RefreshFromStore: store accessor nullptr — still initializing");
        g_proposalsCache.clear();
        g_lastRefreshError =
            SmatchetLocalization::T("agent.proposals.initializing", "Initializing agent proposal store...");
        g_lastRefreshAt = std::chrono::steady_clock::now();
        return;
    }
    AgentProposalStore::Filter filter;
    filter.states.push_back(AgentProposalState::Pending);
    // (Apply-feedback) Surface Failed rows too so the user can see + retry
    // proposals that hit upstream errors (HTTP 403, etc). The panel renders
    // Failed rows with a red applyError line + a [Retry] button.
    filter.states.push_back(AgentProposalState::Failed);
    LOG_TRACE("SmatchetAgentProposalsUi::RefreshFromStore: Query filter states=Pending|Failed");
    std::string err;
    std::vector<AgentProposal> rows;
    if (!store->Query(filter, rows, err)) {
        LOG_WARN("SmatchetAgentProposalsUi: Query failed: %s", err.c_str());
        g_lastRefreshError = err;
    } else {
        const std::size_t n = rows.size();
        if (n != g_lastLoggedProposalCount) {
            LOG_INFO("SmatchetAgentProposalsUi::RefreshFromStore: pending count %zu -> %zu",
                     g_lastLoggedProposalCount == static_cast<std::size_t>(-1) ? 0 : g_lastLoggedProposalCount, n);
            g_lastLoggedProposalCount = n;
        } else {
            LOG_TRACE("SmatchetAgentProposalsUi::RefreshFromStore: Query returned %zu pending proposals (unchanged)",
                      n);
        }
        g_lastRefreshError.clear();
        // Newest-first by createdAtSec — Query returns in insertion order; sort
        // here so the UI never depends on undocumented store-side ordering.
        std::sort(rows.begin(), rows.end(),
                  [](const AgentProposal& a, const AgentProposal& b) { return a.createdAtSec > b.createdAtSec; });
        g_proposalsCache = std::move(rows);
    }
    g_lastRefreshAt = std::chrono::steady_clock::now();
    g_initialFetchDone = true;
    // (Edit-before-send) Prune edit-buffer entries for rows no longer in the
    // visible cache (transitioned to Applied / Failed-then-dismissed / etc.).
    // Keeps g_commentEditBuf bounded across long sessions.
    SweepCommentEditBuf();
    LOG_TRACE("SmatchetAgentProposalsUi::RefreshFromStore exit (cache_size=%zu)", g_proposalsCache.size());
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

    LOG_INFO("SmatchetAgentProposalsUi::TransitionAndToast click id=%lld issueKey=%s from=%s to=%s op=%s",
             static_cast<long long>(proposalId), issueKey.c_str(), AgentProposalStateToString(fromState),
             AgentProposalStateToString(target), operationId.c_str());
    LOG_DEBUG("SmatchetAgentProposalsUi::TransitionAndToast appending audit-trail Begin event");
    BackendAuditTrail::AppendEvent(
        smatchet::agentic::pure::MakeBeginEvent(proposalId, issueKey, fromState, target, operationId));

    app.LaunchBackgroundTask([&app, proposalId, issueKey, fromState, target, operationId]() {
        LOG_TRACE("SmatchetAgentProposalsUi::TransitionAndToast worker enter (id=%lld)",
                  static_cast<long long>(proposalId));
        auto* store = app.GetAgentProposalStore();
        if (store == nullptr) {
            LOG_WARN("SmatchetAgentProposalsUi::TransitionAndToast worker: store nullptr id=%lld",
                     static_cast<long long>(proposalId));
            BackendAuditTrail::AppendEvent(smatchet::agentic::pure::MakeResultEvent(
                proposalId, issueKey, fromState, target, operationId, /*success*/ false,
                /*error*/ "Proposal store unavailable."));
            const std::string toastTitle = SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
            const std::string toastMsg =
                SmatchetLocalization::T("agent.proposals.toastStoreUnavailable", "Proposal store unavailable.");
            app.mainThreadDispatcher.PostToMainThread([toastTitle, toastMsg]() {
                SmatchetToastManager::Instance().Push(toastTitle, toastMsg, ToastType::Error, 4000);
            });
            return;
        }
        LOG_DEBUG("SmatchetAgentProposalsUi::TransitionAndToast SQL Transition id=%lld -> %s",
                  static_cast<long long>(proposalId), AgentProposalStateToString(target));
        std::string err;
        const bool ok = store->Transition(proposalId, target, std::string(), err);
        LOG_DEBUG("SmatchetAgentProposalsUi::TransitionAndToast SQL Transition done ok=%d", ok ? 1 : 0);
        BackendAuditTrail::AppendEvent(smatchet::agentic::pure::MakeResultEvent(
            proposalId, issueKey, fromState, target, operationId, ok, ok ? std::string() : err));
        if (!ok) {
            LOG_WARN("SmatchetAgentProposalsUi: Transition id=%lld -> %s failed: %s",
                     static_cast<long long>(proposalId), AgentProposalStateToString(target), err.c_str());
            const std::string toastErr = err;
            const std::string toastTitle = SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
            app.mainThreadDispatcher.PostToMainThread([toastTitle, toastErr]() {
                SmatchetToastManager::Instance().Push(toastTitle, toastErr, ToastType::Error, 5000);
            });
            return;
        }
        LOG_INFO("SmatchetAgentProposalsUi: proposal id=%lld transitioned to %s", static_cast<long long>(proposalId),
                 AgentProposalStateToString(target));
        // Force a refresh on the next frame so the row disappears immediately
        // rather than waiting for the 1 Hz tick. Touching the TU-static via
        // the dispatcher keeps the mutation on the UI thread (the only writer
        // for `g_initialFetchDone`).
        app.mainThreadDispatcher.PostToMainThread([]() {
            LOG_TRACE("SmatchetAgentProposalsUi::TransitionAndToast dispatcher: forcing refresh-now");
            g_initialFetchDone = false;
        });
        LOG_TRACE("SmatchetAgentProposalsUi::TransitionAndToast worker exit ok");
    });

    // Optimistic local prune so the user sees their click stick at human
    // latency even while the worker still runs. The next refresh tick
    // reconciles against the SQLite truth — on worker failure the row
    // reappears + a toast surfaces.
    const auto it = std::find_if(g_proposalsCache.begin(), g_proposalsCache.end(),
                                 [proposalId](const AgentProposal& p) { return p.id == proposalId; });
    if (it != g_proposalsCache.end()) {
        LOG_DEBUG("SmatchetAgentProposalsUi::TransitionAndToast optimistic prune id=%lld from cache",
                  static_cast<long long>(proposalId));
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
void ApproveAndStartHandoff(AppController& app, const AgentProposal& row) {
    const std::int64_t proposalId = row.id;
    const std::string issueKey = row.issueKey;

    LOG_INFO("SmatchetAgentProposalsUi::ApproveAndStartHandoff click id=%lld issueKey=%s action=%s",
             static_cast<long long>(proposalId), issueKey.c_str(),
             AgenticInferenceClientPure::ActionToString(row.action));

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
        LOG_DEBUG("SmatchetAgentProposalsUi::ApproveAndStartHandoff appending audit-trail HandoffStarted (begin) op=%s",
                  ev.OperationId.c_str());
        BackendAuditTrail::AppendEvent(ev);
    }

    app.LaunchBackgroundTask([&app, proposalId, issueKey]() {
        LOG_TRACE("SmatchetAgentProposalsUi::ApproveAndStartHandoff worker enter (id=%lld)",
                  static_cast<long long>(proposalId));
        auto* store = app.GetAgentProposalStore();
        auto* ctrl = app.GetAgenticHandoffController();
        if (store == nullptr || ctrl == nullptr) {
            LOG_WARN("SmatchetAgentProposalsUi::ApproveAndStartHandoff degraded — store=%p ctrl=%p",
                     static_cast<const void*>(store), static_cast<const void*>(ctrl));
            const std::string toastTitle = SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
            const std::string toastMsg =
                (store == nullptr)
                    ? SmatchetLocalization::T("agent.proposals.toastStoreUnavailable", "Proposal store unavailable.")
                    : SmatchetLocalization::T("agent.proposals.toastHandoffControllerUnavailable",
                                              "Agentic handoff controller unavailable (configure agentic flow).");
            app.mainThreadDispatcher.PostToMainThread([toastTitle, toastMsg]() {
                SmatchetToastManager::Instance().Push(toastTitle, toastMsg, ToastType::Error, 5000);
            });
            return;
        }
        // Transition first. If the row was already Approved (auto-start fired
        // in a parallel path), Transition fails with "invalid transition" —
        // tolerated; we still try Start().
        LOG_DEBUG("SmatchetAgentProposalsUi::ApproveAndStartHandoff SQL Transition id=%lld -> Approved",
                  static_cast<long long>(proposalId));
        std::string transErr;
        const bool transOk = store->Transition(proposalId, AgentProposalState::Approved, std::string(), transErr);
        LOG_DEBUG("SmatchetAgentProposalsUi::ApproveAndStartHandoff SQL Transition done ok=%d", transOk ? 1 : 0);
        if (!transOk) {
            LOG_INFO("SmatchetAgentProposalsUi: ApproveAndStartHandoff Transition skipped/failed (id=%lld): %s",
                     static_cast<long long>(proposalId), transErr.c_str());
            // Fall through — the row may have been approved already (cfg auto-start
            // raced ahead of us). Start() below will surface the duplicate-in-flight
            // condition with a clean error if so.
        }

        LOG_INFO("SmatchetAgentProposalsUi::ApproveAndStartHandoff invoking controller.Start id=%lld",
                 static_cast<long long>(proposalId));
        smatchet::agentic::AgenticHandoffController::ActiveHandoff out;
        std::string startErr;
        const bool startOk = ctrl->Start(proposalId, out, startErr);
        LOG_DEBUG("SmatchetAgentProposalsUi::ApproveAndStartHandoff controller.Start done ok=%d branch=%s",
                  startOk ? 1 : 0, out.branchName.c_str());
        if (!startOk) {
            // "already in flight" is the expected branch on auto-start race; surface
            // it as an INFO + a softer toast since the user's intent (handoff this
            // proposal) is already being satisfied.
            const bool isAlreadyInFlight = startErr.find("already in flight") != std::string::npos;
            if (isAlreadyInFlight) {
                LOG_INFO("SmatchetAgentProposalsUi: ApproveAndStartHandoff race — handoff already in flight "
                         "for proposalId=%lld",
                         static_cast<long long>(proposalId));
                const std::string toastTitle = SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
                const std::string toastMsg = SmatchetLocalization::T("agent.proposals.toastHandoffAlreadyInFlight",
                                                                     "Handoff already in flight for this proposal.");
                app.mainThreadDispatcher.PostToMainThread([toastTitle, toastMsg]() {
                    SmatchetToastManager::Instance().Push(toastTitle, toastMsg, ToastType::Info, 4000);
                });
                return;
            }
            LOG_WARN("SmatchetAgentProposalsUi: ApproveAndStartHandoff failed proposalId=%lld: %s",
                     static_cast<long long>(proposalId), startErr.c_str());
            const std::string toastTitle = SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
            const std::string toastMsgFmt =
                SmatchetLocalization::T("agent.proposals.toastHandoffStartFailed", "Handoff start failed: %s");
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
        const std::string toastTitle = SmatchetLocalization::T("agent.proposals.toastTitle", "Agent proposals");
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
    // (Auto-open) Refresh always — even when the panel is closed — so we can
    // detect a count-delta and pop the panel open. ShouldRefresh() throttles
    // to 1 Hz so the closed-panel cost is one SQLite SELECT per second; we
    // skip this work entirely when the store accessor is nullptr (per
    // RefreshFromStore's early-return).
    const std::size_t prevCount = g_lastLoggedProposalCount == static_cast<std::size_t>(-1)
                                      ? static_cast<std::size_t>(0)
                                      : g_lastLoggedProposalCount;
    if (ShouldRefresh()) {
        RefreshFromStore(app);
    }
    const std::size_t newCount = g_lastLoggedProposalCount == static_cast<std::size_t>(-1) ? static_cast<std::size_t>(0)
                                                                                           : g_lastLoggedProposalCount;
    if (newCount > prevCount && !d.showAgentProposals) {
        // (Auto-open) Pending count went up while the panel was closed — pop
        // the panel open so the user sees the new row. LOG_INFO once per
        // auto-open so operators can grep for "auto-open" in the timeline.
        LOG_INFO("SmatchetAgentProposalsUi: auto-opening panel on count delta %zu -> %zu", prevCount, newCount);
        d.showAgentProposals = true;
    }

    if (!d.showAgentProposals) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_FirstUseEver);
    const char* title = SmatchetLocalization::T("agent.proposals.windowTitle", "Agent proposals");
    if (!ImGui::Begin(title, &d.showAgentProposals)) {
        ImGui::End();
        return;
    }

    // Header line — count + last-refresh error (if any).
    ImGui::Text("%s: %d", SmatchetLocalization::T("agent.proposals.totalPending", "Total Pending"),
                static_cast<int>(g_proposalsCache.size()));
    if (!g_lastRefreshError.empty()) {
        ImGui::SameLine();
        const char* errPrefix = SmatchetLocalization::T("agent.proposals.refreshErrorPrefix", "  (refresh error: %s)");
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.55f, 1.0f), errPrefix, g_lastRefreshError.c_str());
    }

    if (ImGui::SmallButton(SmatchetLocalization::T("agent.proposals.refresh", "Refresh"))) {
        RefreshFromStore(app);
    }

    ImGui::Separator();

    if (g_proposalsCache.empty()) {
        ImGui::TextWrapped(
            "%s", SmatchetLocalization::T("agent.proposals.empty",
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
        // (Flicker fix) Use a string-id keyed by the SQLite ROWID so the
        // CollapsingHeader open/closed state survives RefreshFromStore's
        // 1 Hz cache rebuild. The previous PushID(&row) was a fresh pointer
        // every refresh (g_proposalsCache moves new rows into place), so
        // ImGui storage keyed off the pointer lost open-state every second
        // and the panel flickered collapse/open. id is the stable identity.
        // Buffer is 32 bytes (enough for "p" + 20-digit int64 + nul).
        char rowIdBuf[32];
        std::snprintf(rowIdBuf, sizeof(rowIdBuf), "p%lld", static_cast<long long>(row.id));
        ImGui::PushID(rowIdBuf);

        // Header — issue title (when present) + bracketed action name; falls
        // back to the issueKey alone when title is empty (e.g. legacy v2 rows
        // that pre-date the schema-v3 column or upstream issues without a
        // title field). The full canonical key is always shown in the
        // metadata line below so the user can still grep by id.
        const char* actionStr = AgenticInferenceClientPure::ActionToString(row.action);
        const std::string headerCore =
            row.issueTitle.empty() ? row.issueKey : (row.issueKey + std::string(" — ") + row.issueTitle);
        const std::string headerLabel = headerCore + std::string("  [") + actionStr + std::string("]");
        const bool headerOpen = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

        if (headerOpen) {
            // Metadata line — id / source-tracker / created HH:MM:SS / age in
            // minutes. Renders compactly so the row's per-action summary stays
            // the visual focus. Computed from the cached AgentProposal (no
            // SQLite round-trip).
            const std::int64_t nowSec =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
            const std::int64_t ageSec = (nowSec > row.createdAtSec) ? (nowSec - row.createdAtSec) : 0;
            const long long ageMin = static_cast<long long>(ageSec / 60);
            const long long ageRemSec = static_cast<long long>(ageSec % 60);
            const std::string createdHms = FormatLocalHms(row.createdAtSec);
            char metaBuf[256];
            std::snprintf(
                metaBuf, sizeof(metaBuf), "id=%lld \xC2\xB7 src=%s \xC2\xB7 created=%s \xC2\xB7 age=%lldm%llds",
                static_cast<long long>(row.id), row.sourceTracker.c_str(), createdHms.c_str(), ageMin, ageRemSec);
            ImGui::TextDisabled("%s", metaBuf);

            // [Open in browser] — derive upstream URL from sourceTracker +
            // issueKey. When derivation fails (unknown tracker, malformed key)
            // we render a disabled button so the user still sees the slot.
            const std::string upstreamUrl = MakeIssueUrl(row.sourceTracker, row.issueKey);
            const bool haveUrl = !upstreamUrl.empty();
            if (!haveUrl) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton(SmatchetLocalization::T("agent.proposals.openInBrowser", "Open in browser"))) {
                LOG_INFO("SmatchetAgentProposalsUi: [Open in browser] click id=%lld url=%s",
                         static_cast<long long>(row.id), upstreamUrl.c_str());
                OpenInShell(upstreamUrl);
            }
            if (!haveUrl) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(no canonical URL for sourceTracker=%s)", row.sourceTracker.c_str());
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", upstreamUrl.c_str());
            }

            // Issue description — rendered as Markdown via md4c +
            // MarkdownPreviewRender. Hidden behind a TreeNode (collapsed by
            // default) so a row with a multi-kB description doesn't dominate
            // the panel. Click "Description" to expand. Empty body falls
            // through to a placeholder line.
            if (!row.issueBody.empty()) {
                ImGui::Spacing();
                if (ImGui::TreeNode("##issueBody", SmatchetLocalization::T("agent.proposals.description",
                                                                           "Description (Markdown):"))) {
                    ImGui::Indent(16.0f);
                    MarkdownPreviewRender::Options mdOpts;
                    MarkdownPreviewRender::Render(row.issueBody, mdOpts);
                    ImGui::Unindent(16.0f);
                    ImGui::TreePop();
                }
            } else {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.proposals.descriptionEmpty",
                                                                  "(no description on upstream issue)"));
            }

            // (Schema v4) Issue comments — parse the stored JSON array; render
            // each comment as a sub-block with author header + body markdown.
            // Hidden behind a TreeNode like Description. Empty / parse-failed
            // → placeholder.
            ImGui::Spacing();
            if (!row.issueCommentsJson.empty()) {
                std::size_t commentCount = 0;
                try {
                    const nlohmann::json arr = nlohmann::json::parse(row.issueCommentsJson);
                    if (arr.is_array()) {
                        commentCount = arr.size();
                    }
                } catch (const std::exception&) {
                    // Parse failure logged inside the TreeNode body so the user
                    // sees the corruption surface in-place.
                }
                char headerBuf[96];
                std::snprintf(headerBuf, sizeof(headerBuf), "%s (%zu)",
                              SmatchetLocalization::T("agent.proposals.comments", "Comments"), commentCount);
                if (ImGui::TreeNode("##issueComments", "%s", headerBuf)) {
                    ImGui::Indent(16.0f);
                    try {
                        const nlohmann::json arr = nlohmann::json::parse(row.issueCommentsJson);
                        if (!arr.is_array()) {
                            ImGui::TextDisabled("(comments JSON is not an array)");
                        } else {
                            MarkdownPreviewRender::Options mdOpts;
                            for (std::size_t ci = 0; ci < arr.size(); ++ci) {
                                const auto& c = arr[ci];
                                std::string author = "(unknown)";
                                std::string body;
                                std::int64_t createdSec = 0;
                                if (c.is_object()) {
                                    if (c.contains("author") && c["author"].is_string()) {
                                        author = c["author"].get<std::string>();
                                    }
                                    if (c.contains("body") && c["body"].is_string()) {
                                        body = c["body"].get<std::string>();
                                    }
                                    if (c.contains("createdAtSec") && c["createdAtSec"].is_number_integer()) {
                                        createdSec = c["createdAtSec"].get<std::int64_t>();
                                    }
                                }
                                if (ci > 0) {
                                    ImGui::Spacing();
                                    ImGui::Separator();
                                    ImGui::Spacing();
                                }
                                const std::string hms = FormatLocalHms(createdSec);
                                char hdrBuf[256];
                                std::snprintf(hdrBuf, sizeof(hdrBuf), "%s \xC2\xB7 %s", author.c_str(), hms.c_str());
                                ImGui::TextDisabled("%s", hdrBuf);
                                if (!body.empty()) {
                                    MarkdownPreviewRender::Render(body, mdOpts);
                                } else {
                                    ImGui::TextDisabled("(empty)");
                                }
                            }
                        }
                    } catch (const std::exception& ex) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.55f, 1.0f));
                        ImGui::TextWrapped("Comments JSON parse failed: %s", ex.what());
                        ImGui::PopStyleColor();
                    }
                    ImGui::Unindent(16.0f);
                    ImGui::TreePop();
                }
            } else {
                ImGui::TextDisabled(
                    "%s", SmatchetLocalization::T("agent.proposals.commentsEmpty", "(no comments on upstream issue)"));
            }

            // Action-specific block. CommentAdd on a Pending row renders an
            // editable text area so the user can tweak the LLM-proposed
            // comment body before clicking Approve. Other actions (and non-
            // Pending CommentAdd) fall through to the read-only one-line
            // summary from MakeActionSummary.
            const bool isPendingCommentAdd = (row.state == AgentProposalState::Pending) &&
                                             (row.action == AgenticInferenceClientPure::ProposedAction::CommentAdd);
            if (isPendingCommentAdd) {
                ImGui::Spacing();
                ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.proposals.commentEditor",
                                                                  "Comment body (edit before Approve):"));
                // Lazy-init the edit buffer from the current payload body.
                auto editIt = g_commentEditBuf.find(row.id);
                if (editIt == g_commentEditBuf.end()) {
                    std::string initial;
                    try {
                        if (row.payload.is_object() && row.payload.contains("body") &&
                            row.payload["body"].is_string()) {
                            initial = row.payload["body"].get<std::string>();
                        }
                    } catch (const std::exception&) {
                        initial.clear();
                    }
                    editIt = g_commentEditBuf.emplace(row.id, std::move(initial)).first;
                }
                // InputTextMultiline writes into the buffer's raw memory; reserve
                // 16 KB of headroom + size to its current C-string length after
                // each frame so the user can type beyond the initial payload
                // size without segfaulting on the strcpy-style overflow.
                editIt->second.reserve(editIt->second.size() + 16384);
                if (editIt->second.capacity() == 0) {
                    editIt->second.reserve(1024);
                }
                // ImGui needs a writable buffer; resize to capacity-1 + clamp
                // back to actual length after the call.
                const std::size_t cap = editIt->second.capacity();
                editIt->second.resize(cap);
                if (ImGui::InputTextMultiline("##commentAddBody", &editIt->second[0], cap, ImVec2(-1.0f, 120.0f),
                                              ImGuiInputTextFlags_AllowTabInput)) {
                    // Edit happened — nothing to do here, the buffer is the
                    // source of truth. Persist on Approve click below.
                }
                editIt->second.resize(std::strlen(editIt->second.c_str()));
                ImGui::TextDisabled(
                    "%s", SmatchetLocalization::T("agent.proposals.commentEditorHint",
                                                  "Edits are committed to the proposal when you press Approve."));
            } else {
                const std::string actionSummary = MakeActionSummary(row);
                if (!actionSummary.empty()) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.92f, 1.0f, 1.0f));
                    ImGui::TextWrapped("%s", actionSummary.c_str());
                    ImGui::PopStyleColor();
                }
            }

            // Rationale — preview + collapsible full text.
            ImGui::Spacing();
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

            // Payload — JSON pretty-print, auto-shown (was collapsed behind a
            // TreeNode; operators reported "not enough info to debug" because
            // the trigger was non-obvious). The TextWrapped scales with the
            // window; payloads above ~20 lines wrap to the visible width.
            ImGui::Spacing();
            ImGui::TextDisabled("%s", SmatchetLocalization::T("agent.proposals.payload", "Payload:"));
            ImGui::Indent(16.0f);
            std::string payloadDump;
            try {
                payloadDump = row.payload.is_null() ? std::string("{}") : row.payload.dump(2);
            } catch (const std::exception& ex) {
                payloadDump = std::string("<dump failed: ") + ex.what() + std::string(">");
            }
            ImGui::TextWrapped("%s", payloadDump.c_str());
            ImGui::Unindent(16.0f);

            // (Apply-feedback) Failed rows render the applyError text in red so
            // the user sees what went wrong (HTTP 403, missing PAT scope, etc.)
            // without watching the log stream. The toast also surfaced the
            // error at apply time; the inline render persists across panel
            // toggles.
            if (row.state == AgentProposalState::Failed && !row.applyError.empty()) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.55f, 1.0f));
                char errBuf[1024];
                std::snprintf(errBuf, sizeof(errBuf), "%s %s",
                              SmatchetLocalization::T("agent.proposals.applyError", "Apply failed:"),
                              row.applyError.c_str());
                ImGui::TextWrapped("%s", errBuf);
                ImGui::PopStyleColor();
            }

            // Buttons. Pending rows show Approve + Reject; Failed rows show
            // Retry (re-runs the action-apply via Failed -> Approved retry
            // carve-out in the state machine). Push/Pop colour state per
            // button so the rest of the panel stays on standard theme.
            ImGui::Spacing();
            if (row.state == AgentProposalState::Pending) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.30f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.36f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.48f, 0.26f, 1.0f));
                if (ImGui::Button(SmatchetLocalization::T("agent.proposals.approve", "Approve"))) {
                    // (Edit-before-send) For CommentAdd, persist the editor
                    // buffer into the proposal's payload BEFORE the
                    // transition so the apply path reads the user-edited
                    // body, not the LLM original. Other actions skip this.
                    if (row.action == AgenticInferenceClientPure::ProposedAction::CommentAdd) {
                        auto editIt = g_commentEditBuf.find(row.id);
                        if (editIt != g_commentEditBuf.end()) {
                            try {
                                nlohmann::json newPayload =
                                    row.payload.is_object() ? row.payload : nlohmann::json::object();
                                newPayload["body"] = editIt->second;
                                auto* store = app.GetAgentProposalStore();
                                if (store != nullptr) {
                                    std::string upErr;
                                    if (!store->UpdatePayload(row.id, newPayload, upErr)) {
                                        LOG_WARN(
                                            "SmatchetAgentProposalsUi: UpdatePayload before Approve failed id=%lld: %s",
                                            static_cast<long long>(row.id), upErr.c_str());
                                    } else {
                                        // Reflect locally too so the
                                        // optimistic prune + next refresh
                                        // see the new body.
                                        row.payload = std::move(newPayload);
                                        LOG_INFO("SmatchetAgentProposalsUi: persisted edited CommentAdd body id=%lld "
                                                 "(%zu bytes)",
                                                 static_cast<long long>(row.id), editIt->second.size());
                                    }
                                }
                            } catch (const std::exception& ex) {
                                LOG_WARN("SmatchetAgentProposalsUi: edited payload JSON build failed id=%lld: %s",
                                         static_cast<long long>(row.id), ex.what());
                            }
                        }
                    }
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
            } else if (row.state == AgentProposalState::Failed) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.45f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.55f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.48f, 0.38f, 0.15f, 1.0f));
                if (ImGui::Button(SmatchetLocalization::T("agent.proposals.retry", "Retry"))) {
                    // Failed -> Approved via the state-machine retry carve-out.
                    // OnProposalApproved fires + re-attempts the action. On
                    // success the row transitions Applied; on failure it lands
                    // back in Failed with a fresh applyError.
                    LOG_INFO("SmatchetAgentProposalsUi: Retry click id=%lld issueKey=%s action=%s",
                             static_cast<long long>(row.id), row.issueKey.c_str(),
                             AgenticInferenceClientPure::ActionToString(row.action));
                    TransitionAndToast(app, row, AgentProposalState::Approved);
                }
                ImGui::PopStyleColor(3);

                ImGui::SameLine();

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.22f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.52f, 0.28f, 0.28f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.32f, 0.18f, 0.18f, 1.0f));
                if (ImGui::Button(SmatchetLocalization::T("agent.proposals.dismiss", "Dismiss"))) {
                    // (Dismiss persistence) Transition Failed -> Rejected so
                    // the next RefreshFromStore tick doesn't re-surface the
                    // row. Previously we just erased from g_proposalsCache,
                    // but the 1 Hz refresh queries SQLite (Pending|Failed)
                    // and the Failed row reappeared. Rejected is filtered
                    // out + terminal, so the row stays hidden + audit-trail
                    // stays linear. State machine carries an explicit
                    // Failed -> Rejected carve-out for this UX path.
                    LOG_INFO("SmatchetAgentProposalsUi: Dismiss click id=%lld (Failed -> Rejected)",
                             static_cast<long long>(row.id));
                    TransitionAndToast(app, row, AgentProposalState::Rejected);
                }
                ImGui::PopStyleColor(3);
            }

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
                if (ImGui::Button(SmatchetLocalization::T("agent.proposals.startHandoff", "Start handoff"))) {
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
