// AgenticHandoffController — drives one HarnessRunState lifecycle end-to-end.
// FSM transitions are validated at the controller boundary — runner callbacks
// never directly set state; they go through ControllerTransition() which
// audit-trails first.

#include "AgenticHandoffController.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposalStore.h"
#include "ICodingHarnessRunner.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <utility>

namespace smatchet {
namespace agentic {

namespace {

std::int64_t NowUnixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Kebab-case ASCII transform — every char becomes lower-case alnum or `-`.
// Runs of non-alnum collapse to a single `-`; leading/trailing dashes are
// stripped. Empty input -> empty output. The 32-char cap is the caller's
// responsibility (BuildShortSlug applies it after transformation).
std::string KebabCase(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool lastDash = true; // treat start-of-string as "just saw a dash" so the
                          // output never starts with one.
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            lastDash = false;
        } else if (!lastDash) {
            out.push_back('-');
            lastDash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

void NoopAuditSink(const std::string&, const std::string&, const std::string&, bool,
                   const std::string&, const nlohmann::json&) {}

} // namespace

// ─── Static helpers ────────────────────────────────────────────────────────

std::string AgenticHandoffController::BuildShortSlug(const std::string& issueKey) {
    // Plan decision #1: short-slug = first 32 chars of kebab-case(issueKey).
    // For H4 we use issueKey directly since `fetch-title-on-demand` is H7.
    std::string slug = KebabCase(issueKey);
    if (slug.size() > 32) {
        slug.resize(32);
        // After truncation we may end on a `-` — strip it for tidiness.
        while (!slug.empty() && slug.back() == '-') {
            slug.pop_back();
        }
    }
    if (slug.empty()) {
        slug = "issue";
    }
    return slug;
}

std::string AgenticHandoffController::BuildBranchName(std::int64_t proposalId, const std::string& issueKey) {
    std::ostringstream os;
    os << "agent/" << proposalId << "/" << BuildShortSlug(issueKey);
    return os.str();
}

std::string AgenticHandoffController::BuildWorktreeDir(std::int64_t proposalId) {
    std::ostringstream os;
    os << ".claude/worktrees/agent-" << proposalId;
    return os.str();
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────

AgenticHandoffController::AgenticHandoffController(WorkerDispatcher dispatcher, CodingHarness::IRunner* runner,
                                                   AgentProposalStore* proposalStore, AuditSink auditSink)
    : dispatcher_(std::move(dispatcher)), runner_(runner), proposalStore_(proposalStore),
      auditSink_(auditSink ? std::move(auditSink) : AuditSink(&NoopAuditSink)) {}

AgenticHandoffController::~AgenticHandoffController() = default;

// ─── Audit + transition core ──────────────────────────────────────────────

void AgenticHandoffController::EmitAudit(const std::string& issueKey, CodingHarness::RunState fromState,
                                         CodingHarness::RunState toState, bool success,
                                         const std::string& errorMessage, const std::string& prUrl) {
    nlohmann::json data = nlohmann::json::object();
    data["fromState"] = CodingHarness::RunStateToString(fromState);
    data["toState"] = CodingHarness::RunStateToString(toState);
    if (!prUrl.empty()) {
        data["prUrl"] = prUrl;
    }
    if (!errorMessage.empty()) {
        data["errorMessage"] = errorMessage;
    }
    auditSink_(std::string("HandoffStateTransition"), std::string("agentic"), issueKey, success,
               errorMessage, data);
}

bool AgenticHandoffController::ControllerTransition(std::int64_t proposalId, CodingHarness::RunState toState,
                                                    const std::string& errorMessage, const std::string& prUrl) {
    ActiveHandoff snap;
    bool ok = false;
    std::string issueKey;
    CodingHarness::RunState fromState = CodingHarness::RunState::Pending;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            LOG_WARN("AgenticHandoffController::ControllerTransition: unknown proposalId=%lld",
                     static_cast<long long>(proposalId));
            return false;
        }
        fromState = it->second.state;
        // We need the issue key for the audit entry. Read it from the store
        // proactively so we do not hold the proposal-store lock + handoffs
        // mutex at once. Best effort — on store-read failure we fall back
        // to "unknown" so the audit row is still emitted.
        // (Looked up post-unlock below.)
        if (!CodingHarness::IsTransitionAllowed(fromState, toState)) {
            LOG_WARN("AgenticHandoffController::ControllerTransition: disallowed %s -> %s for proposalId=%lld",
                     CodingHarness::RunStateToString(fromState), CodingHarness::RunStateToString(toState),
                     static_cast<long long>(proposalId));
            return false;
        }
        it->second.state = toState;
        if (!errorMessage.empty()) {
            it->second.lastError = errorMessage;
        }
        if (!prUrl.empty()) {
            it->second.prUrl = prUrl;
        }
        snap = it->second;
        ok = true;
    }

    if (proposalStore_) {
        AgentProposal row;
        std::string lookupErr;
        if (proposalStore_->Find(proposalId, row, lookupErr)) {
            issueKey = row.issueKey;
        }
    }

    // success flag passed to audit: every successful transition (including
    // transitions INTO Failed) records success=true at the audit-row level
    // (the row was successfully appended) but carries the errorMessage in
    // its payload. The audit-row's success bit is repurposed to mean "the
    // run reached a Complete state" only — Failed transitions set it false
    // so downstream queries can filter. Cancelled is also false (operator
    // cancelled — not a success).
    const bool auditSuccess = (toState == CodingHarness::RunState::Complete);
    EmitAudit(issueKey, fromState, toState, auditSuccess, errorMessage, prUrl);

    LOG_INFO("AgenticHandoffController: proposalId=%lld %s -> %s", static_cast<long long>(proposalId),
             CodingHarness::RunStateToString(fromState), CodingHarness::RunStateToString(toState));
    (void)snap;
    return ok;
}

// ─── Start / worker dispatch ──────────────────────────────────────────────

bool AgenticHandoffController::Start(std::int64_t proposalId, ActiveHandoff& outHandoff, std::string& outError) {
    outHandoff = ActiveHandoff();
    outError.clear();

    if (runner_ == nullptr) {
        outError = "no runner configured";
        return false;
    }
    if (proposalStore_ == nullptr) {
        outError = "no proposal store configured";
        return false;
    }

    // 1. Look up proposal + validate it's an ImplementIssue.
    AgentProposal row;
    std::string lookupErr;
    if (!proposalStore_->Find(proposalId, row, lookupErr)) {
        outError = "proposal lookup failed: " + lookupErr;
        return false;
    }
    if (row.action != AgenticInferenceClientPure::ProposedAction::ImplementIssue) {
        outError = "proposal action is not ImplementIssue (triage-only)";
        return false;
    }

    // 2. Refuse duplicate in-flight start.
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it != handoffs_.end() && !CodingHarness::IsTerminal(it->second.state)) {
            outError = "handoff already in flight for this proposal";
            return false;
        }

        // 3. Build the in-memory record.
        ActiveHandoff h;
        h.proposalId = proposalId;
        h.worktreeDir = BuildWorktreeDir(proposalId);
        h.branchName = BuildBranchName(proposalId, row.issueKey);
        h.state = CodingHarness::RunState::Pending;
        h.startedAtSec = NowUnixSec();
        h.cancelToken = std::make_shared<std::atomic<bool>>(false);
        handoffs_[proposalId] = h;
        outHandoff = h;
    }

    // 4. Transition Pending -> Spawning (audit).
    if (!ControllerTransition(proposalId, CodingHarness::RunState::Spawning, std::string(), std::string())) {
        outError = "initial Pending->Spawning transition rejected (internal FSM error)";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it != handoffs_.end()) {
            outHandoff = it->second;
        }
    }

    // 5. Dispatch the worker. Tests pass an empty dispatcher and drive the
    // worker through RunSpawnSynchronouslyForTests instead.
    if (dispatcher_) {
        std::int64_t pid = proposalId;
        dispatcher_([this, pid]() { this->RunHandoffWorker(pid); });
    }
    return true;
}

bool AgenticHandoffController::RunSpawnSynchronouslyForTests(std::int64_t proposalId, std::string& outError) {
    outError.clear();
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        if (handoffs_.find(proposalId) == handoffs_.end()) {
            outError = "no handoff for proposalId";
            return false;
        }
    }
    RunHandoffWorker(proposalId);
    return true;
}

void AgenticHandoffController::RunHandoffWorker(std::int64_t proposalId) {
    // Snapshot what the worker needs so we can drop the mutex before the
    // long-running Spawn() call.
    ActiveHandoff snap;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            LOG_WARN("AgenticHandoffController::RunHandoffWorker: proposalId=%lld vanished before spawn",
                     static_cast<long long>(proposalId));
            return;
        }
        snap = it->second;
    }

    // Look up the originating proposal to populate the seed. If the row
    // vanished after Start succeeded (deletion under us), surface a Failed
    // transition and bail.
    AgentProposal row;
    std::string lookupErr;
    if (proposalStore_ == nullptr || !proposalStore_->Find(proposalId, row, lookupErr)) {
        ControllerTransition(proposalId, CodingHarness::RunState::Failed,
                             "proposal vanished mid-flight: " + lookupErr, std::string());
        return;
    }

    CodingHarness::Seed seed;
    seed.proposalId = proposalId;
    seed.sourceTracker = row.sourceTracker;
    seed.issueKey = row.issueKey;
    // H4 ships without a body/title fetch — H7 wires fetch-title-on-demand
    // through the runner's IGitHubReadClient adapter. Until then we feed
    // the seed with the proposal's rationale as a stand-in.
    seed.issueBodyMarkdown = row.rationale;
    if (row.payload.is_object()) {
        seed.approachOutline = row.payload.value("approachOutline", std::string());
        seed.complexityHint = row.payload.value("complexityHint", std::string());
    }
    seed.targetBranch = snap.branchName;
    seed.workingDirectory = snap.worktreeDir;
    seed.timestampUnixSec = NowUnixSec();

    // 6. Spawn the runner. State-change callback funnels into the FSM-checked
    // controller transition. The runner emits canonical state strings — any
    // unknown literal LOG_WARNs and is dropped.
    //
    // H4 ships without a `MainThreadDispatcher` marshal — the only consumer
    // today is the CLI handler (already a worker thread). H8 wires the UI
    // panel and at that point the callback will post to the dispatcher so
    // ImGui state mutates from the UI thread only (pillar 2).
    CodingHarness::IRunner::DeltaCallback onDelta =
        [proposalId](const CodingHarness::StreamEvent& ev) {
            // H4 has no consumer for delta events; H8's UI panel will tap
            // in. For now we just LOG_TRACE so test runs can see the wire.
            LOG_TRACE("AgenticHandoffController: proposalId=%lld delta type=%s", static_cast<long long>(proposalId),
                      ev.type.c_str());
            (void)proposalId;
            (void)ev;
        };
    CodingHarness::IRunner::StateChangeCallback onStateChange =
        [this, proposalId](const std::string& newState) {
            CodingHarness::RunState ts;
            if (!CodingHarness::ParseRunState(newState, ts)) {
                LOG_WARN("AgenticHandoffController: runner emitted unknown state '%s' for proposalId=%lld",
                         newState.c_str(), static_cast<long long>(proposalId));
                return;
            }
            // The runner emits terminal states (Complete / Failed / Cancelled)
            // for self-bookkeeping; the controller owns the final transition
            // so it can attach the RunResult's error / prUrl in one audit
            // entry. Filtering here keeps the FSM crisp and stops the
            // belt-and-braces final transition from being rejected as a
            // duplicate.
            if (CodingHarness::IsTerminal(ts)) {
                return;
            }
            this->ControllerTransition(proposalId, ts, std::string(), std::string());
        };

    CodingHarness::RunResult result;
    std::string spawnErr;
    const bool spawnOk = runner_->Spawn(seed, snap.worktreeDir, std::move(onDelta), std::move(onStateChange),
                                        snap.cancelToken, result, spawnErr);

    // 7. Final FSM transition based on RunResult.
    if (snap.cancelToken && snap.cancelToken->load()) {
        // Cancel atom flipped — record Cancelled even if the runner already
        // self-reported. The FSM rejects no-op transitions on a terminal
        // state so a duplicate Cancelled is silently dropped.
        ControllerTransition(proposalId, CodingHarness::RunState::Cancelled, "cancelled by operator",
                             result.prUrl);
        return;
    }
    if (!spawnOk || !result.ok) {
        const std::string err = result.errorMessage.empty() ? spawnErr : result.errorMessage;
        // The runner may already have emitted Failed via the state callback
        // — if so this is a no-op transition (terminal -> terminal rejected).
        ControllerTransition(proposalId, CodingHarness::RunState::Failed, err, result.prUrl);
        return;
    }
    // The runner reports Complete via its state callback already; this is a
    // belt-and-braces transition that no-ops on the already-terminal record.
    ControllerTransition(proposalId, CodingHarness::RunState::Complete, std::string(), result.prUrl);
}

// ─── Clarification + cancel ──────────────────────────────────────────────

bool AgenticHandoffController::ProvideClarification(std::int64_t proposalId, const std::string& answer,
                                                    std::string& outError) {
    outError.clear();
    std::shared_ptr<std::atomic<bool>> tok;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            outError = "no handoff for proposalId";
            return false;
        }
        if (it->second.state != CodingHarness::RunState::AwaitingUser) {
            outError = "handoff is not in AwaitingUser state";
            return false;
        }
        tok = it->second.cancelToken;
    }
    if (runner_ == nullptr) {
        outError = "no runner configured";
        return false;
    }
    CodingHarness::ClarificationResponse rsp;
    rsp.answer = answer;
    rsp.timestampUnixSec = NowUnixSec();

    // The runner's Resume call may block briefly on a file write; H4 keeps
    // it synchronous on the calling thread (the CLI thread for `handoff.clarify`,
    // the worker thread for the H8 UI panel). UI callers must wrap in
    // LaunchBackgroundTask.
    return runner_->Resume(rsp, tok, outError);
}

bool AgenticHandoffController::Cancel(std::int64_t proposalId, std::string& outError) {
    outError.clear();
    std::shared_ptr<std::atomic<bool>> tok;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            outError = "no handoff for proposalId";
            return false;
        }
        if (CodingHarness::IsTerminal(it->second.state)) {
            outError = "handoff is already terminal";
            return false;
        }
        tok = it->second.cancelToken;
    }
    if (tok) {
        tok->store(true);
    }
    return true;
}

// ─── Snapshot ────────────────────────────────────────────────────────────

std::vector<AgenticHandoffController::ActiveHandoff> AgenticHandoffController::SnapshotActive() const {
    std::vector<ActiveHandoff> out;
    std::lock_guard<std::mutex> lk(handoffsMu_);
    out.reserve(handoffs_.size());
    for (const auto& kv : handoffs_) {
        if (!CodingHarness::IsTerminal(kv.second.state)) {
            out.push_back(kv.second);
        }
    }
    // Stable order: by proposalId ascending.
    std::sort(out.begin(), out.end(),
              [](const ActiveHandoff& a, const ActiveHandoff& b) { return a.proposalId < b.proposalId; });
    return out;
}

std::vector<AgenticHandoffController::ActiveHandoff> AgenticHandoffController::SnapshotAllForTests() const {
    std::vector<ActiveHandoff> out;
    std::lock_guard<std::mutex> lk(handoffsMu_);
    out.reserve(handoffs_.size());
    for (const auto& kv : handoffs_) {
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(),
              [](const ActiveHandoff& a, const ActiveHandoff& b) { return a.proposalId < b.proposalId; });
    return out;
}

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
