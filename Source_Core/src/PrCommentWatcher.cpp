// PrCommentWatcher — H7 PR-thread watcher.
//
// One `Tick()` call per scheduled-poll iteration; the watcher itself owns no
// thread. See PrCommentWatcher.h for the design rationale.
//
// Bot-filter source of truth: `AgenticHandoffController::IsHandoffBotComment`
// (the same `<!-- smatchet-handoff -->` marker used by H5's issue-comment
// path; H7's budget-exhausted comment carries the same marker so a hostile
// next tick cannot mistake it for a user reply).

#include "PrCommentWatcher.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposalStore.h"
#include "Logger.h"
#include "PrCommentClassifier.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace smatchet {
namespace agentic {

namespace {

int ClampBudget(int budget) {
    // Mirror the ConfigManager clamp shape: [1, 50]. Below 1 the watcher
    // would loop forever; above 50 the operator would never notice the
    // runaway. Values in this range are forgiving enough that an operator
    // overriding via config-edit is never silently ignored.
    if (budget < 1) {
        return 1;
    }
    if (budget > 50) {
        return 50;
    }
    return budget;
}

// Compare GitHub comment-id strings as numeric values when possible.
// GitHub renders comment IDs as monotonically-increasing integers in string
// form; lexicographic comparison mis-orders varying-length numeric strings
// ("9" > "100" lexically). Falls back to lex compare when either side is
// non-numeric (defensive — preserves ordering on synthetic test fixtures).
bool CommentIdLessOrEqual(const std::string& a, const std::string& b) {
    auto isAllDigits = [](const std::string& s) {
        if (s.empty()) {
            return false;
        }
        return std::all_of(s.begin(), s.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    };
    if (a.empty()) {
        return true; // empty baseline → every comment is "newer"
    }
    if (b.empty()) {
        return false;
    }
    if (isAllDigits(a) && isAllDigits(b)) {
        if (a.size() != b.size()) {
            return a.size() < b.size();
        }
        return a <= b; // same-length all-digit strings compare correctly lex
    }
    return a <= b;
}

} // namespace

PrCommentWatcher::PrCommentWatcher(AgenticHandoffController* controller, int iterationBudget, WatchMode mode)
    : controller_(controller), iterationBudget_(ClampBudget(iterationBudget)), mode_(mode) {}

PrCommentWatcher::~PrCommentWatcher() = default;

void PrCommentWatcher::SetPrCommentFetcher(PrCommentFetcher fetcher) { fetcher_ = std::move(fetcher); }
void PrCommentWatcher::SetPrCommentPoster(PrCommentPoster poster) { poster_ = std::move(poster); }
void PrCommentWatcher::SetRespawnDispatcher(HarnessRespawnDispatcher dispatcher) {
    dispatcher_ = std::move(dispatcher);
}
void PrCommentWatcher::SetOpenPrRespawnDispatcher(OpenPrRespawnDispatcher dispatcher) {
    openPrDispatcher_ = std::move(dispatcher);
}
void PrCommentWatcher::SetReviewThreadResolver(ReviewThreadResolver resolver) {
    threadResolver_ = std::move(resolver);
}
void PrCommentWatcher::SetClassifier(std::unique_ptr<PrCommentClassifier> classifier) {
    classifier_ = std::move(classifier);
}
WatchMode PrCommentWatcher::GetMode() const { return mode_; }

void PrCommentWatcher::SetIterationBudget(int budget) { iterationBudget_.store(ClampBudget(budget)); }
int PrCommentWatcher::GetIterationBudget() const { return iterationBudget_.load(); }

void PrCommentWatcher::SetProposalStore(AgentProposalStore* store) { store_ = store; }

int PrCommentWatcher::LoadCursorsFromStore() {
    // H10 — hydrate the in-memory controller cursor + iteration count from
    // persisted agent_pr_watch rows. Best-effort: failures LOG_WARN and the
    // affected handoff falls back to "first poll" semantics (cursor=0,
    // iterationCount=0). The watcher proceeds; the user sees at most one
    // duplicate respawn dispatch on the first post-restart tick.
    if (!controller_ || !store_) {
        return 0;
    }
    const auto handoffs = controller_->SnapshotPrOpen();
    if (handoffs.empty()) {
        return 0;
    }
    int loaded = 0;
    for (const auto& h : handoffs) {
        AgentProposalStore::PrWatchRow row;
        std::string err;
        if (!store_->GetPrWatch(h.proposalId, row, err)) {
            LOG_WARN("PrCommentWatcher::LoadCursorsFromStore: GetPrWatch failed for proposalId=%lld: %s",
                     static_cast<long long>(h.proposalId), err.c_str());
            continue;
        }
        if (row.proposalId == 0) {
            // No persisted row yet — handoff was created since the last
            // persistence pass. Start the in-memory cursor at zero (default).
            continue;
        }
        // The controller's MarkHandoffIteration accepts an explicit cursor
        // value + bumps iterationCount by 1; replaying it with the persisted
        // count would double-count. Mirror the persisted count directly by
        // looping the controller-side helper; since we hold the lock under
        // MarkHandoffIteration, just call the helper iterationCount times to
        // walk the count up. The cursor is set on the final call.
        //
        // Trade-off: this fires iterationCount audit-trail rows on startup.
        // For typical budgets (<= 10) that is acceptable; future variants
        // can expose a direct "hydrate from persisted row" helper that
        // bypasses the audit fire.
        const int targetCount = row.iterationCount;
        const std::int64_t targetCursor = row.lastPolledAtSec;
        // (CR Bundle A6) The persisted column now stores the backend-stable
        // comment id, not a seconds-as-string fallback. Older rows written
        // before A6 carry a stringified second value; that decays gracefully
        // — `(seconds, "12345") > (seconds, "")` still admits any future
        // same-second comment with a non-empty id.
        const std::string targetCursorIdStr = row.lastSeenCommentIdStr;
        std::string markErr;
        for (int i = 0; i < targetCount; ++i) {
            // Cursor only matters on the final call; pass targetCursor on
            // the last iteration so the in-memory record matches the
            // persisted snapshot.
            const bool isLast = (i == targetCount - 1);
            const std::int64_t cursorThisCall = isLast ? targetCursor : static_cast<std::int64_t>(0);
            const std::string idThisCall = isLast ? targetCursorIdStr : std::string();
            if (!controller_->MarkHandoffIteration(h.proposalId, cursorThisCall, idThisCall, markErr)) {
                LOG_WARN("PrCommentWatcher::LoadCursorsFromStore: MarkHandoffIteration(%lld) failed: %s",
                         static_cast<long long>(h.proposalId), markErr.c_str());
                break;
            }
        }
        ++loaded;
        LOG_INFO("PrCommentWatcher::LoadCursorsFromStore: hydrated cursor for proposalId=%lld (count=%d, cursor=%lld)",
                 static_cast<long long>(h.proposalId), targetCount, static_cast<long long>(targetCursor));
    }
    return loaded;
}

namespace {

// (H10) Helper — persist the current in-memory state of a handoff to the
// agent_pr_watch row so the cursor survives an app restart. Best-effort:
// failures LOG_WARN and the in-memory state remains the source of truth
// for the rest of the session.
void PersistWatchRowBestEffort(AgentProposalStore* store, std::int64_t proposalId, const std::string& prUrl,
                               std::int64_t cursorSec, const std::string& cursorIdStr, int iterationCount) {
    if (!store) {
        return;
    }
    AgentProposalStore::PrWatchRow row;
    row.proposalId = proposalId;
    row.prUrl = prUrl;
    // (CR Bundle A6) The `lastSeenCommentIdStr` column is the canonical home
    // for the backend-stable comment id at the cursor. The watcher's tuple
    // compare uses `(cursorSec, id)`; persisting both lets a restarted session
    // resume without dropping a same-second comment. Empty cursorIdStr is
    // valid (first-poll case) — the column stores an empty string then.
    row.lastSeenCommentIdStr = cursorIdStr;
    row.iterationCount = iterationCount;
    row.lastPolledAtSec = cursorSec;
    std::string err;
    if (!store->SetPrWatch(row, err)) {
        LOG_WARN("PrCommentWatcher: SetPrWatch best-effort failed for proposalId=%lld: %s",
                 static_cast<long long>(proposalId), err.c_str());
    }
}

} // namespace

std::string PrCommentWatcher::BuildBudgetExhaustedCommentBody(std::int64_t proposalId, int iterationsUsed) {
    // Marker prefix matches AgenticHandoffController::HandoffBotMarker() so
    // the next tick's bot-filter skips our own posted comment. Keep the
    // body short — operators read this in the PR thread and scan for the
    // proposalId.
    std::ostringstream oss;
    oss << AgenticHandoffController::HandoffBotMarker() << '\n'
        << '\n'
        << "**PR-iteration budget exhausted (proposal #" << proposalId << "):**\n"
        << '\n'
        << "Smatchet reached the configured iteration limit (" << iterationsUsed
        << " respawns). The handoff is marked Failed. Operator intervention is required to continue — "
        << "address the remaining feedback manually or raise `handoff_pr_iteration_budget` "
        << "in Preferences > Agentic and restart the handoff.\n";
    return oss.str();
}

bool PrCommentWatcher::ParsePrKeyFromUrl(const std::string& prUrl, std::string& outPrKey, std::string& outError) {
    // GitHub PR URL shape: `https://github.com/<owner>/<repo>/pull/<N>` (with
    // an optional trailing `/`, query string, or `#` fragment that we
    // tolerate by chopping off at the first non-digit after the N).
    // Enterprise GitHub: `https://github.acme.com/<owner>/<repo>/pull/<N>` —
    // host is irrelevant to the parse; we look for the `/<owner>/<repo>/pull/<N>`
    // tail.
    outPrKey.clear();
    outError.clear();
    if (prUrl.empty()) {
        outError = "PR URL is empty.";
        return false;
    }
    // Strip the scheme.
    std::string rest = prUrl;
    auto schemeEnd = rest.find("://");
    if (schemeEnd != std::string::npos) {
        rest = rest.substr(schemeEnd + 3);
    }
    // Strip the host (everything up to the first '/').
    auto firstSlash = rest.find('/');
    if (firstSlash == std::string::npos || firstSlash + 1 >= rest.size()) {
        outError = "PR URL missing path after host.";
        return false;
    }
    rest = rest.substr(firstSlash + 1);
    // Split on '/'; expect at least 4 segments: owner / repo / "pull" / N.
    std::vector<std::string> segs;
    std::string acc;
    for (char c : rest) {
        if (c == '/' || c == '?' || c == '#') {
            if (!acc.empty()) {
                segs.push_back(std::move(acc));
                acc.clear();
            }
            if (c == '?' || c == '#') {
                // Stop scanning — query / fragment trail after the N.
                break;
            }
        } else {
            acc.push_back(c);
        }
    }
    if (!acc.empty()) {
        segs.push_back(std::move(acc));
    }
    if (segs.size() < 4) {
        outError = "PR URL must have shape /<owner>/<repo>/pull/<N>.";
        return false;
    }
    if (segs[2] != "pull") {
        outError = "PR URL third segment is not 'pull'.";
        return false;
    }
    const std::string& nstr = segs[3];
    if (nstr.empty()) {
        outError = "PR URL number segment is empty.";
        return false;
    }
    if (!std::all_of(nstr.begin(), nstr.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        outError = "PR URL number segment is not numeric.";
        return false;
    }
    outPrKey = segs[0] + "/" + segs[1] + "#" + nstr;
    return true;
}

int PrCommentWatcher::Tick() {
    if (!controller_) {
        return 0;
    }
    if (!fetcher_) {
        if (!fetcherWarnLatched_.exchange(true)) {
            LOG_INFO("PrCommentWatcher::Tick: no PR-comment fetcher wired — watcher is a no-op until "
                     "AppController binds GitHubClient::FetchPrComments");
        }
        return 0;
    }

    if (mode_ == WatchMode::OpenPrScan) {
        // (coderabbit-react phase-4) OpenPrScan-mode tick. Iterate
        // `agent_open_pr_watch` rows the OpenPrRegistrar wrote; per-row
        // cursor + iteration count persist on the row directly.
        if (!store_) {
            // Mirror the fetcher-null latch — OpenPrScan is meaningless
            // without the registrar's table.
            if (!dispatcherWarnLatched_.exchange(true)) {
                LOG_INFO("PrCommentWatcher::Tick[OpenPrScan]: AgentProposalStore not wired — watcher is a "
                         "no-op until AppController binds it");
            }
            return 0;
        }
        std::vector<AgentProposalStore::OpenPrWatchRow> rows;
        std::string listErr;
        if (!store_->ListOpenPrWatch(rows, listErr)) {
            LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: ListOpenPrWatch failed: %s", listErr.c_str());
            return 0;
        }
        const int budget = iterationBudget_.load();
        int dispatched = 0;
        for (const auto& row : rows) {
            std::string prKey;
            std::string keyErr;
            if (!ParsePrKeyFromUrl(row.prUrl, prKey, keyErr)) {
                LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: malformed PR URL '%s' (%s) — skipping",
                         row.prUrl.c_str(), keyErr.c_str());
                continue;
            }

            // Pre-flight budget check — post the marker comment + skip if
            // already at cap. The OpenPrScan budget lives on the persisted
            // `iteration_count` column (no controller-side FSM here).
            if (row.iterationCount >= budget) {
                if (poster_) {
                    const std::string body = BuildBudgetExhaustedCommentBody(/*proposalId*/ 0, row.iterationCount);
                    std::string postErr;
                    if (!poster_(prKey, body, postErr)) {
                        LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: failed to post budget-exhausted marker for "
                                 "prUrl=%s: %s", row.prUrl.c_str(), postErr.c_str());
                    }
                }
                LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: budget exhausted on prUrl=%s (used=%d budget=%d)",
                         row.prUrl.c_str(), row.iterationCount, budget);
                continue;
            }

            std::vector<PostedComment> comments;
            std::string fetchErr;
            if (!fetcher_(prKey, comments, fetchErr)) {
                LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: fetch failed (%s) for prUrl=%s", fetchErr.c_str(),
                         row.prUrl.c_str());
                continue;
            }
            // Mirror OriginTracking's tuple-sort. The OpenPrScan row only
            // persists the id-string cursor (no separate `cursorSec` column),
            // so the tuple key here is just `id` — but sorting by
            // `(createdAtSec, id)` keeps the per-tick processing order
            // deterministic for the test rig + future operators reading the
            // audit trail.
            std::stable_sort(comments.begin(), comments.end(), [](const PostedComment& a, const PostedComment& b) {
                if (a.createdAtSec != b.createdAtSec) {
                    return a.createdAtSec < b.createdAtSec;
                }
                return a.id < b.id;
            });
            const std::string& baselineId = row.lastSeenCommentIdStr;
            // Find the first comment whose id is strictly greater than the
            // baseline id-string. The `agent_open_pr_watch` row carries only
            // the id-cursor today; comparing by id alone is consistent with
            // the registrar's seed (empty initial cursor advances to the
            // first comment's id on the first poll). Use the numeric-safe
            // comparator — GitHub comment IDs are numeric strings and
            // lexicographic comparison mis-orders varying-length values.
            const PostedComment* firstNewComment = nullptr;
            for (const auto& c : comments) {
                if (CommentIdLessOrEqual(c.id, baselineId)) {
                    continue;
                }
                firstNewComment = &c;
                break;
            }
            if (!firstNewComment) {
                continue;
            }
            // Bot-filter check — Smatchet-own markers are skipped regardless
            // of classifier (the classifier is allowed to also short-circuit
            // them, but the bot-filter is the load-bearing safety net so a
            // missing classifier registration doesn't dispatch our own
            // marker post back at the watcher).
            if (AgenticHandoffController::IsHandoffBotComment(firstNewComment->body)) {
                std::string cursorErr;
                (void)store_->SetOpenPrCommentCursor(row.prUrl, firstNewComment->id, cursorErr);
                continue;
            }
            // Classifier hook (Risk #1 correction) — runs BEFORE the
            // default dispatch. Three verdict branches:
            ClassificationResult cls;
            if (classifier_) {
                cls = classifier_->Classify(*firstNewComment);
            } else {
                // No classifier registered — fall through to Dispatch with
                // an empty target so the OpenPrRespawnDispatcher binding
                // can decide what to do. Phase-4 + phase-7 are needed to
                // make CodeRabbit feedback actionable; without them the
                // watcher just routes everything to whatever the
                // OpenPrRespawnDispatcher does.
                cls.verdict = ClassifierVerdict::Dispatch;
                cls.dispatchTargetAgent.clear();
            }
            // Cursor-advance happens regardless of verdict so a Skip /
            // Reject doesn't re-fire on the next tick. We use the
            // dedicated `SetOpenPrCommentCursor` helper (NOT
            // `SetOpenPrWatch`) so the registrar's head-tracking columns
            // stay intact.
            std::string cursorErr;
            if (!store_->SetOpenPrCommentCursor(row.prUrl, firstNewComment->id, cursorErr)) {
                LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: SetOpenPrCommentCursor failed for prUrl=%s: %s",
                         row.prUrl.c_str(), cursorErr.c_str());
                continue;
            }
            if (cls.verdict == ClassifierVerdict::Skip) {
                LOG_INFO("PrCommentWatcher::Tick[OpenPrScan]: Skip verdict on prUrl=%s (reason=%s)",
                         row.prUrl.c_str(), cls.skipReason.c_str());
                continue;
            }
            if (cls.verdict == ClassifierVerdict::RejectShortCircuit) {
                if (poster_) {
                    std::string postErr;
                    if (!poster_(prKey, cls.rejectReplyBody, postErr)) {
                        LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: reject-reply post failed for prUrl=%s: %s",
                                 row.prUrl.c_str(), postErr.c_str());
                    }
                }
                // (phase-7) Resolve the CodeRabbit review thread via GraphQL
                // after posting the reply. Unblocks the merge-gates plan's
                // unresolved-thread gate cleanly. Failure is non-fatal — the
                // reply already landed; manual resolve is a recoverable
                // follow-up. Skipped when the seam is unwired (tests or
                // operators that opted out).
                if (threadResolver_ && !firstNewComment->id.empty()) {
                    std::string resolveErr;
                    if (!threadResolver_(firstNewComment->id, resolveErr)) {
                        LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: review-thread resolve failed for prUrl=%s "
                                 "commentId=%s: %s",
                                 row.prUrl.c_str(), firstNewComment->id.c_str(), resolveErr.c_str());
                    }
                }
                LOG_INFO("PrCommentWatcher::Tick[OpenPrScan]: RejectShortCircuit on prUrl=%s (%s)",
                         row.prUrl.c_str(), cls.rejectRuleCitation.c_str());
                continue;
            }
            // ClassifierVerdict::Dispatch.
            if (!openPrDispatcher_) {
                if (!openPrDispatcherWarnLatched_.exchange(true)) {
                    LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: open-PR dispatch not wired yet — Dispatch "
                             "verdict on prUrl=%s (target=%s) recorded but cannot spawn (phase-7 binding)",
                             row.prUrl.c_str(), cls.dispatchTargetAgent.c_str());
                }
                continue;
            }
            std::string dispatchErr;
            if (!openPrDispatcher_(row.prUrl, firstNewComment->body, cls.dispatchTargetAgent, dispatchErr)) {
                LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: openPrDispatcher failed for prUrl=%s: %s",
                         row.prUrl.c_str(), dispatchErr.c_str());
                continue;
            }
            // Bump iteration_count atomically.
            int newCount = 0;
            std::string bumpErr;
            if (!store_->IncrementOpenPrIterationCount(row.prUrl, newCount, bumpErr)) {
                LOG_WARN("PrCommentWatcher::Tick[OpenPrScan]: IncrementOpenPrIterationCount failed for prUrl=%s: %s",
                         row.prUrl.c_str(), bumpErr.c_str());
            }
            ++dispatched;
            LOG_INFO("PrCommentWatcher::Tick[OpenPrScan]: dispatched respawn for prUrl=%s (iteration %d of %d, "
                     "target=%s)",
                     row.prUrl.c_str(), newCount, budget, cls.dispatchTargetAgent.c_str());
        }
        return dispatched;
    }

    // ─── OriginTracking mode (the H7 default) ─────────────────────────────
    //
    // Snapshot all PrOpen handoffs under the controller's mutex. Iterate
    // outside the lock — each fetch can be hundreds of ms.
    const auto handoffs = controller_->SnapshotPrOpen();
    if (handoffs.empty()) {
        return 0;
    }

    const int budget = iterationBudget_.load();
    int dispatched = 0;

    for (const auto& h : handoffs) {
        // Pre-flight: budget already at or above the cap? Trip the
        // exhausted path immediately so we never miss the boundary across
        // restarts (the in-memory record could be reconstructed from
        // SQLite in a future H10).
        if (h.iterationCount >= budget) {
            std::string transErr;
            if (controller_->MarkHandoffBudgetExhausted(h.proposalId, budget, transErr)) {
                // Best-effort post the marker comment.
                std::string prKey;
                std::string keyErr;
                if (poster_ && ParsePrKeyFromUrl(h.prUrl, prKey, keyErr)) {
                    const std::string body = BuildBudgetExhaustedCommentBody(h.proposalId, h.iterationCount);
                    std::string postErr;
                    if (!poster_(prKey, body, postErr)) {
                        LOG_WARN("PrCommentWatcher::Tick: failed to post budget-exhausted marker for proposalId=%lld: %s",
                                 static_cast<long long>(h.proposalId), postErr.c_str());
                    }
                }
                LOG_WARN("PrCommentWatcher::Tick: budget exhausted on proposalId=%lld (used=%d budget=%d)",
                         static_cast<long long>(h.proposalId), h.iterationCount, budget);
                // H10 — handoff is terminal; clean up the persisted cursor row
                // so the table does not accumulate dead entries. Best-effort.
                if (store_) {
                    std::string delErr;
                    (void)store_->DeletePrWatch(h.proposalId, delErr);
                }
                ++dispatched;
            } else {
                LOG_WARN("PrCommentWatcher::Tick: MarkHandoffBudgetExhausted failed: %s", transErr.c_str());
            }
            continue;
        }

        std::string prKey;
        std::string keyErr;
        if (!ParsePrKeyFromUrl(h.prUrl, prKey, keyErr)) {
            LOG_WARN("PrCommentWatcher::Tick: malformed PR URL '%s' on proposalId=%lld (%s) — skipping",
                     h.prUrl.c_str(), static_cast<long long>(h.proposalId), keyErr.c_str());
            continue;
        }

        std::vector<PostedComment> comments;
        std::string fetchErr;
        if (!fetcher_(prKey, comments, fetchErr)) {
            LOG_WARN("PrCommentWatcher::Tick: fetch failed (%s) for proposalId=%lld", fetchErr.c_str(),
                     static_cast<long long>(h.proposalId));
            continue;
        }
        // (CR Bundle A6) Sort by `(createdAtSec, id)` so multiple comments
        // sharing the same `createdAtSec` are processed in a deterministic
        // backend-stable order. GitHub returns oldest-first but does not
        // guarantee a secondary key when the second-granularity timestamp
        // collides; sorting here removes the ambiguity. A copy is cheap —
        // PR comment threads are short.
        std::stable_sort(comments.begin(), comments.end(), [](const PostedComment& a, const PostedComment& b) {
            if (a.createdAtSec != b.createdAtSec) {
                return a.createdAtSec < b.createdAtSec;
            }
            return a.id < b.id;
        });
        // (CR Bundle A6) Find the first non-bot comment whose
        // `(createdAtSec, id)` tuple is strictly greater than the cursor's
        // `(prCommentCursorSec, prCommentCursorIdStr)` tuple. The prior
        // single-key `createdAtSec <= baseline` test dropped same-second
        // comments authored by different users on rapid review.
        const std::int64_t baselineSec = h.prCommentCursorSec;
        const std::string& baselineId = h.prCommentCursorIdStr;
        const PostedComment* firstNewComment = nullptr;
        for (const auto& c : comments) {
            const bool tupleGreater = (c.createdAtSec > baselineSec) ||
                                      (c.createdAtSec == baselineSec && c.id > baselineId);
            if (!tupleGreater) {
                continue;
            }
            if (AgenticHandoffController::IsHandoffBotComment(c.body)) {
                continue;
            }
            firstNewComment = &c;
            break;
        }
        if (!firstNewComment) {
            continue;
        }
        const std::string body = firstNewComment->body;
        const std::int64_t bodyTs = firstNewComment->createdAtSec;
        const std::string bodyId = firstNewComment->id;

        // (coderabbit-react phase-4 / Risk #1 correction) Classifier hook
        // runs BEFORE the default dispatch in OriginTracking mode too.
        // Three verdict branches: Skip / RejectShortCircuit / Dispatch.
        // Cursor-advance happens regardless of verdict so a Skip / Reject
        // does not re-fire on the next tick.
        ClassificationResult cls;
        if (classifier_) {
            cls = classifier_->Classify(*firstNewComment);
        } else {
            cls.verdict = ClassifierVerdict::Dispatch;
        }

        // Advance the cursor BEFORE dispatching so a concurrent tick (or a
        // dispatcher that takes longer than the poll interval) cannot pick
        // up the same comment.
        std::string markErr;
        if (!controller_->MarkHandoffIteration(h.proposalId, bodyTs, bodyId, markErr)) {
            LOG_WARN("PrCommentWatcher::Tick: MarkHandoffIteration failed (%s) for proposalId=%lld — skipping",
                     markErr.c_str(), static_cast<long long>(h.proposalId));
            continue;
        }
        // H10 — persist the post-Mark in-memory state to agent_pr_watch.
        // After MarkHandoffIteration the in-memory iterationCount is
        // (h.iterationCount + 1); we mirror that to the row.
        PersistWatchRowBestEffort(store_, h.proposalId, h.prUrl, bodyTs, bodyId, h.iterationCount + 1);

        if (cls.verdict == ClassifierVerdict::Skip) {
            LOG_INFO("PrCommentWatcher::Tick: Skip verdict on proposalId=%lld (reason=%s)",
                     static_cast<long long>(h.proposalId), cls.skipReason.c_str());
            continue;
        }
        if (cls.verdict == ClassifierVerdict::RejectShortCircuit) {
            if (poster_) {
                std::string postErr;
                if (!poster_(prKey, cls.rejectReplyBody, postErr)) {
                    LOG_WARN("PrCommentWatcher::Tick: reject-reply post failed for proposalId=%lld: %s",
                             static_cast<long long>(h.proposalId), postErr.c_str());
                }
            }
            // (phase-7) Same review-thread resolve seam as OpenPrScan.
            if (threadResolver_ && !bodyId.empty()) {
                std::string resolveErr;
                if (!threadResolver_(bodyId, resolveErr)) {
                    LOG_WARN("PrCommentWatcher::Tick: review-thread resolve failed for proposalId=%lld commentId=%s: %s",
                             static_cast<long long>(h.proposalId), bodyId.c_str(), resolveErr.c_str());
                }
            }
            LOG_INFO("PrCommentWatcher::Tick: RejectShortCircuit on proposalId=%lld (%s)",
                     static_cast<long long>(h.proposalId), cls.rejectRuleCitation.c_str());
            continue;
        }
        // ClassifierVerdict::Dispatch.
        if (!dispatcher_) {
            if (!dispatcherWarnLatched_.exchange(true)) {
                LOG_INFO("PrCommentWatcher::Tick: respawn dispatcher not wired — recording iteration but cannot "
                         "respawn (operator must intervene)");
            }
            continue;
        }
        // dispatchTargetAgent is logged for parity with OpenPrScan but is
        // not (yet) plumbed into the proposalId-based dispatcher seam —
        // phase-7 widens that seam.
        std::string respawnErr;
        if (!dispatcher_(h.proposalId, body, respawnErr)) {
            LOG_WARN("PrCommentWatcher::Tick: respawn dispatcher failed (%s) for proposalId=%lld", respawnErr.c_str(),
                     static_cast<long long>(h.proposalId));
            continue;
        }
        ++dispatched;
        LOG_INFO("PrCommentWatcher::Tick: dispatched respawn for proposalId=%lld (iteration %d of %d, target=%s)",
                 static_cast<long long>(h.proposalId), h.iterationCount + 1, budget,
                 cls.dispatchTargetAgent.c_str());
    }
    return dispatched;
}

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
