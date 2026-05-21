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
      transientRerunCap_(ClampRerunCap(transientRerunCap)) {
    LOG_TRACE("PrCheckRunWatcher::ctor enter store=%p iterationBudget=%d transientRerunCap=%d",
              static_cast<void*>(store), iterationBudget, transientRerunCap);
    LOG_DEBUG("PrCheckRunWatcher constructed clamped_budget=%d clamped_rerun_cap=%d", iterationBudget_.load(),
              transientRerunCap_.load());
}

PrCheckRunWatcher::~PrCheckRunWatcher() = default;

void PrCheckRunWatcher::SetCheckRunFetcher(CheckRunFetcher fetcher) {
    LOG_TRACE("PrCheckRunWatcher::SetCheckRunFetcher enter wired=%d", static_cast<int>(static_cast<bool>(fetcher)));
    checkRunFetcher_ = std::move(fetcher);
}
void PrCheckRunWatcher::SetAnnotationFetcher(CheckRunAnnotationFetcher fetcher) {
    LOG_TRACE("PrCheckRunWatcher::SetAnnotationFetcher enter wired=%d", static_cast<int>(static_cast<bool>(fetcher)));
    annotationFetcher_ = std::move(fetcher);
}
void PrCheckRunWatcher::SetLogTailFetcher(LogTailFetcher fetcher) {
    LOG_TRACE("PrCheckRunWatcher::SetLogTailFetcher enter wired=%d", static_cast<int>(static_cast<bool>(fetcher)));
    logTailFetcher_ = std::move(fetcher);
}
void PrCheckRunWatcher::SetWorkflowRerunDispatcher(WorkflowRerunDispatcher dispatcher) {
    LOG_TRACE("PrCheckRunWatcher::SetWorkflowRerunDispatcher enter wired=%d",
              static_cast<int>(static_cast<bool>(dispatcher)));
    rerunDispatcher_ = std::move(dispatcher);
}
void PrCheckRunWatcher::SetCheckRunRespawnDispatcher(CheckRunRespawnDispatcher dispatcher) {
    LOG_TRACE("PrCheckRunWatcher::SetCheckRunRespawnDispatcher enter wired=%d",
              static_cast<int>(static_cast<bool>(dispatcher)));
    respawnDispatcher_ = std::move(dispatcher);
}
void PrCheckRunWatcher::SetClassifier(std::unique_ptr<PrCheckRunClassifier> classifier) {
    LOG_TRACE("PrCheckRunWatcher::SetClassifier enter wired=%d", static_cast<int>(classifier.get() != nullptr));
    classifier_ = std::move(classifier);
}

void PrCheckRunWatcher::SetIterationBudget(int budget) {
    LOG_TRACE("PrCheckRunWatcher::SetIterationBudget enter budget=%d", budget);
    iterationBudget_.store(ClampBudget(budget));
    LOG_DEBUG("PrCheckRunWatcher iteration budget set to clamped=%d", iterationBudget_.load());
}
int PrCheckRunWatcher::GetIterationBudget() const {
    LOG_TRACE("PrCheckRunWatcher::GetIterationBudget enter");
    return iterationBudget_.load();
}
void PrCheckRunWatcher::SetTransientRerunCap(int cap) {
    LOG_TRACE("PrCheckRunWatcher::SetTransientRerunCap enter cap=%d", cap);
    transientRerunCap_.store(ClampRerunCap(cap));
    LOG_DEBUG("PrCheckRunWatcher transient rerun cap set to clamped=%d", transientRerunCap_.load());
}
int PrCheckRunWatcher::GetTransientRerunCap() const {
    LOG_TRACE("PrCheckRunWatcher::GetTransientRerunCap enter");
    return transientRerunCap_.load();
}

int PrCheckRunWatcher::GetRerunCount(const std::string& prUrl) const {
    LOG_TRACE("PrCheckRunWatcher::GetRerunCount enter prUrl=%s", prUrl.c_str());
    std::lock_guard<std::mutex> lock(rerunCountMu_);
    auto it = rerunCountByPrUrl_.find(prUrl);
    if (it == rerunCountByPrUrl_.end()) {
        return 0;
    }
    return it->second;
}

bool PrCheckRunWatcher::ParseOwnerRepoFromPrUrl(const std::string& prUrl, std::string& outOwner, std::string& outRepo,
                                                std::string& outError) {
    LOG_TRACE("PrCheckRunWatcher::ParseOwnerRepoFromPrUrl enter prUrl=%s", prUrl.c_str());
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
    LOG_TRACE("PrCheckRunWatcher::Tick enter");
    if (!store_) {
        LOG_DEBUG("PrCheckRunWatcher::Tick: store_ is null — no-op");
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
    int dispatched = 0;
    int reran = 0;
    int skipped = 0;
    LOG_INFO("ci watcher tick begin rows=%zu budget=%d rerun_cap=%d", rows.size(), budget, rerunCap);

    for (const auto& row : rows) {
        LOG_TRACE("PrCheckRunWatcher::Tick row prUrl=%s prNumber=%d iterationCount=%d lastSeenCheckRunId=%lld",
                  row.prUrl.c_str(), row.prNumber, row.iterationCount, static_cast<long long>(row.lastSeenCheckRunId));
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
        LOG_DEBUG("HTTP GET check-runs sha=%s", row.headSha.c_str());
        if (!checkRunFetcher_(owner, repo, row.headSha, runs, fetchErr)) {
            LOG_WARN("PrCheckRunWatcher::Tick: FetchCheckRuns failed for prUrl=%s: %s", row.prUrl.c_str(),
                     fetchErr.c_str());
            continue;
        }
        LOG_DEBUG("HTTP %d check_runs_returned=%zu", 200, runs.size());

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

            LOG_TRACE("PrCheckRunWatcher::Tick processing check-run id=%lld name=%s conclusion=%s",
                      static_cast<long long>(run.id), run.name.c_str(), run.conclusion.c_str());
            // Fetch annotations + log tail. Both are best-effort — the
            // classifier accepts empty inputs.
            std::vector<GitHubClient::CheckRunAnnotation> annots;
            if (annotationFetcher_) {
                std::string aErr;
                LOG_DEBUG("HTTP GET annotations check_run=%lld", static_cast<long long>(run.id));
                if (!annotationFetcher_(owner, repo, run.id, annots, aErr)) {
                    LOG_WARN("PrCheckRunWatcher::Tick: FetchCheckRunAnnotations failed for prUrl=%s "
                             "checkRunId=%lld: %s",
                             row.prUrl.c_str(), static_cast<long long>(run.id), aErr.c_str());
                } else {
                    LOG_DEBUG("HTTP GET annotations check_run=%lld count=%zu", static_cast<long long>(run.id),
                              annots.size());
                }
            }
            std::string logTail;
            if (logTailFetcher_) {
                std::string lErr;
                LOG_DEBUG("HTTP GET job logs check_run=%lld tail_lines=%d", static_cast<long long>(run.id), 200);
                if (!logTailFetcher_(owner, repo, run.detailsUrl, /*tailLines*/ 200, logTail, lErr)) {
                    LOG_WARN("PrCheckRunWatcher::Tick: log-tail fetch failed for prUrl=%s checkRunId=%lld: %s",
                             row.prUrl.c_str(), static_cast<long long>(run.id), lErr.c_str());
                } else {
                    LOG_DEBUG("HTTP GET job logs check_run=%lld lines=%zu", static_cast<long long>(run.id),
                              logTail.size());
                }
            }

            CheckRunClassification cls;
            if (classifier_) {
                cls = classifier_->Classify(run, annots, logTail);
            } else {
                cls.verdict = CheckRunVerdict::Skip;
                cls.skipReason = "no classifier wired";
            }
            const char* verdictStr = (cls.verdict == CheckRunVerdict::Dispatch)         ? "Dispatch"
                                     : (cls.verdict == CheckRunVerdict::RerunTransient) ? "RerunTransient"
                                                                                        : "Skip";
            LOG_DEBUG("classify check-run name=%s conclusion=%s verdict=%s", run.name.c_str(), run.conclusion.c_str(),
                      verdictStr);

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
                ++skipped;
                LOG_INFO("PrCheckRunWatcher::Tick: Skip on prUrl=%s checkRunId=%lld (reason=%s)", row.prUrl.c_str(),
                         static_cast<long long>(run.id), cls.skipReason.c_str());
                continue;
            }

            if (cls.verdict == CheckRunVerdict::RerunTransient) {
                // Apply the per-PR rerun cap. The classifier returns the
                // verdict unconditionally; the cap is a watcher concern
                // so it stays a single source of truth.
                //
                // (CodeRabbit PR #303 review) Read + write happen under
                // separate critical sections, but the post-increment value
                // is captured under the second lock so the LOG_INFO below
                // reports the actual stored count (not a stale
                // `currentReruns + 1` that two concurrent Ticks could
                // both compute against the same `currentReruns`).
                int currentReruns = 0;
                {
                    std::lock_guard<std::mutex> lock(rerunCountMu_);
                    auto it = rerunCountByPrUrl_.find(row.prUrl);
                    currentReruns = (it != rerunCountByPrUrl_.end()) ? it->second : 0;
                }
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
                int newReruns = 0;
                {
                    std::lock_guard<std::mutex> lock(rerunCountMu_);
                    newReruns = ++rerunCountByPrUrl_[row.prUrl];
                }
                ++actioned;
                ++reran;
                LOG_INFO("transient re-run pr=%s workflow_run=%lld attempt=%d/%d", row.prUrl.c_str(),
                         static_cast<long long>(cls.rerunWorkflowId), newReruns, rerunCap);
                LOG_INFO("PrCheckRunWatcher::Tick: RerunTransient fired for prUrl=%s runId=%lld (rerun %d of %d)",
                         row.prUrl.c_str(), static_cast<long long>(cls.rerunWorkflowId), newReruns, rerunCap);
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
            ++dispatched;
            LOG_INFO("dispatch %s pr=%s check=%s iter=%d", cls.dispatchTargetAgent.c_str(), row.prUrl.c_str(),
                     run.name.c_str(), newCount);
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
    LOG_INFO("ci watcher tick end dispatched=%d reran=%d skipped=%d actioned=%d", dispatched, reran, skipped, actioned);
    return actioned;
}

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
