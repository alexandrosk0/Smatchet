// CiFailureClassifier.cpp — phase-6 of the `coderabbit-react-loop` plan.
// Concrete `PrCheckRunClassifier` impl. Wires phase-5 pure helpers to the
// verdict logic per the table in `agents/coderabbit-triage.md` (and
// `docs/design/coderabbit-react-loop.md § Phase 6`).

#if defined(SMATCHET_WITH_AGENTIC)

#include "CiFailureClassifier.h"

#include "CiFailureClassifierPure.h"
#include "Logger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace agentic {

namespace {

bool IsAllDigits(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

CiFailureClassifier::CiFailureClassifier() { LOG_TRACE("CiFailureClassifier::ctor enter"); }

CiFailureClassifier::~CiFailureClassifier() = default;

void CiFailureClassifier::SetAutoDispatchDebugDetective(bool enabled) {
    LOG_TRACE("CiFailureClassifier::SetAutoDispatchDebugDetective enter enabled=%d", static_cast<int>(enabled));
    autoDispatchDebugDetective_.store(enabled);
}
bool CiFailureClassifier::GetAutoDispatchDebugDetective() const {
    LOG_TRACE("CiFailureClassifier::GetAutoDispatchDebugDetective enter");
    return autoDispatchDebugDetective_.load();
}
void CiFailureClassifier::SetAutoDispatchTestRig(bool enabled) {
    LOG_TRACE("CiFailureClassifier::SetAutoDispatchTestRig enter enabled=%d", static_cast<int>(enabled));
    autoDispatchTestRig_.store(enabled);
}
bool CiFailureClassifier::GetAutoDispatchTestRig() const {
    LOG_TRACE("CiFailureClassifier::GetAutoDispatchTestRig enter");
    return autoDispatchTestRig_.load();
}

std::int64_t CiFailureClassifier::ParseRunIdFromDetailsUrl(const std::string& detailsUrl, std::string& outError) {
    LOG_TRACE("CiFailureClassifier::ParseRunIdFromDetailsUrl enter detailsUrl=%s", detailsUrl.c_str());
    outError.clear();
    if (detailsUrl.empty()) {
        outError = "details_url is empty";
        return 0;
    }
    // GitHub check-run details URL shape:
    //   https://github.com/<owner>/<repo>/actions/runs/<runId>/job/<jobId>
    // Tolerate query strings / fragments by chopping at the first '?' / '#'
    // past the runId. Enterprise GitHub hosts other than github.com still
    // carry the `/actions/runs/<N>/` path tail.
    const std::string anchor = "/actions/runs/";
    const std::size_t pos = detailsUrl.find(anchor);
    if (pos == std::string::npos) {
        outError = "details_url missing '/actions/runs/' segment";
        return 0;
    }
    std::size_t i = pos + anchor.size();
    std::string digits;
    while (i < detailsUrl.size() && detailsUrl[i] >= '0' && detailsUrl[i] <= '9') {
        digits.push_back(detailsUrl[i]);
        ++i;
    }
    if (digits.empty()) {
        outError = "details_url runId segment is not numeric";
        return 0;
    }
    if (!IsAllDigits(digits)) {
        outError = "details_url runId segment is not numeric";
        return 0;
    }
    // std::stoll throws on overflow; clamp by length first so >19-digit
    // inputs (impossible for a real run-id but a malformed URL could
    // carry them) fail gracefully instead of throwing. INT64_MAX is
    // 9223372036854775807 (19 digits) — accept legitimate 19-digit
    // values up to that bound; reject 20+ digits or 19 digits > max.
    if (digits.size() > 19 || (digits.size() == 19 && digits > "9223372036854775807")) {
        outError = "details_url runId segment overflows int64";
        return 0;
    }
    try {
        return static_cast<std::int64_t>(std::stoll(digits));
    } catch (const std::exception&) {
        outError = "details_url runId parse failure";
        return 0;
    }
}

CheckRunClassification CiFailureClassifier::Classify(const GitHubClient::CheckRun& run,
                                                     const std::vector<GitHubClient::CheckRunAnnotation>& annotations,
                                                     const std::string& logTail) const {
    LOG_TRACE("CiFailureClassifier::Classify enter name=%s id=%lld conclusion=%s annotations=%zu logTailBytes=%zu",
              run.name.c_str(), static_cast<long long>(run.id), run.conclusion.c_str(), annotations.size(),
              logTail.size());
    CheckRunClassification out;
    out.verdict = CheckRunVerdict::Skip;

    // Step 1 — conclusion gate. The watcher only feeds failed runs, but
    // belt-and-braces here means a future refactor that re-uses the
    // classifier for any-status doesn't silently dispatch a passing check.
    if (run.conclusion != "failure") {
        out.skipReason = "conclusion != failure";
        LOG_DEBUG("CiFailureClassifier: skip (conclusion != failure) name=%s conclusion=%s", run.name.c_str(),
                  run.conclusion.c_str());
        return out;
    }

    // Step 2 — category map. Source of truth lives in CiFailureClassifierPure.
    const ci_pure::CheckRunCategory category = ci_pure::MapCheckRunNameToCategory(run.name);
    LOG_DEBUG("CiFailureClassifier: category-map name=%s -> %d", run.name.c_str(), static_cast<int>(category));
    switch (category) {
    case ci_pure::CheckRunCategory::CoverageAdvisory:
        out.verdict = CheckRunVerdict::Skip;
        out.skipReason = "coverage check is advisory until 2026-05-30 cutoff per locked decision";
        LOG_DEBUG("fingerprint match CoverageAdvisory -> verdict=Skip");
        return out;
    case ci_pure::CheckRunCategory::Unknown:
        out.verdict = CheckRunVerdict::Skip;
        out.skipReason = "unknown check-run name; add to phase-5 name table";
        LOG_WARN("unknown check-run skipped name=%s (Self-improvement backlog candidate)", run.name.c_str());
        LOG_WARN("CiFailureClassifier: unknown check-run name '%s' (id=%lld) — skipping. Add to "
                 "CiFailureClassifierPure.cpp::kCheckRunNameTable to actionable.",
                 run.name.c_str(), static_cast<long long>(run.id));
        return out;
    case ci_pure::CheckRunCategory::CoverageGate:
        if (!autoDispatchTestRig_) {
            out.verdict = CheckRunVerdict::Skip;
            out.skipReason = "test-rig auto-dispatch disabled";
            LOG_DEBUG("fingerprint match CoverageGate -> verdict=Skip (auto-dispatch disabled)");
            return out;
        }
        out.verdict = CheckRunVerdict::Dispatch;
        out.dispatchTargetAgent = "test-rig";
        out.dispatchSource = "ci_coverage_gate";
        LOG_DEBUG("fingerprint match CoverageGate -> verdict=Dispatch target=test-rig");
        return out;
    case ci_pure::CheckRunCategory::CmakeOrCtest:
        // Falls through to the fingerprint sub-classification below.
        break;
    }

    // Step 3 — CmakeOrCtest fingerprinting. Concatenate annotation
    // messages so a single buffer holds the searchable content.
    std::vector<std::string> annotMsgs;
    annotMsgs.reserve(annotations.size());
    std::transform(annotations.begin(), annotations.end(), std::back_inserter(annotMsgs),
                   [](const GitHubClient::CheckRunAnnotation& a) { return a.message; });
    const std::string annotConcat = ci_pure::ConcatenateAnnotations(annotMsgs);
    const ci_pure::CmakeOrCtestKind kind = ci_pure::FingerprintCmakeOrCtest(annotConcat, logTail);
    LOG_DEBUG("CiFailureClassifier: fingerprint cmake-or-ctest kind=%d annotConcatBytes=%zu logTailBytes=%zu",
              static_cast<int>(kind), annotConcat.size(), logTail.size());

    switch (kind) {
    case ci_pure::CmakeOrCtestKind::TransientFlake: {
        LOG_DEBUG("fingerprint match TransientFlake -> verdict=RerunTransient");
        std::string parseErr;
        const std::int64_t runId = ParseRunIdFromDetailsUrl(run.detailsUrl, parseErr);
        if (runId == 0) {
            // Can't fire a rerun without the runId. Degrade to Skip so the
            // watcher logs but doesn't spawn — the user re-runs manually.
            LOG_WARN("CiFailureClassifier: transient flake detected on check-run id=%lld but details_url "
                     "parse failed (%s) — skipping",
                     static_cast<long long>(run.id), parseErr.c_str());
            out.verdict = CheckRunVerdict::Skip;
            out.skipReason = "transient flake but details_url unparseable";
            return out;
        }
        out.verdict = CheckRunVerdict::RerunTransient;
        out.rerunWorkflowId = runId;
        return out;
    }
    case ci_pure::CmakeOrCtestKind::CtestFailure:
    case ci_pure::CmakeOrCtestKind::SanitizerHit:
        LOG_DEBUG("fingerprint match %s -> verdict=Dispatch target=debug-detective",
                  kind == ci_pure::CmakeOrCtestKind::CtestFailure ? "CtestFailure" : "SanitizerHit");
        if (!autoDispatchDebugDetective_) {
            out.verdict = CheckRunVerdict::Skip;
            out.skipReason = "debug-detective auto-dispatch disabled";
            LOG_DEBUG("CiFailureClassifier: debug-detective auto-dispatch disabled -> verdict=Skip");
            return out;
        }
        out.verdict = CheckRunVerdict::Dispatch;
        out.dispatchTargetAgent = "debug-detective";
        out.dispatchSource = "ci_ctest_failure";
        return out;
    case ci_pure::CmakeOrCtestKind::CmakeConfigError:
    case ci_pure::CmakeOrCtestKind::LinkError:
        LOG_DEBUG("fingerprint match %s -> verdict=Dispatch target=build-doctor",
                  kind == ci_pure::CmakeOrCtestKind::CmakeConfigError ? "CmakeConfigError" : "LinkError");
        out.verdict = CheckRunVerdict::Dispatch;
        out.dispatchTargetAgent = "build-doctor";
        out.dispatchSource = "ci_build_failure";
        return out;
    case ci_pure::CmakeOrCtestKind::Unknown:
        // No fingerprint matched against the annotations + log tail.
        // Default to build-doctor (most common cause of an unfingerprinted
        // CmakeOrCtest failure is a build-system regression where the
        // failure line landed outside our needles).
        LOG_DEBUG("fingerprint match Unknown -> verdict=Dispatch target=build-doctor (default fallback)");
        out.verdict = CheckRunVerdict::Dispatch;
        out.dispatchTargetAgent = "build-doctor";
        out.dispatchSource = "ci_build_failure";
        return out;
    }
    // Unreachable — all enumerators handled above.
    out.verdict = CheckRunVerdict::Skip;
    out.skipReason = "unreachable fingerprint switch fallthrough";
    LOG_WARN("CiFailureClassifier: unreachable switch fallthrough name=%s", run.name.c_str());
    return out;
}

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
