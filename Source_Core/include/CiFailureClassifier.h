// CiFailureClassifier.h — phase-6 of the `coderabbit-react-loop` plan
// (docs/design/coderabbit-react-loop.md § Phased rollout § Phase 6). Concrete
// `PrCheckRunClassifier` impl that wires the phase-5 pure helpers
// (`CiFailureClassifierPure.{h,cpp}`) to the verdict logic:
//
//   - Category CmakeOrCtest + fingerprint CmakeConfigError / LinkError
//                                              → Dispatch (build-doctor,
//                                                ci_build_failure)
//   - Category CmakeOrCtest + fingerprint CtestFailure / SanitizerHit
//                                              → Dispatch (debug-detective,
//                                                ci_ctest_failure) — gated on
//                                                `autoDispatchDebugDetective_`;
//                                                disabled → Skip.
//   - Category CmakeOrCtest + fingerprint TransientFlake
//                                              → RerunTransient with
//                                                `rerunWorkflowId` resolved
//                                                from the check-run's
//                                                `details_url`. The PrCheckRunWatcher
//                                                applies the per-PR rerun cap
//                                                (`cfg.ci_react.transient_rerun_max_per_pr`)
//                                                — the classifier returns the
//                                                verdict unconditionally so the
//                                                cap policy stays a single
//                                                source of truth.
//   - Category CoverageGate                    → Dispatch (test-rig,
//                                                ci_coverage_gate) — gated on
//                                                `autoDispatchTestRig_`.
//   - Category CoverageAdvisory                → Skip (advisory until 2026-05-30).
//   - Category Unknown                         → Skip (add to phase-5 name table).
//   - `conclusion != "failure"`                → Skip (the watcher should only
//                                                feed failures anyway).
//
// Build-time gating: source-list-conditional on SMATCHET_WITH_AGENTIC.
// AGENTIC=OFF builds never compile this TU.

#ifndef SMATCHET_CI_FAILURE_CLASSIFIER_H
#define SMATCHET_CI_FAILURE_CLASSIFIER_H

#if defined(SMATCHET_WITH_AGENTIC)

#include "PrCheckRunClassifier.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace agentic {

class CiFailureClassifier : public PrCheckRunClassifier {
  public:
    /// Defaults match the plan's `cfg.ci_react.*` block (phase 8 lands the
    /// actual config plumbing; phase 6 hardcodes sensible defaults inline):
    ///   - autoDispatchDebugDetective_ = true
    ///   - autoDispatchTestRig_        = true
    CiFailureClassifier();
    ~CiFailureClassifier() override;

    CiFailureClassifier(const CiFailureClassifier&) = delete;
    CiFailureClassifier& operator=(const CiFailureClassifier&) = delete;

    /// Knobs — copied from `cfg.ci_react.*` at AppController-init time and
    /// re-applied whenever Preferences fires a config-change event (phase 8).
    void SetAutoDispatchDebugDetective(bool enabled);
    bool GetAutoDispatchDebugDetective() const;
    void SetAutoDispatchTestRig(bool enabled);
    bool GetAutoDispatchTestRig() const;

    /// Classify a failed check-run. See PrCheckRunClassifier.h for verdict
    /// semantics. Stateless — the per-PR rerun cap lives on the watcher
    /// (which owns the in-memory counter keyed by `pr_url`).
    CheckRunClassification Classify(const GitHubClient::CheckRun& run,
                                    const std::vector<GitHubClient::CheckRunAnnotation>& annotations,
                                    const std::string& logTail) const override;

    /// Resolve the `runId` (used by `RerunWorkflowRun`) from a check-run's
    /// `details_url`. GitHub's check-run URL shape carries the run-id as a
    /// path segment:
    ///   `https://github.com/<owner>/<repo>/actions/runs/<runId>/job/<jobId>`
    /// Returns 0 + populates outError on malformed input. Exposed for tests
    /// + for the watcher's RerunTransient path.
    static std::int64_t ParseRunIdFromDetailsUrl(const std::string& detailsUrl, std::string& outError);

  private:
    // Atomic so reads from `Classify()` (called on the poll worker) race-free
    // with `Set*` writes from the Preferences UI thread. CodeRabbit #296
    // finding 1 — `Source_Core/include/CiFailureClassifier.h:62-67`.
    std::atomic<bool> autoDispatchDebugDetective_{true};
    std::atomic<bool> autoDispatchTestRig_{true};
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
#endif // SMATCHET_CI_FAILURE_CLASSIFIER_H
