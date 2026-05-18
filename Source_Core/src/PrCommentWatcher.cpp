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

} // namespace

PrCommentWatcher::PrCommentWatcher(AgenticHandoffController* controller, int iterationBudget)
    : controller_(controller), iterationBudget_(ClampBudget(iterationBudget)) {}

PrCommentWatcher::~PrCommentWatcher() = default;

void PrCommentWatcher::SetPrCommentFetcher(PrCommentFetcher fetcher) { fetcher_ = std::move(fetcher); }
void PrCommentWatcher::SetPrCommentPoster(PrCommentPoster poster) { poster_ = std::move(poster); }
void PrCommentWatcher::SetRespawnDispatcher(HarnessRespawnDispatcher dispatcher) {
    dispatcher_ = std::move(dispatcher);
}

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
        std::string markErr;
        for (int i = 0; i < targetCount; ++i) {
            // Cursor only matters on the final call; pass targetCursor on
            // the last iteration so the in-memory record matches the
            // persisted snapshot.
            const std::int64_t cursorThisCall =
                (i == targetCount - 1) ? targetCursor : static_cast<std::int64_t>(0);
            if (!controller_->MarkHandoffIteration(h.proposalId, cursorThisCall, markErr)) {
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
                               std::int64_t cursorSec, int iterationCount) {
    if (!store) {
        return;
    }
    AgentProposalStore::PrWatchRow row;
    row.proposalId = proposalId;
    row.prUrl = prUrl;
    // The watcher tracks comments by `createdAtSec` (unix-seconds), not by
    // GitHub's `id`. We persist the cursor seconds as the id-string column
    // so a future restart can re-derive the cursor without needing a second
    // column; if a later wave switches to id-based dedup, this column
    // becomes the canonical home for the id.
    row.lastSeenCommentIdStr = std::to_string(cursorSec);
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
    for (char c : nstr) {
        if (c < '0' || c > '9') {
            outError = "PR URL number segment is not numeric.";
            return false;
        }
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
        // Find the first non-bot comment newer than the cursor. GitHub returns
        // oldest-first; we honour that ordering so a queue of multiple
        // user comments produces one respawn per tick (the next tick picks
        // up the next).
        const std::int64_t baseline = h.prCommentCursorSec;
        std::string body;
        std::int64_t bodyTs = 0;
        for (const auto& c : comments) {
            if (c.createdAtSec <= baseline) {
                continue;
            }
            if (AgenticHandoffController::IsHandoffBotComment(c.body)) {
                continue;
            }
            body = c.body;
            bodyTs = c.createdAtSec;
            break;
        }
        if (body.empty()) {
            continue;
        }
        // Advance the cursor BEFORE dispatching so a concurrent tick (or a
        // dispatcher that takes longer than the poll interval) cannot pick
        // up the same comment.
        std::string markErr;
        if (!controller_->MarkHandoffIteration(h.proposalId, bodyTs, markErr)) {
            LOG_WARN("PrCommentWatcher::Tick: MarkHandoffIteration failed (%s) for proposalId=%lld — skipping",
                     markErr.c_str(), static_cast<long long>(h.proposalId));
            continue;
        }
        // H10 — persist the post-Mark in-memory state to agent_pr_watch.
        // After MarkHandoffIteration the in-memory iterationCount is
        // (h.iterationCount + 1); we mirror that to the row.
        PersistWatchRowBestEffort(store_, h.proposalId, h.prUrl, bodyTs, h.iterationCount + 1);
        if (!dispatcher_) {
            if (!dispatcherWarnLatched_.exchange(true)) {
                LOG_INFO("PrCommentWatcher::Tick: respawn dispatcher not wired — recording iteration but cannot "
                         "respawn (operator must intervene)");
            }
            continue;
        }
        std::string respawnErr;
        if (!dispatcher_(h.proposalId, body, respawnErr)) {
            LOG_WARN("PrCommentWatcher::Tick: respawn dispatcher failed (%s) for proposalId=%lld", respawnErr.c_str(),
                     static_cast<long long>(h.proposalId));
            continue;
        }
        ++dispatched;
        LOG_INFO("PrCommentWatcher::Tick: dispatched respawn for proposalId=%lld (iteration %d of %d)",
                 static_cast<long long>(h.proposalId), h.iterationCount + 1, budget);
    }
    return dispatched;
}

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
