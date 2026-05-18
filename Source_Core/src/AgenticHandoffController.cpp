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
#include <fstream>
#include <sstream>
#include <utility>

namespace smatchet {
namespace agentic {

namespace {

std::int64_t NowUnixSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
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

void NoopAuditSink(const std::string&, const std::string&, const std::string&, bool, const std::string&,
                   const nlohmann::json&) {}

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

// ─── H5 bot-filter + comment formatting helpers ─────────────────────────────

// The literal lives here as a single source of truth — H7's PrCommentWatcher
// pulls it via this accessor so both watchers stay in sync if the marker ever
// needs to change. CRITICAL: changing the literal silently breaks every
// in-flight handoff's poll loop until the next start — they will treat our
// own posted question as a user reply and call ProvideClarification with the
// question text as the answer (cf. the anti-deception note in the H5 packet).
const char* AgenticHandoffController::HandoffBotMarker() { return "<!-- smatchet-handoff -->"; }

bool AgenticHandoffController::IsHandoffBotComment(const std::string& body) {
    const std::string marker = HandoffBotMarker();
    // Tolerate up to a few leading whitespace bytes — GitHub's REST API
    // round-trips comment bodies byte-for-byte but a paranoid future feature
    // (trailing-whitespace strip on a Smatchet-side mutation, say) shouldn't
    // tip a comment over the bot-detection edge.
    std::size_t i = 0;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) {
        ++i;
    }
    if (body.size() - i < marker.size()) {
        return false;
    }
    return std::equal(marker.begin(), marker.end(), body.begin() + static_cast<std::ptrdiff_t>(i));
}

std::string AgenticHandoffController::BuildClarificationCommentBody(std::int64_t proposalId,
                                                                    const std::string& question) {
    std::ostringstream os;
    os << HandoffBotMarker() << "\n\n"
       << "**Agent question (proposal #" << proposalId << "):**\n\n"
       << question << "\n\n"
       << "Reply to this comment to answer.";
    return os.str();
}

std::string AgenticHandoffController::BuildAnswerCommentBody(std::int64_t proposalId, const std::string& answer) {
    std::ostringstream os;
    os << HandoffBotMarker() << "\n\n"
       << "**User answered (proposal #" << proposalId << "):**\n\n"
       << answer;
    return os.str();
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────

AgenticHandoffController::AgenticHandoffController(WorkerDispatcher dispatcher, CodingHarness::IRunner* runner,
                                                   AgentProposalStore* proposalStore, AuditSink auditSink)
    : dispatcher_(std::move(dispatcher)), runner_(runner), proposalStore_(proposalStore),
      auditSink_(auditSink ? std::move(auditSink) : AuditSink(&NoopAuditSink)) {}

AgenticHandoffController::~AgenticHandoffController() = default;

void AgenticHandoffController::SetGitHubCommentPoster(GitHubCommentPoster poster) { githubPoster_ = std::move(poster); }

void AgenticHandoffController::SetGitHubCommentFetcher(GitHubCommentFetcher fetcher) {
    githubFetcher_ = std::move(fetcher);
}

void AgenticHandoffController::SetGitHubClarificationEnabled(bool enabled) {
    githubClarificationEnabled_.store(enabled);
}

// ─── Audit + transition core ──────────────────────────────────────────────

void AgenticHandoffController::EmitAudit(const std::string& issueKey, CodingHarness::RunState fromState,
                                         CodingHarness::RunState toState, bool success, const std::string& errorMessage,
                                         const std::string& prUrl) {
    nlohmann::json data = nlohmann::json::object();
    data["fromState"] = CodingHarness::RunStateToString(fromState);
    data["toState"] = CodingHarness::RunStateToString(toState);
    if (!prUrl.empty()) {
        data["prUrl"] = prUrl;
    }
    if (!errorMessage.empty()) {
        data["errorMessage"] = errorMessage;
    }
    auditSink_(std::string("HandoffStateTransition"), std::string("agentic"), issueKey, success, errorMessage, data);
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

    // (H5) AwaitingUser side-effect — read CLARIFICATION_NEEDED.json from the
    // worktree and post the question as a GitHub comment when wired. Side-effects
    // run AFTER audit so a poster-side failure does not roll back the FSM
    // transition; the worktree-file channel is canonical.
    if (toState == CodingHarness::RunState::AwaitingUser) {
        ReadAndStashClarificationQuestion(proposalId);
        // Re-read the just-stashed question + issue key under the lock to
        // build the comment.
        std::string question;
        std::string ikForPost;
        {
            std::lock_guard<std::mutex> lk(handoffsMu_);
            auto it = handoffs_.find(proposalId);
            if (it != handoffs_.end()) {
                question = it->second.lastClarificationQuestion;
                ikForPost = it->second.issueKey;
            }
        }
        if (!question.empty() && !ikForPost.empty()) {
            PostClarificationToGitHub(ikForPost, proposalId, question);
        }
    }
    return ok;
}

// ─── H5 helpers ──────────────────────────────────────────────────────────

namespace {

std::string ReadFileText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return std::string();
    }
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

std::string JoinPathLocal(const std::string& dir, const char* name) {
    if (dir.empty()) {
        return std::string(name);
    }
    std::string out = dir;
    if (out.back() != '/' && out.back() != '\\') {
        out.push_back('/');
    }
    out.append(name);
    return out;
}

} // namespace

void AgenticHandoffController::ReadAndStashClarificationQuestion(std::int64_t proposalId) {
    // Snapshot the worktree dir under the lock; the file read happens
    // outside so we never hold the mutex through I/O.
    std::string worktree;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            return;
        }
        worktree = it->second.worktreeDir;
    }
    if (worktree.empty()) {
        return;
    }
    const std::string path = JoinPathLocal(worktree, "CLARIFICATION_NEEDED.json");
    const std::string body = ReadFileText(path);
    if (body.empty()) {
        // No file (or unreadable) — runner emitted AwaitingUser without writing
        // the question file, or the file race-condition lost us. Leave the
        // field empty so the H8 UI can render "AwaitingUser (no question text)".
        LOG_WARN("AgenticHandoffController::ReadAndStashClarificationQuestion: empty/missing %s — proposalId=%lld",
                 path.c_str(), static_cast<long long>(proposalId));
        return;
    }
    std::string question;
    std::int64_t ts = 0;
    try {
        nlohmann::json j = nlohmann::json::parse(body);
        if (j.is_object()) {
            question = j.value("question", std::string());
            ts = j.value("timestampUnixSec", static_cast<std::int64_t>(0));
        }
    } catch (const std::exception& e) {
        LOG_WARN("AgenticHandoffController::ReadAndStashClarificationQuestion: parse failed (%s) for proposalId=%lld",
                 e.what(), static_cast<long long>(proposalId));
        return;
    }
    if (question.empty()) {
        LOG_WARN(
            "AgenticHandoffController::ReadAndStashClarificationQuestion: question field empty for proposalId=%lld",
            static_cast<long long>(proposalId));
        return;
    }
    const std::int64_t resolvedTs = ts != 0 ? ts : NowUnixSec();
    std::lock_guard<std::mutex> lk(handoffsMu_);
    auto it = handoffs_.find(proposalId);
    if (it == handoffs_.end()) {
        return;
    }
    it->second.lastClarificationQuestion = question;
    it->second.lastClarificationAtSec = resolvedTs;
    // Reset the poll-cursor to "now" so the poll loop ignores any historical
    // comment posted before the runner flagged AwaitingUser.
    it->second.clarificationCommentCursorSec = NowUnixSec();
}

bool AgenticHandoffController::PostClarificationToGitHub(const std::string& issueKey, std::int64_t proposalId,
                                                         const std::string& question) {
    if (!githubClarificationEnabled_.load()) {
        LOG_INFO("AgenticHandoffController: GitHub clarification posting disabled (cfg) — proposalId=%lld",
                 static_cast<long long>(proposalId));
        return false;
    }
    if (!githubPoster_) {
        LOG_INFO("AgenticHandoffController: GitHub comment poster not wired — proposalId=%lld worktree-only",
                 static_cast<long long>(proposalId));
        return false;
    }
    const std::string body = BuildClarificationCommentBody(proposalId, question);
    std::string err;
    if (!githubPoster_(issueKey, body, err)) {
        LOG_WARN("AgenticHandoffController::PostClarificationToGitHub: %s (proposalId=%lld issue=%s)", err.c_str(),
                 static_cast<long long>(proposalId), issueKey.c_str());
        return false;
    }
    LOG_INFO("AgenticHandoffController::PostClarificationToGitHub: posted (proposalId=%lld issue=%s)",
             static_cast<long long>(proposalId), issueKey.c_str());
    return true;
}

bool AgenticHandoffController::PostAnswerToGitHub(const std::string& issueKey, std::int64_t proposalId,
                                                  const std::string& answer) {
    if (!githubClarificationEnabled_.load()) {
        return false;
    }
    if (!githubPoster_) {
        return false;
    }
    const std::string body = BuildAnswerCommentBody(proposalId, answer);
    std::string err;
    if (!githubPoster_(issueKey, body, err)) {
        LOG_WARN("AgenticHandoffController::PostAnswerToGitHub: %s (proposalId=%lld issue=%s)", err.c_str(),
                 static_cast<long long>(proposalId), issueKey.c_str());
        return false;
    }
    LOG_INFO("AgenticHandoffController::PostAnswerToGitHub: posted (proposalId=%lld issue=%s)",
             static_cast<long long>(proposalId), issueKey.c_str());
    return true;
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
        h.issueKey = row.issueKey; // (H5) — used by the GitHub-comment dual-channel
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
        ControllerTransition(proposalId, CodingHarness::RunState::Failed, "proposal vanished mid-flight: " + lookupErr,
                             std::string());
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
    CodingHarness::IRunner::DeltaCallback onDelta = [proposalId](const CodingHarness::StreamEvent& ev) {
        // H4 has no consumer for delta events; H8's UI panel will tap
        // in. For now we just LOG_TRACE so test runs can see the wire.
        LOG_TRACE("AgenticHandoffController: proposalId=%lld delta type=%s", static_cast<long long>(proposalId),
                  ev.type.c_str());
        (void)proposalId;
        (void)ev;
    };
    CodingHarness::IRunner::StateChangeCallback onStateChange = [this, proposalId](const std::string& newState) {
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
        ControllerTransition(proposalId, CodingHarness::RunState::Cancelled, "cancelled by operator", result.prUrl);
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
    std::string issueKey;
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
        issueKey = it->second.issueKey;
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
    if (!runner_->Resume(rsp, tok, outError)) {
        return false;
    }

    // (H5) Local channel succeeded — post a confirming comment to the GitHub
    // issue so the public thread carries the audit trail. Failure to post is
    // non-fatal (the worktree-file channel is canonical) but is LOG_WARN'd.
    if (!issueKey.empty()) {
        PostAnswerToGitHub(issueKey, proposalId, answer);
    }
    // Audit-trail entry — distinct action so downstream queries can filter
    // ClarificationProvided rows separately from HandoffStateTransition rows.
    // The answer payload is included verbatim — operators occasionally need
    // to see what was sent. The audit layer is responsible for any
    // PII / secret redaction (BackendAuditTrail::AppendEvent already runs
    // its redaction sweep on every event payload).
    {
        nlohmann::json data = nlohmann::json::object();
        data["proposalId"] = proposalId;
        data["answer"] = answer;
        const bool postedToGithub = githubClarificationEnabled_.load() && static_cast<bool>(githubPoster_);
        data["postedToGithub"] = postedToGithub;
        auditSink_(std::string("ClarificationProvided"), std::string("agentic"), issueKey, true, std::string(), data);
    }
    // Clear the stashed question so the H8 UI panel + the poll loop both see
    // "no question pending" until the runner emits AwaitingUser again.
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it != handoffs_.end()) {
            it->second.lastClarificationQuestion.clear();
            it->second.lastClarificationAtSec = 0;
        }
    }
    return true;
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

// ─── H5 poll-loop driver ────────────────────────────────────────────────
//
// Piggybacks on T7's existing scheduled-poll worker (see
// `AppController::AgenticPollWorkerLoop`). Rather than spinning a 4th poll
// thread, the T7 loop calls this method at the end of each iteration; the
// network cost is one extra `FetchIssueComments` per in-flight AwaitingUser
// handoff (typically 0–2). The poll cadence is identical to the issue-triage
// cadence (cfg.AgenticPollIntervalSec) so adding `HandoffClarificationPollIntervalSec`
// as a config knob is unnecessary in H5 — operators tune both via the same
// dial. If a future use case demands a faster cadence for clarification
// replies, split out a dedicated worker then.

int AgenticHandoffController::PollClarificationAnswers() {
    if (!githubFetcher_) {
        return 0;
    }
    // Snapshot AwaitingUser handoffs under the lock so we can iterate without
    // holding it through the HTTP fetch (which can be 100s of ms).
    std::vector<ActiveHandoff> awaiting;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        for (const auto& kv : handoffs_) {
            if (kv.second.state == CodingHarness::RunState::AwaitingUser && !kv.second.issueKey.empty()) {
                awaiting.push_back(kv.second);
            }
        }
    }
    int dispatched = 0;
    for (const auto& h : awaiting) {
        std::vector<PostedComment> comments;
        std::string fetchErr;
        if (!githubFetcher_(h.issueKey, comments, fetchErr)) {
            LOG_WARN("AgenticHandoffController::PollClarificationAnswers: fetch failed (%s) for proposalId=%lld",
                     fetchErr.c_str(), static_cast<long long>(h.proposalId));
            continue;
        }
        // Find the first non-bot comment whose createdAt is strictly newer
        // than the per-handoff cursor. GitHub's API returns oldest-first; if
        // multiple post-cursor user replies queued up we treat the FIRST as
        // the answer — operators wanting to amend reply by replying again,
        // which will arrive after the next cursor advance.
        const std::int64_t baseline = h.clarificationCommentCursorSec;
        std::string answer;
        std::int64_t answerTs = 0;
        for (const auto& c : comments) {
            if (c.createdAtSec <= baseline) {
                continue;
            }
            if (IsHandoffBotComment(c.body)) {
                continue;
            }
            answer = c.body;
            answerTs = c.createdAtSec;
            break;
        }
        if (answer.empty()) {
            continue;
        }
        // Advance the cursor BEFORE dispatching the reply so a concurrent
        // poll iteration cannot pick up the same comment.
        {
            std::lock_guard<std::mutex> lk(handoffsMu_);
            auto it = handoffs_.find(h.proposalId);
            if (it != handoffs_.end()) {
                if (answerTs > it->second.clarificationCommentCursorSec) {
                    it->second.clarificationCommentCursorSec = answerTs;
                }
            }
        }
        std::string clarErr;
        if (!ProvideClarification(h.proposalId, answer, clarErr)) {
            // Common case: the runner already resumed via the worktree-file
            // channel and the handoff is no longer in AwaitingUser. That is
            // not an error — log info and move on.
            LOG_INFO("AgenticHandoffController::PollClarificationAnswers: ProvideClarification declined (%s) — "
                     "proposalId=%lld (likely already resumed via worktree file)",
                     clarErr.c_str(), static_cast<long long>(h.proposalId));
            continue;
        }
        ++dispatched;
        LOG_INFO("AgenticHandoffController::PollClarificationAnswers: answered proposalId=%lld via GitHub comment",
                 static_cast<long long>(h.proposalId));
    }
    return dispatched;
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
