// PrCheckRunClassifier.h — phase-5 of the `coderabbit-react-loop` plan
// (docs/design/coderabbit-react-loop.md § Phase 5). Abstract interface for
// classifying a failed GitHub check-run into one of three outcomes:
//
//   1. Dispatch       → spawn a coding harness (build-doctor / debug-detective
//                       / test-rig) with a SEED.json.dispatch_source naming
//                       the failure family (ci_build_failure |
//                       ci_ctest_failure | ci_coverage_gate).
//   2. RerunTransient → call `gh workflow run …` to re-fire the workflow when
//                       the failure looks transient (cpr timeout, MSYS2
//                       mirror DNS, FetchContent ZIP retry). No spawn cost.
//   3. Skip           → unknown check-run name, advisory-mode check, or
//                       non-failure conclusion. Logged + skipped.
//
// The concrete `CiFailureClassifier` lands in phase 6 along with
// `PrCheckRunWatcher`; this interface lets the phase-5 doctests inject a
// deterministic fake and lock the dispatch contract before HTTP wiring.

#ifndef SMATCHET_PR_CHECK_RUN_CLASSIFIER_H
#define SMATCHET_PR_CHECK_RUN_CLASSIFIER_H

#if defined(SMATCHET_WITH_AGENTIC)

#include "GitHubClient.h" // for GitHubClient::CheckRun + CheckRunAnnotation

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace agentic {

enum class CheckRunVerdict {
    /// Spawn a coding harness; SEED.json.dispatch_source picks one of
    /// `ci_build_failure | ci_ctest_failure | ci_coverage_gate`.
    Dispatch,
    /// Failure looks transient (cpr network timeout, MSYS2 mirror DNS,
    /// FetchContent ZIP retry). Call `gh workflow run …` to re-fire;
    /// no spawn cost.
    RerunTransient,
    /// Unknown check-run name, advisory check, or non-failure
    /// conclusion. Log + skip.
    Skip,
};

struct CheckRunClassification {
    CheckRunVerdict verdict = CheckRunVerdict::Skip;
    /// For Dispatch: "build-doctor" | "debug-detective" | "test-rig".
    std::string dispatchTargetAgent;
    /// For Dispatch: the SEED.json.dispatch_source enum value:
    /// "ci_build_failure" | "ci_ctest_failure" | "ci_coverage_gate".
    /// Empty for non-Dispatch verdicts.
    std::string dispatchSource;
    /// For RerunTransient: the workflow_id to re-fire. The watcher
    /// resolves the workflow_id from the check-run's `details_url`
    /// — see phase 6 wiring.
    std::int64_t rerunWorkflowId = 0;
    /// For Skip / RerunTransient: short reason for audit row.
    std::string skipReason;
};

/// Abstract — concrete `CiFailureClassifier` lands in phase 6. Tests
/// can inject a deterministic fake against `PrCheckRunWatcher` via
/// a virtual seam.
class PrCheckRunClassifier {
  public:
    virtual ~PrCheckRunClassifier() = default;
    /// Classify a check-run. `annotations` + `logTail` are best-effort
    /// — production fetches them via the H7 GitHubClient methods;
    /// tests inject fakes. Empty vectors / strings are valid.
    virtual CheckRunClassification
    Classify(const GitHubClient::CheckRun& run,
             const std::vector<GitHubClient::CheckRunAnnotation>& annotations,
             const std::string& logTail) const = 0;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
#endif // SMATCHET_PR_CHECK_RUN_CLASSIFIER_H
