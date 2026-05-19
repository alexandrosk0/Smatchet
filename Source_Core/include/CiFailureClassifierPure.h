// CiFailureClassifierPure.h — phase-5 of the `coderabbit-react-loop` plan
// (docs/design/coderabbit-react-loop.md § Phase 5). Pure C++14 helpers used
// by the phase-6 concrete `CiFailureClassifier` and exercised directly by
// the doctest rig. No third-party / banned-dep includes (no cpr / SQLite /
// ImGui / httplib / sol2). Built into both `SmatchetStandalone` and
// `SmatchetCore_DX12` and into the doctest rig.
//
// Per AGENTS.md § "Pure-helper TU-split recipe" — the matchers live in a
// dependency-light TU so future agentic-flow consumers (the phase-6 concrete
// classifier, the spawned-harness `build-doctor` / `debug-detective` /
// `test-rig` modes) can call them without dragging in the GitHub HTTP stack.
//
// Name-table source of truth: `.github/workflows/build-and-test.yml`,
// `.github/workflows/coverage-gate.yml`, `.github/workflows/coverage.yml`.
// Names are encoded by case-sensitive exact match — workflow `name:` fields
// are user-authored and stable. New variants land in `Unknown` and the
// watcher LOG_WARNs.

#ifndef SMATCHET_CI_FAILURE_CLASSIFIER_PURE_H
#define SMATCHET_CI_FAILURE_CLASSIFIER_PURE_H

#include <cstddef>
#include <string>
#include <vector>

namespace smatchet {
namespace agentic {
namespace ci_pure {

enum class CheckRunCategory {
    Unknown,          // Not in the known name table; skip + log
    CmakeOrCtest,     // `Windows + MSYS2 UCRT64` + AGENTIC=OFF / WHISPER=OFF
                      // variants. Sub-classification (cmake-error vs
                      // ctest-fail vs sanitizer-hit vs transient) happens
                      // via the fingerprint helpers below.
    CoverageGate,     // `Test-delta gate`
    CoverageAdvisory, // `Coverage (windows-2022 + OpenCppCoverage)` —
                      // advisory until 2026-05-30 per coverage.yml.
                      // Phase 5 hardcodes this as advisory; user flips
                      // the config manually post-cutoff per the
                      // 2026-05-18 locked decision.
};

enum class CmakeOrCtestKind {
    Unknown,         // No fingerprint matched; treat as cmake by default
                     // so the orchestrator routes to build-doctor.
    CmakeConfigError,
    LinkError,
    CtestFailure,
    SanitizerHit,
    TransientFlake, // cpr timeout / MSYS2 mirror / FetchContent retry
};

/// Map check-run name to category. Reads from a hardcoded table built
/// from the workflow YAML files at phase 5 commit time. New workflows
/// land in `Unknown` and the watcher LOG_WARNs.
CheckRunCategory MapCheckRunNameToCategory(const std::string& checkRunName);

/// Inspect the annotation messages + log tail for fingerprints. First
/// match wins, with Transient checked first so a flake masquerading as
/// a build error isn't re-classified as a real failure. Conservative —
/// false-positive (treat real fail as transient) defaults are tuned to
/// err toward "real failure" so the user is never left with a silently-
/// ignored bug.
CmakeOrCtestKind FingerprintCmakeOrCtest(const std::string& annotationsConcatenated,
                                         const std::string& logTail);

/// Pure helpers for individual fingerprint families — exposed for
/// fine-grained doctest coverage. Each returns true when the fingerprint
/// matches.
bool MatchesCmakeConfigError(const std::string& text);
bool MatchesLinkError(const std::string& text);
bool MatchesCtestFailure(const std::string& text);
bool MatchesSanitizerHit(const std::string& text);
bool MatchesTransientFlake(const std::string& text);

/// Concatenate annotation messages with newlines for fingerprint
/// scanning. Caps the result at `maxLen` to keep fingerprint cost
/// bounded (long-form annotations occasionally inline huge stack
/// dumps); truncation drops the **leading** bytes so the tail —
/// where the actual error usually sits — survives the cap.
std::string ConcatenateAnnotations(const std::vector<std::string>& annotationMessages,
                                   std::size_t maxLen = 64 * 1024);

} // namespace ci_pure
} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_CI_FAILURE_CLASSIFIER_PURE_H
