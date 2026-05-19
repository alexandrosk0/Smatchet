// PrCheckRunWatcher.cpp — phase-6 of the `coderabbit-react-loop` plan.
// Tick()-only watcher; mirrors `PrCommentWatcher::OpenPrScan` shape. See
// PrCheckRunWatcher.h for the design rationale + verdict-branch table.

#include "PrCheckRunWatcher.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposalStore.h"
#include "CiFailureClassifier.h" // for ParseRunIdFromDetailsUrl static helper
#include "Logger.h"
#include "PrCheckRunClassifier.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace agentic {

namespace {

int ClampBudget(int budget) {
    if (budget < 1) {
        return 1;
    }
    if (budget > 50) {
        return 50;
    }
    return budget;
}

int ClampRerunCap(int cap) {
    if (cap < 0) {
        return 0;
    }
    if (cap > 10) {
        return 10;
    }
    return cap;
}

} // namespace

PrCheckRunWatcher::PrCheckRunWatcher(AgentProposalStore* store, int iterationBudget, int transientRerunCap)
    : store_(store), iterationBudget_(ClampBudget(iterationBudget)),
      transientRerunCap_(ClampRerunCap(transientRerunCap)) {}

PrCheckRunWatcher::~PrCheckRunWatcher() = default;

void PrCheckRunWatcher::SetCheckRunFetcher(CheckRunFetcher fetcher) { checkRunFetcher_ = std::move(fetcher); }
void PrCheckRunWatcher::SetAnnotationFetcher(CheckRunAnnotationFetcher fetcher) {
    annotationFetcher_ = std::move(fetcher);
}
void PrCheckRunWatcher::SetLogTailFetcher(LogTailFetcher fetcher) { logTailFetcher_ = std::move(fetcher); }
void PrCheckRunWatcher::SetWorkflowRerunDispatcher(WorkflowRerunDispatcher dispatcher) {
    rerunDispatcher_ = std::move(dispatcher);
}
void PrCheckRunWatcher::SetCheckRunRespawnDispatcher(CheckRunRespawnDispatcher dispatcher) {
    respawnDispatcher_ = std::move(dispatcher);
}
void PrCheckRunWatcher::SetClassifier(std::unique_ptr<PrCheckRunClassifier> classifier) {
    classifier_ = std::move(classifier);
}

void PrCheckRunWatcher::SetIterationBudget(int budget) { iterationBudget_.store(ClampBudget(budget)); }
int PrCheckRunWatcher::GetIterationBudget() const { return iterationBudget_.load(); }
void PrCheckRunWatcher::SetTransientRerunCap(int cap) { transientRerunCap_.store(ClampRerunCap(cap)); }
int PrCheckRunWatcher::GetTransientRerunCap() const { return transientRerunCap_.load(); }

int PrCheckRunWatcher::GetRerunCount(const std::string& prUrl) const {
    std::lock_guard<std::mutex> lock(rerunCountMu_);
    auto it = rerunCountByPrUrl_.find(prUrl);
    if (it == rerunCountByPrUrl_.end()) {
        return 0;
    }
    return it->second;
}

bool PrCheckRunWatcher::ParseOwnerRepoFromPrUrl(const std::string& prUrl, std::string& outOwner, std::string& outRepo,
                                                std::string& outError) {
    outOwner.clear();
    outRepo.clear();
    outError.clear();
    if (prUrl.empty()) {
        outError = "PR URL is empty.";
        return false;
    }
    std::string rest = prUrl;
    const auto schemeEnd = rest.find("://");
    if (schemeEnd != std::string::npos) {
        rest = rest.substr(schemeEnd + 3);
    }
    const auto firstSlash = rest.find('/');
    if (firstSlash == std::string::npos || firstSlash + 1 >= rest.size()) {
        outError = "PR URL missing path after host.";
        return false;
    }
    rest = rest.substr(firstSlash + 1);
    std::vector<std::string> segs;
    std::string acc;
    for (char c : rest) {
        if (c == '/' || c == '?' || c == '#') {
            if (!acc.empty()) {
                segs.push_back(std::move(acc));
                acc.clear();
            }
            if (c == '?' || c == '#') {
                break;
            }
        } else {
            acc.push_back(c);
        }
    }
    if (!acc.empty()) {
        segs.push_back(std::move(acc));
    }
    if (segs.size() < 4 || segs[2] != "pull") {
        outError = "PR URL must have shape /<owner>/<repo>/pull/<N>.";
        return false;
    }
    if (segs[0].empty() || segs[1].empty()) {
        outError = "PR URL owner or repo segment is empty.";
        return false;
    }
    outOwner = segs[0];
    outRepo = segs[1];
    return true;
}

int PrCheckRunWatcher::Tick() {
    if (!store_) {
        return 0;
    }
    if (!checkRunFetcher_) {
        if (!fetcherWarnLatched_.exchange(true)) {
            LOG_INFO("PrCheckRunWatcher::Tick: no check-run fetcher wired — watcher is a no-op until "
                     "AppController binds GitHubClient::FetchCheckRuns");
        }
        return 0;
    }

    std::vector<AgentProposalStore::OpenPrWatchRow> rows;
    std::string listErr;
    if (!store_->ListOpenPrWatch(rows, listErr)) {
        LOG_WARN("PrCheckRunWatcher::Tick: ListOpenPrWatch failed: %s", listErr.c_str());
        return 0;
    }

    const int budget = iterationBudget_.load();
    const int rerunCap = transientRerunCap_.load();
    int actioned = 0;

    for (const auto& row : rows) {
        // Owner / repo parse — malformed URLs are logged once and skipped;
        // the registrar would not have written a malformed URL, but a
        // hand-edited row could trip this.
        std::string owner;
        std::string repo;
        std::string parseErr;
        if (!ParseOwnerRepoFromPrUrl(row.prUrl, owner, repo, parseErr)) {
            LOG_WARN("PrCheckRunWatcher::Tick: malformed PR URL '%s' (%s) — skipping", row.prUrl.c_str(),
                     parseErr.c_str());
            continue;
        }

        if (row.headSha.empty()) {
            // Without head_sha the FetchCheckRuns endpoint can't be queried.
            // The registrar populates head_sha at upsert time, so this
            // branch fires only when the row was hand-written or migrated
            // from an older schema. Surface for triage but keep going.
            LOG_WARN("PrCheckRunWatcher::Tick: empty head_sha for prUrl=%s — skipping until registrar refresh",
                     row.prUrl.c_str());
            continue;
        }

        std::vector<GitHubClient::CheckRun> runs;
        std::string fetchErr;
        if (!checkRunFetcher_(owner, repo, row.headSha, runs, fetchErr)) {
            LOG_WARN("PrCheckRunWatcher::Tick: FetchCheckRuns failed for prUrl=%s: %s", row.prUrl.c_str(),
                     fetchErr.c_str());
            continue;
        }

        // Filter to failed runs newer than the cursor + sort ascending by
        // id so cursor advance is monotonic. GitHub returns check-runs in
        // an unspecified order; sorting locally is cheap (single-digit
        // entries per commit typically).
        std::vector<GitHubClient::CheckRun> failed;
        failed.reserve(runs.size());
        for (const auto& r : runs) {
            if (r.conclusion != "failure") {
                continue;
            }
            if (r.id <= row.lastSeenCheckRunId) {
                continue;
            }
            failed.push_back(r);
        }
        std::stable_sort(failed.begin(), failed.end(),
                         [](const GitHubClient::CheckRun& a, const GitHubClient::CheckRun& b) { return a.id < b.id; });

        // Per-PR iteration cap pre-flight — if we are already at the cap
        // log + skip; do not advance the cursor (next tick will hit the
        // same line until iteration_count is reset by an operator).
        if (row.iterationCount >= budget) {
            if (!failed.empty()) {
                LOG_WARN("PrCheckRunWatcher::Tick: iteration budget exhausted on prUrl=%s (used=%d budget=%d) — "
                         "%zu failed check-run(s) deferred",
                         row.prUrl.c_str(), row.iterationCount, budget, failed.size());
            }
            continue;
        }

        // Walk failed check-runs in id order; advance the cursor as each
        // is processed. We do not batch the cursor advance to one call at
        // the end of the loop — a partial-failure mid-walk should still
        // commit the cursor for the runs we already handled.
        for (const auto& run : failed) {
            // Smatchet-own-marker hook — placeholder. Smatchet does not
            // author check-runs today; future Smatchet-CI integration
            // could opt out here. The current condition is always false.
            const bool isSmatchetOwn = false;
            if (isSmatchetOwn) {
                std::string cursorErr;
                (void)store_->SetOpenPrCheckRunCursor(row.prUrl, run.id, cursorErr);
                continue;
            }

            // Fetch annotations + log tail. Both are best-effort — the
            // classifier accepts empty inputs.
            std::vector<GitHubClient::CheckRunAnnotation> annots;
            if (annotationFetcher_) {
                std::string aErr;
                if (!annotationFetcher_(owner, repo, run.id, annots, aErr)) {
                    LOG_WARN("PrCheckRunWatcher::Tick: FetchCheckRunAnnotations failed for prUrl=%s "
                             "checkRunId=%lld: %s",
                             row.prUrl.c_str(), static_cast<long long>(run.id), aErr.c_str());
                }
            }
            std::string logTail;
            if (logTailFetcher_) {
                std::string lErr;
                if (!logTailFetcher_(owner, repo, run.detailsUrl, /*tailLines*/ 200, logTail, lErr)) {
                    LOG_WARN("PrCheckRunWatcher::Tick: log-tail fetch failed for prUrl=%s checkRunId=%lld: %s",
                             row.prUrl.c_str(), static_cast<long long>(run.id), lErr.c_str());
                }
            }

            CheckRunClassification cls;
            if (classifier_) {
                cls = classifier_->Classify(run, annots, logTail);
            } else {
                cls.verdict = CheckRunVerdict::Skip;
                cls.skipReason = "no classifier wired";
            }

            // Advance cursor BEFORE acting on the verdict so a dispatcher
            // failure does not re-fire the same run on the next tick.
            std::string cursorErr;
            if (!store_->SetOpenPrCheckRunCursor(row.prUrl, run.id, cursorErr)) {
                LOG_WARN("PrCheckRunWatcher::Tick: SetOpenPrCheckRunCursor failed for prUrl=%s: %s", row.prUrl.c_str(),
                         cursorErr.c_str());
                // Continue — losing the cursor means a duplicate dispatch
                // on the next tick, which is preferable to silently
                // skipping the action entirely.
            }

            if (cls.verdict == CheckRunVerdict::Skip) {
                LOG_INFO("PrCheckRunWatcher::Tick: Skip on prUrl=%s checkRunId=%lld (reason=%s)", row.prUrl.c_str(),
                         static_cast<long long>(run.id), cls.skipReason.c_str());
                continue;
            }

            if (cls.verdict == CheckRunVerdict::RerunTransient) {
                // Apply the per-PR rerun cap. The classifier returns the
                // verdict unconditionally; the cap is a watcher concern
                // so it stays a single source of truth.
                const int currentReruns = GetRerunCount(row.prUrl);
                if (currentReruns >= rerunCap) {
                    LOG_WARN("PrCheckRunWatcher::Tick: transient rerun cap exceeded on prUrl=%s "
                             "(reruns=%d cap=%d) — skipping rerun of checkRunId=%lld",
                             row.prUrl.c_str(), currentReruns, rerunCap, static_cast<long long>(run.id));
                    continue;
                }
                if (!rerunDispatcher_) {
                    if (!rerunDispatcherWarnLatched_.exchange(true)) {
                        LOG_WARN("PrCheckRunWatcher::Tick: workflow-rerun dispatcher not wired — "
                                 "RerunTransient verdict on prUrl=%s recorded but cannot rerun",
                                 row.prUrl.c_str());
                    }
                    continue;
                }
                std::string rerunErr;
                if (!rerunDispatcher_(owner, repo, cls.rerunWorkflowId, rerunErr)) {
                    LOG_WARN("PrCheckRunWatcher::Tick: RerunWorkflowRun failed for prUrl=%s runId=%lld: %s",
                             row.prUrl.c_str(), static_cast<long long>(cls.rerunWorkflowId), rerunErr.c_str());
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(rerunCountMu_);
                    rerunCountByPrUrl_[row.prUrl] = currentReruns + 1;
                }
                ++actioned;
                LOG_INFO("PrCheckRunWatcher::Tick: RerunTransient fired for prUrl=%s runId=%lld (rerun %d of %d)",
                         row.prUrl.c_str(), static_cast<long long>(cls.rerunWorkflowId), currentReruns + 1, rerunCap);
                continue;
            }

            // CheckRunVerdict::Dispatch.
            if (!respawnDispatcher_) {
                if (!respawnDispatcherWarnLatched_.exchange(true)) {
                    LOG_WARN("PrCheckRunWatcher::Tick: open-PR respawn dispatcher not wired yet — Dispatch "
                             "verdict on prUrl=%s checkRunId=%lld (target=%s source=%s) recorded but cannot "
                             "spawn (phase-7 binding)",
                             row.prUrl.c_str(), static_cast<long long>(run.id), cls.dispatchTargetAgent.c_str(),
                             cls.dispatchSource.c_str());
                }
                continue;
            }
            std::string dispatchErr;
            if (!respawnDispatcher_(row.prUrl, cls.dispatchSource, cls.dispatchTargetAgent, run, dispatchErr)) {
                LOG_WARN("PrCheckRunWatcher::Tick: open-PR respawn dispatcher failed for prUrl=%s "
                         "checkRunId=%lld: %s",
                         row.prUrl.c_str(), static_cast<long long>(run.id), dispatchErr.c_str());
                continue;
            }
            int newCount = 0;
            std::string bumpErr;
            if (!store_->IncrementOpenPrIterationCount(row.prUrl, newCount, bumpErr)) {
                LOG_WARN("PrCheckRunWatcher::Tick: IncrementOpenPrIterationCount failed for prUrl=%s: %s",
                         row.prUrl.c_str(), bumpErr.c_str());
            }
            ++actioned;
            LOG_INFO("PrCheckRunWatcher::Tick: dispatched respawn for prUrl=%s checkRunId=%lld "
                     "(iteration %d of %d, target=%s source=%s)",
                     row.prUrl.c_str(), static_cast<long long>(run.id), newCount, budget,
                     cls.dispatchTargetAgent.c_str(), cls.dispatchSource.c_str());
            // If this dispatch tipped us at the budget, stop walking
            // further failed check-runs for this PR — leave them for the
            // next tick (when iteration_count is the budget value the
            // pre-flight up top will short-circuit cleanly).
            if (newCount >= budget) {
                break;
            }
        }
    }
    return actioned;
}

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
