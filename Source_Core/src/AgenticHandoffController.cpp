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

// (H5/H6) File-read helpers — shared between PrOpen URL resolution + the
// clarification question stash. Lift into the top anonymous namespace so
// ControllerTransition can call them without forward declarations.
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

// ─── Static helpers ────────────────────────────────────────────────────────

std::string AgenticHandoffController::BuildShortSlug(const std::string& issueKey) {
    LOG_TRACE("AgenticHandoffController::BuildShortSlug enter (issueKey=%s)", issueKey.c_str());
    // Plan decision #1: short-slug = first 32 chars of kebab-case(issueKey).
    // For H4 we use issueKey directly since `fetch-title-on-demand` is H7.
    std::string slug = KebabCase(issueKey);
    if (slug.size() > 32) {
        LOG_DEBUG("BuildShortSlug: truncating slug from %zu to 32 chars (issueKey=%s)", slug.size(), issueKey.c_str());
        slug.resize(32);
        // After truncation we may end on a `-` — strip it for tidiness.
        while (!slug.empty() && slug.back() == '-') {
            slug.pop_back();
        }
    }
    if (slug.empty()) {
        LOG_DEBUG("BuildShortSlug: empty slug after kebab-case, falling back to 'issue' (issueKey=%s)",
                  issueKey.c_str());
        slug = "issue";
    }
    LOG_TRACE("AgenticHandoffController::BuildShortSlug exit (slug=%s)", slug.c_str());
    return slug;
}

std::string AgenticHandoffController::BuildBranchName(std::int64_t proposalId, const std::string& issueKey) {
    LOG_TRACE("AgenticHandoffController::BuildBranchName enter (proposalId=%lld issueKey=%s)",
              static_cast<long long>(proposalId), issueKey.c_str());
    std::ostringstream os;
    os << "agent/" << proposalId << "/" << BuildShortSlug(issueKey);
    const std::string out = os.str();
    LOG_TRACE("AgenticHandoffController::BuildBranchName exit (branch=%s)", out.c_str());
    return out;
}

std::string AgenticHandoffController::BuildWorktreeDir(std::int64_t proposalId) {
    LOG_TRACE("AgenticHandoffController::BuildWorktreeDir enter (proposalId=%lld)", static_cast<long long>(proposalId));
    std::ostringstream os;
    os << ".claude/worktrees/agent-" << proposalId;
    const std::string out = os.str();
    LOG_TRACE("AgenticHandoffController::BuildWorktreeDir exit (dir=%s)", out.c_str());
    return out;
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
    LOG_TRACE("AgenticHandoffController::BuildClarificationCommentBody enter (proposalId=%lld questionBytes=%zu)",
              static_cast<long long>(proposalId), question.size());
    std::ostringstream os;
    os << HandoffBotMarker() << "\n\n"
       << "**Agent question (proposal #" << proposalId << "):**\n\n"
       << question << "\n\n"
       << "Reply to this comment to answer.";
    return os.str();
}

std::string AgenticHandoffController::BuildAnswerCommentBody(std::int64_t proposalId, const std::string& answer) {
    LOG_TRACE("AgenticHandoffController::BuildAnswerCommentBody enter (proposalId=%lld answerBytes=%zu)",
              static_cast<long long>(proposalId), answer.size());
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

void AgenticHandoffController::SetGitHubCommentPoster(GitHubCommentPoster poster) {
    LOG_DEBUG("AgenticHandoffController::SetGitHubCommentPoster (wired=%d)",
              static_cast<int>(static_cast<bool>(poster)));
    githubPoster_ = std::move(poster);
}

void AgenticHandoffController::SetGitHubCommentFetcher(GitHubCommentFetcher fetcher) {
    LOG_DEBUG("AgenticHandoffController::SetGitHubCommentFetcher (wired=%d)",
              static_cast<int>(static_cast<bool>(fetcher)));
    githubFetcher_ = std::move(fetcher);
}

void AgenticHandoffController::SetGitHubClarificationEnabled(bool enabled) {
    LOG_DEBUG("AgenticHandoffController::SetGitHubClarificationEnabled enabled=%d", static_cast<int>(enabled));
    githubClarificationEnabled_.store(enabled);
}

void AgenticHandoffController::SetToastSink(ToastSink sink) {
    LOG_DEBUG("AgenticHandoffController::SetToastSink (wired=%d)", static_cast<int>(static_cast<bool>(sink)));
    toastSink_ = std::move(sink);
}

// ─── Audit + transition core ──────────────────────────────────────────────

void AgenticHandoffController::EmitAudit(const std::string& issueKey, CodingHarness::RunState fromState,
                                         CodingHarness::RunState toState, bool success, const std::string& errorMessage,
                                         const std::string& prUrl) {
    LOG_TRACE("AgenticHandoffController::EmitAudit enter (issueKey=%s %s -> %s success=%d errBytes=%zu prUrlBytes=%zu)",
              issueKey.c_str(), CodingHarness::RunStateToString(fromState), CodingHarness::RunStateToString(toState),
              static_cast<int>(success), errorMessage.size(), prUrl.size());
    nlohmann::json data = nlohmann::json::object();
    data["fromState"] = CodingHarness::RunStateToString(fromState);
    data["toState"] = CodingHarness::RunStateToString(toState);
    if (!prUrl.empty()) {
        data["prUrl"] = prUrl;
    }
    if (!errorMessage.empty()) {
        data["errorMessage"] = errorMessage;
    }
    LOG_DEBUG("AgenticHandoffController::EmitAudit dispatching audit row HandoffStateTransition (issueKey=%s)",
              issueKey.c_str());
    auditSink_(std::string("HandoffStateTransition"), std::string("agentic"), issueKey, success, errorMessage, data);
}

bool AgenticHandoffController::ControllerTransition(std::int64_t proposalId, CodingHarness::RunState toState,
                                                    const std::string& errorMessage, const std::string& prUrl) {
    LOG_TRACE("AgenticHandoffController::ControllerTransition enter (proposalId=%lld toState=%s errBytes=%zu "
              "prUrlBytes=%zu)",
              static_cast<long long>(proposalId), CodingHarness::RunStateToString(toState), errorMessage.size(),
              prUrl.size());
    ActiveHandoff snap;
    bool ok = false;
    std::string issueKey;
    CodingHarness::RunState fromState;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            LOG_WARN("AgenticHandoffController::ControllerTransition: unknown proposalId=%lld",
                     static_cast<long long>(proposalId));
            LOG_TRACE("AgenticHandoffController::ControllerTransition exit reason=unknown-proposalId");
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
            LOG_WARN("dropped transition %s -> %s (handoff %lld) — FSM integrity",
                     CodingHarness::RunStateToString(fromState), CodingHarness::RunStateToString(toState),
                     static_cast<long long>(proposalId));
            LOG_TRACE("AgenticHandoffController::ControllerTransition exit reason=fsm-disallowed");
            return false;
        }
        LOG_DEBUG("ControllerTransition: applying FSM transition %s -> %s (proposalId=%lld)",
                  CodingHarness::RunStateToString(fromState), CodingHarness::RunStateToString(toState),
                  static_cast<long long>(proposalId));
        it->second.state = toState;
        if (!errorMessage.empty()) {
            LOG_DEBUG("ControllerTransition: stamping lastError on handoff (proposalId=%lld bytes=%zu)",
                      static_cast<long long>(proposalId), errorMessage.size());
            it->second.lastError = errorMessage;
        }
        if (!prUrl.empty()) {
            LOG_DEBUG("ControllerTransition: stamping prUrl on handoff (proposalId=%lld url=%s)",
                      static_cast<long long>(proposalId), prUrl.c_str());
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
            LOG_TRACE("ControllerTransition: store lookup ok (proposalId=%lld issueKey=%s)",
                      static_cast<long long>(proposalId), issueKey.c_str());
        } else {
            LOG_DEBUG("ControllerTransition: store lookup failed for proposalId=%lld (%s) — audit emits with empty "
                      "issueKey",
                      static_cast<long long>(proposalId), lookupErr.c_str());
        }
    }

    // (H6) Pre-audit PR-URL resolution. The runner emits "PrOpen" via the
    // bare `(string newState)` state callback which has no slot for a URL.
    // Read PR_URL.txt from the worktree here so the audit payload + toast
    // sink both see the URL on the first transition. The worker-thread
    // final transition path passes the URL directly (line ~568) so this
    // branch is a no-op for that case.
    std::string resolvedPrUrl = prUrl;
    if (toState == CodingHarness::RunState::PrOpen && resolvedPrUrl.empty()) {
        LOG_DEBUG("ControllerTransition: PrOpen with no caller-supplied URL — resolving via PR_URL.txt "
                  "(proposalId=%lld)",
                  static_cast<long long>(proposalId));
        std::string worktree;
        {
            std::lock_guard<std::mutex> lk(handoffsMu_);
            auto it = handoffs_.find(proposalId);
            if (it != handoffs_.end()) {
                worktree = it->second.worktreeDir;
            }
        }
        if (!worktree.empty()) {
            const std::string urlPath = JoinPathLocal(worktree, "PR_URL.txt");
            std::string body = ReadFileText(urlPath);
            LOG_DEBUG("read PR_URL.txt from %s ok=%d bytes=%zu", urlPath.c_str(), static_cast<int>(!body.empty()),
                      body.size());
            while (!body.empty() &&
                   (body.back() == '\n' || body.back() == '\r' || body.back() == ' ' || body.back() == '\t')) {
                body.pop_back();
            }
            resolvedPrUrl = body;
        }
        if (!resolvedPrUrl.empty()) {
            LOG_DEBUG("ControllerTransition: backfilling prUrl on in-memory record (proposalId=%lld url=%s)",
                      static_cast<long long>(proposalId), resolvedPrUrl.c_str());
            // Backfill the in-memory record so the H8 UI panel reads the
            // URL on the next snapshot.
            std::lock_guard<std::mutex> lk(handoffsMu_);
            auto it = handoffs_.find(proposalId);
            if (it != handoffs_.end()) {
                it->second.prUrl = resolvedPrUrl;
            }
        } else {
            LOG_DEBUG("ControllerTransition: PrOpen URL still empty after resolve (proposalId=%lld)",
                      static_cast<long long>(proposalId));
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
    EmitAudit(issueKey, fromState, toState, auditSuccess, errorMessage, resolvedPrUrl);

    LOG_INFO("handoff %lld state %s -> %s", static_cast<long long>(proposalId),
             CodingHarness::RunStateToString(fromState), CodingHarness::RunStateToString(toState));
    LOG_INFO("AgenticHandoffController: proposalId=%lld %s -> %s", static_cast<long long>(proposalId),
             CodingHarness::RunStateToString(fromState), CodingHarness::RunStateToString(toState));
    (void)snap;

    // (H6) PrOpen toast — fires after audit so a sink-side failure does not
    // roll back the FSM transition. Skipped on a degenerate URL (transition
    // fired with no file + no URL) — the audit row still records the
    // transition for downstream queries.
    if (toState == CodingHarness::RunState::PrOpen && toastSink_ && !resolvedPrUrl.empty()) {
        LOG_INFO("PR opened handoff=%lld url=%s", static_cast<long long>(proposalId), resolvedPrUrl.c_str());
        toastSink_(std::string("Agent PR opened: ") + resolvedPrUrl);
    } else if (toState == CodingHarness::RunState::PrOpen) {
        LOG_DEBUG("ControllerTransition: PrOpen toast skipped (proposalId=%lld toastWired=%d urlEmpty=%d)",
                  static_cast<long long>(proposalId), static_cast<int>(static_cast<bool>(toastSink_)),
                  static_cast<int>(resolvedPrUrl.empty()));
    }

    // (H5) AwaitingUser side-effect — read CLARIFICATION_NEEDED.json from the
    // worktree and post the question as a GitHub comment when wired. Side-effects
    // run AFTER audit so a poster-side failure does not roll back the FSM
    // transition; the worktree-file channel is canonical.
    if (toState == CodingHarness::RunState::AwaitingUser) {
        LOG_DEBUG("ControllerTransition: AwaitingUser side-effect — stash + maybe-post clarification "
                  "(proposalId=%lld)",
                  static_cast<long long>(proposalId));
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
            LOG_INFO("clarification round (new question) handoff=%lld bytes=%zu", static_cast<long long>(proposalId),
                     question.size());
            PostClarificationToGitHub(ikForPost, proposalId, question);
        } else {
            LOG_DEBUG("ControllerTransition: AwaitingUser without postable question (proposalId=%lld qEmpty=%d "
                      "issueKeyEmpty=%d)",
                      static_cast<long long>(proposalId), static_cast<int>(question.empty()),
                      static_cast<int>(ikForPost.empty()));
        }
    }
    LOG_TRACE("AgenticHandoffController::ControllerTransition exit (proposalId=%lld ok=%d)",
              static_cast<long long>(proposalId), static_cast<int>(ok));
    return ok;
}

// ─── H5 helpers ──────────────────────────────────────────────────────────
// (ReadFileText + JoinPathLocal lifted to the top anonymous namespace so
// ControllerTransition can use them — see top-of-file.)

void AgenticHandoffController::ReadAndStashClarificationQuestion(std::int64_t proposalId) {
    LOG_TRACE("AgenticHandoffController::ReadAndStashClarificationQuestion enter (proposalId=%lld)",
              static_cast<long long>(proposalId));
    // Snapshot the worktree dir under the lock; the file read happens
    // outside so we never hold the mutex through I/O.
    std::string worktree;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            LOG_TRACE("ReadAndStashClarificationQuestion exit reason=unknown-proposalId");
            return;
        }
        worktree = it->second.worktreeDir;
    }
    if (worktree.empty()) {
        LOG_TRACE("ReadAndStashClarificationQuestion exit reason=empty-worktree (proposalId=%lld)",
                  static_cast<long long>(proposalId));
        return;
    }
    const std::string path = JoinPathLocal(worktree, "CLARIFICATION_NEEDED.json");
    const std::string body = ReadFileText(path);
    LOG_DEBUG("read CLARIFICATION_NEEDED.json from %s ok=%d bytes=%zu", path.c_str(), static_cast<int>(!body.empty()),
              body.size());
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
    LOG_DEBUG("ReadAndStashClarificationQuestion: stashing question (proposalId=%lld bytes=%zu ts=%lld)",
              static_cast<long long>(proposalId), question.size(), static_cast<long long>(resolvedTs));
    std::lock_guard<std::mutex> lk(handoffsMu_);
    auto it = handoffs_.find(proposalId);
    if (it == handoffs_.end()) {
        LOG_TRACE("ReadAndStashClarificationQuestion exit reason=proposal-vanished-after-IO (proposalId=%lld)",
                  static_cast<long long>(proposalId));
        return;
    }
    it->second.lastClarificationQuestion = question;
    it->second.lastClarificationAtSec = resolvedTs;
    // Reset the poll-cursor to "now" so the poll loop ignores any historical
    // comment posted before the runner flagged AwaitingUser.
    it->second.clarificationCommentCursorSec = NowUnixSec();
    LOG_TRACE("ReadAndStashClarificationQuestion exit ok (proposalId=%lld)", static_cast<long long>(proposalId));
}

bool AgenticHandoffController::PostClarificationToGitHub(const std::string& issueKey, std::int64_t proposalId,
                                                         const std::string& question) {
    LOG_TRACE("AgenticHandoffController::PostClarificationToGitHub enter (proposalId=%lld issueKey=%s "
              "questionBytes=%zu)",
              static_cast<long long>(proposalId), issueKey.c_str(), question.size());
    if (!githubClarificationEnabled_.load()) {
        LOG_INFO("AgenticHandoffController: GitHub clarification posting disabled (cfg) — proposalId=%lld",
                 static_cast<long long>(proposalId));
        LOG_TRACE("PostClarificationToGitHub exit reason=disabled-by-cfg");
        return false;
    }
    if (!githubPoster_) {
        LOG_INFO("AgenticHandoffController: GitHub comment poster not wired — proposalId=%lld worktree-only",
                 static_cast<long long>(proposalId));
        LOG_TRACE("PostClarificationToGitHub exit reason=poster-not-wired");
        return false;
    }
    const std::string body = BuildClarificationCommentBody(proposalId, question);
    LOG_DEBUG("PostClarificationToGitHub: invoking poster (proposalId=%lld bodyBytes=%zu)",
              static_cast<long long>(proposalId), body.size());
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
    LOG_TRACE("AgenticHandoffController::PostAnswerToGitHub enter (proposalId=%lld issueKey=%s answerBytes=%zu)",
              static_cast<long long>(proposalId), issueKey.c_str(), answer.size());
    if (!githubClarificationEnabled_.load()) {
        LOG_TRACE("PostAnswerToGitHub exit reason=disabled-by-cfg");
        return false;
    }
    if (!githubPoster_) {
        LOG_TRACE("PostAnswerToGitHub exit reason=poster-not-wired");
        return false;
    }
    const std::string body = BuildAnswerCommentBody(proposalId, answer);
    LOG_DEBUG("PostAnswerToGitHub: invoking poster (proposalId=%lld bodyBytes=%zu)", static_cast<long long>(proposalId),
              body.size());
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
    LOG_TRACE("AgenticHandoffController::Start enter (proposalId=%lld)", static_cast<long long>(proposalId));
    outHandoff = ActiveHandoff();
    outError.clear();

    if (runner_ == nullptr) {
        outError = "no runner configured";
        LOG_ERROR("AgenticHandoffController::Start: no runner configured (proposalId=%lld)",
                  static_cast<long long>(proposalId));
        LOG_TRACE("AgenticHandoffController::Start exit reason=no-runner");
        return false;
    }
    if (proposalStore_ == nullptr) {
        outError = "no proposal store configured";
        LOG_ERROR("AgenticHandoffController::Start: no proposal store configured (proposalId=%lld)",
                  static_cast<long long>(proposalId));
        LOG_TRACE("AgenticHandoffController::Start exit reason=no-store");
        return false;
    }

    // 1. Look up proposal + validate it's an ImplementIssue.
    AgentProposal row;
    std::string lookupErr;
    if (!proposalStore_->Find(proposalId, row, lookupErr)) {
        outError = "proposal lookup failed: " + lookupErr;
        LOG_WARN("AgenticHandoffController::Start: proposal lookup failed (%s) for proposalId=%lld", lookupErr.c_str(),
                 static_cast<long long>(proposalId));
        LOG_TRACE("AgenticHandoffController::Start exit reason=lookup-failed");
        return false;
    }
    LOG_DEBUG("Start: proposal lookup ok (proposalId=%lld issueKey=%s sourceTracker=%s)",
              static_cast<long long>(proposalId), row.issueKey.c_str(), row.sourceTracker.c_str());
    if (row.action != AgenticInferenceClientPure::ProposedAction::ImplementIssue) {
        outError = "proposal action is not ImplementIssue (triage-only)";
        LOG_WARN("AgenticHandoffController::Start: refusing non-ImplementIssue action (proposalId=%lld)",
                 static_cast<long long>(proposalId));
        LOG_TRACE("AgenticHandoffController::Start exit reason=not-implement-issue");
        return false;
    }

    // 2. Refuse duplicate in-flight start.
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it != handoffs_.end() && !CodingHarness::IsTerminal(it->second.state)) {
            outError = "handoff already in flight for this proposal";
            LOG_WARN("AgenticHandoffController::Start: duplicate in-flight start refused (proposalId=%lld state=%s)",
                     static_cast<long long>(proposalId), CodingHarness::RunStateToString(it->second.state));
            LOG_TRACE("AgenticHandoffController::Start exit reason=already-in-flight");
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
        LOG_DEBUG("SQL/insert agent_handoffs (in-memory) id=%lld state=%s branch=%s worktree=%s",
                  static_cast<long long>(proposalId), CodingHarness::RunStateToString(h.state), h.branchName.c_str(),
                  h.worktreeDir.c_str());
        handoffs_[proposalId] = h;
        outHandoff = h;
    }

    // 4. Transition Pending -> Spawning (audit).
    LOG_DEBUG("Start: invoking initial Pending->Spawning transition (proposalId=%lld)",
              static_cast<long long>(proposalId));
    if (!ControllerTransition(proposalId, CodingHarness::RunState::Spawning, std::string(), std::string())) {
        outError = "initial Pending->Spawning transition rejected (internal FSM error)";
        LOG_ERROR("AgenticHandoffController::Start: initial transition rejected (proposalId=%lld)",
                  static_cast<long long>(proposalId));
        LOG_TRACE("AgenticHandoffController::Start exit reason=initial-transition-rejected");
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
        LOG_INFO("spawn handoff proposalId=%lld branch=%s worktree=%s", static_cast<long long>(proposalId),
                 outHandoff.branchName.c_str(), outHandoff.worktreeDir.c_str());
        std::int64_t pid = proposalId;
        dispatcher_([this, pid]() { this->RunHandoffWorker(pid); });
    } else {
        LOG_DEBUG("Start: no dispatcher wired — caller expected to drive worker synchronously (proposalId=%lld)",
                  static_cast<long long>(proposalId));
    }
    LOG_TRACE("AgenticHandoffController::Start exit ok (proposalId=%lld)", static_cast<long long>(proposalId));
    return true;
}

bool AgenticHandoffController::RunSpawnSynchronouslyForTests(std::int64_t proposalId, std::string& outError) {
    LOG_TRACE("AgenticHandoffController::RunSpawnSynchronouslyForTests enter (proposalId=%lld)",
              static_cast<long long>(proposalId));
    outError.clear();
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        if (handoffs_.find(proposalId) == handoffs_.end()) {
            outError = "no handoff for proposalId";
            LOG_TRACE("RunSpawnSynchronouslyForTests exit reason=no-handoff");
            return false;
        }
    }
    RunHandoffWorker(proposalId);
    LOG_TRACE("RunSpawnSynchronouslyForTests exit ok (proposalId=%lld)", static_cast<long long>(proposalId));
    return true;
}

void AgenticHandoffController::RunHandoffWorker(std::int64_t proposalId) {
    LOG_TRACE("AgenticHandoffController::RunHandoffWorker enter (proposalId=%lld)", static_cast<long long>(proposalId));
    // Snapshot what the worker needs so we can drop the mutex before the
    // long-running Spawn() call.
    ActiveHandoff snap;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            LOG_WARN("AgenticHandoffController::RunHandoffWorker: proposalId=%lld vanished before spawn",
                     static_cast<long long>(proposalId));
            LOG_TRACE("RunHandoffWorker exit reason=vanished-before-spawn");
            return;
        }
        snap = it->second;
    }
    LOG_DEBUG("RunHandoffWorker: snapshotted (proposalId=%lld branch=%s worktree=%s state=%s)",
              static_cast<long long>(proposalId), snap.branchName.c_str(), snap.worktreeDir.c_str(),
              CodingHarness::RunStateToString(snap.state));

    // Look up the originating proposal to populate the seed. If the row
    // vanished after Start succeeded (deletion under us), surface a Failed
    // transition and bail.
    AgentProposal row;
    std::string lookupErr;
    if (proposalStore_ == nullptr || !proposalStore_->Find(proposalId, row, lookupErr)) {
        LOG_ERROR("RunHandoffWorker: proposal vanished mid-flight (proposalId=%lld err=%s)",
                  static_cast<long long>(proposalId), lookupErr.c_str());
        ControllerTransition(proposalId, CodingHarness::RunState::Failed, "proposal vanished mid-flight: " + lookupErr,
                             std::string());
        LOG_TRACE("RunHandoffWorker exit reason=proposal-vanished");
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
    LOG_DEBUG("RunHandoffWorker: seed assembled (proposalId=%lld issueKey=%s targetBranch=%s wd=%s bodyBytes=%zu)",
              static_cast<long long>(proposalId), seed.issueKey.c_str(), seed.targetBranch.c_str(),
              seed.workingDirectory.c_str(), seed.issueBodyMarkdown.size());

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
        LOG_TRACE("AgenticHandoffController::onStateChange (proposalId=%lld newState=%s)",
                  static_cast<long long>(proposalId), newState.c_str());
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
            LOG_DEBUG("onStateChange: dropping runner-emitted terminal state '%s' — controller owns final transition "
                      "(proposalId=%lld)",
                      newState.c_str(), static_cast<long long>(proposalId));
            return;
        }
        this->ControllerTransition(proposalId, ts, std::string(), std::string());
    };

    LOG_INFO("RunHandoffWorker: invoking runner Spawn (proposalId=%lld worktree=%s branch=%s)",
             static_cast<long long>(proposalId), snap.worktreeDir.c_str(), snap.branchName.c_str());
    CodingHarness::RunResult result;
    std::string spawnErr;
    const bool spawnOk = runner_->Spawn(seed, snap.worktreeDir, std::move(onDelta), std::move(onStateChange),
                                        snap.cancelToken, result, spawnErr);
    LOG_DEBUG("RunHandoffWorker: runner Spawn returned (proposalId=%lld spawnOk=%d result.ok=%d errBytes=%zu "
              "prUrlBytes=%zu)",
              static_cast<long long>(proposalId), static_cast<int>(spawnOk), static_cast<int>(result.ok),
              result.errorMessage.size(), result.prUrl.size());

    // 7. Final FSM transition based on RunResult.
    if (snap.cancelToken && snap.cancelToken->load()) {
        // Cancel atom flipped — record Cancelled even if the runner already
        // self-reported. The FSM rejects no-op transitions on a terminal
        // state so a duplicate Cancelled is silently dropped.
        LOG_INFO("cancel handoff proposalId=%lld (cancel-token observed after Spawn)",
                 static_cast<long long>(proposalId));
        ControllerTransition(proposalId, CodingHarness::RunState::Cancelled, "cancelled by operator", result.prUrl);
        LOG_TRACE("RunHandoffWorker exit reason=cancelled");
        return;
    }
    if (!spawnOk || !result.ok) {
        const std::string err = result.errorMessage.empty() ? spawnErr : result.errorMessage;
        LOG_ERROR("RunHandoffWorker: spawn failed (proposalId=%lld spawnOk=%d resultOk=%d err=%s)",
                  static_cast<long long>(proposalId), static_cast<int>(spawnOk), static_cast<int>(result.ok),
                  err.c_str());
        // The runner may already have emitted Failed via the state callback
        // — if so this is a no-op transition (terminal -> terminal rejected).
        ControllerTransition(proposalId, CodingHarness::RunState::Failed, err, result.prUrl);
        LOG_TRACE("RunHandoffWorker exit reason=failed");
        return;
    }
    LOG_DEBUG("RunHandoffWorker: success path — transitioning to Complete (proposalId=%lld prUrl=%s)",
              static_cast<long long>(proposalId), result.prUrl.c_str());
    // The runner reports PrOpen via its state callback (sentinel-file poll OR
    // the H6 fallback path's explicit emit). The controller-side PrOpen handler
    // resolves the URL from PR_URL.txt for the audit + toast emission, so by
    // the time we get here the record already has prUrl set. The Complete
    // transition is the terminal — `Running` is no longer the from-state
    // (the FSM rejects Running -> Complete); we transition PrOpen -> Complete
    // for the URL-carrying success path or stay in Running for the rare
    // never-emitted-PrOpen case (which surfaces Failed below).
    ControllerTransition(proposalId, CodingHarness::RunState::Complete, std::string(), result.prUrl);
    LOG_TRACE("RunHandoffWorker exit ok (proposalId=%lld)", static_cast<long long>(proposalId));
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

// ─── H7 PR-watcher integration ────────────────────────────────────────────

std::vector<AgenticHandoffController::ActiveHandoff> AgenticHandoffController::SnapshotPrOpen() const {
    std::vector<ActiveHandoff> out;
    std::lock_guard<std::mutex> lk(handoffsMu_);
    for (const auto& kv : handoffs_) {
        // Only include handoffs that have reached PrOpen but have not been
        // already flagged budget-exhausted. Terminal states (Complete /
        // Failed / Cancelled) are out — those handoffs are done with PR
        // iteration. A handoff that re-enters Running on a respawn briefly
        // leaves this set; the next tick picks it back up after the runner
        // re-reaches PrOpen.
        if (kv.second.state == CodingHarness::RunState::PrOpen && !kv.second.budgetExhausted &&
            !kv.second.prUrl.empty()) {
            out.push_back(kv.second);
        }
    }
    return out;
}

bool AgenticHandoffController::MarkHandoffIteration(std::int64_t proposalId, std::int64_t newCursorSec,
                                                    std::string& outError) {
    std::lock_guard<std::mutex> lk(handoffsMu_);
    auto it = handoffs_.find(proposalId);
    if (it == handoffs_.end()) {
        outError = "Unknown proposalId.";
        return false;
    }
    if (newCursorSec > it->second.prCommentCursorSec) {
        it->second.prCommentCursorSec = newCursorSec;
    }
    ++it->second.iterationCount;
    return true;
}

bool AgenticHandoffController::MarkHandoffIteration(std::int64_t proposalId, std::int64_t newCursorSec,
                                                    const std::string& newCursorIdStr, std::string& outError) {
    // (CR Bundle A6) Tuple-cursor variant — advances the pair so that two
    // comments sharing `createdAtSec` are no longer collapsed.
    std::lock_guard<std::mutex> lk(handoffsMu_);
    auto it = handoffs_.find(proposalId);
    if (it == handoffs_.end()) {
        outError = "Unknown proposalId.";
        return false;
    }
    const std::int64_t curSec = it->second.prCommentCursorSec;
    const std::string& curId = it->second.prCommentCursorIdStr;
    const bool advance = (newCursorSec > curSec) || (newCursorSec == curSec && newCursorIdStr > curId);
    if (advance) {
        it->second.prCommentCursorSec = newCursorSec;
        it->second.prCommentCursorIdStr = newCursorIdStr;
    }
    ++it->second.iterationCount;
    return true;
}

bool AgenticHandoffController::MarkHandoffBudgetExhausted(std::int64_t proposalId, int iterationBudget,
                                                          std::string& outError) {
    // Lookup + idempotency flip under the mutex, then drop the lock before
    // calling ControllerTransition (which takes the same mutex when it
    // updates `it->second.state`). The state read under the lock is the
    // pre-transition value — we use it to decide whether the FSM call is
    // even legal.
    bool alreadyExhausted = false;
    CodingHarness::RunState fromState;
    {
        std::lock_guard<std::mutex> lk(handoffsMu_);
        auto it = handoffs_.find(proposalId);
        if (it == handoffs_.end()) {
            outError = "Unknown proposalId.";
            return false;
        }
        alreadyExhausted = it->second.budgetExhausted;
        fromState = it->second.state;
        it->second.budgetExhausted = true;
    }
    if (alreadyExhausted) {
        // Subsequent ticks: idempotent no-op. The state was already flipped
        // by the first invocation; nothing more to do.
        return true;
    }
    if (CodingHarness::IsTerminal(fromState)) {
        // Already terminal — leave the budget flag set but skip the
        // transition (the FSM would reject `Failed -> Failed`).
        return true;
    }
    std::ostringstream oss;
    oss << "PR-iteration budget exhausted (" << iterationBudget << " respawns). Operator must intervene to continue.";
    return ControllerTransition(proposalId, CodingHarness::RunState::Failed, oss.str(), std::string());
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
