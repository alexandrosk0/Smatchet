// CiFailureClassifier.test.cpp — phase-6 of the `coderabbit-react-loop`
// plan. Exercises the concrete `CiFailureClassifier` against the phase-5
// fixtures (`check_runs_failed_sample.json` +
// `check_run_annotations_sample.json`) and against synthetic inputs for the
// non-fixture verdict branches.

#if defined(SMATCHET_WITH_AGENTIC)

#include "CiFailureClassifier.h"

#include "GitHubClient.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef SMATCHET_TESTS_REPO_ROOT
#error "SMATCHET_TESTS_REPO_ROOT must be defined to locate tests/fixtures/*"
#endif

using ::smatchet::agentic::CheckRunClassification;
using ::smatchet::agentic::CheckRunVerdict;
using ::smatchet::agentic::CiFailureClassifier;

namespace {

const std::string kFixtureDir = std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures";

nlohmann::json LoadJson(const std::string& relPath) {
    const std::string path = kFixtureDir + "/" + relPath;
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.is_open(), ("Fixture file unreadable: " + path).c_str());
    std::stringstream ss;
    ss << in.rdbuf();
    return nlohmann::json::parse(ss.str());
}

GitHubClient::CheckRun BuildCheckRun(const nlohmann::json& j) {
    GitHubClient::CheckRun r;
    r.id = j.at("id").get<std::int64_t>();
    r.name = j.at("name").get<std::string>();
    r.status = j.at("status").get<std::string>();
    r.conclusion = j.at("conclusion").get<std::string>();
    r.detailsUrl = j.at("details_url").get<std::string>();
    return r;
}

std::vector<GitHubClient::CheckRunAnnotation> BuildAnnotations(const nlohmann::json& msgs) {
    std::vector<GitHubClient::CheckRunAnnotation> out;
    if (!msgs.is_array()) {
        return out;
    }
    out.reserve(msgs.size());
    for (const auto& m : msgs) {
        GitHubClient::CheckRunAnnotation a;
        a.message = m.get<std::string>();
        a.annotationLevel = "failure";
        out.push_back(std::move(a));
    }
    return out;
}

} // namespace

TEST_SUITE("CiFailureClassifier") {

    // ─── Fixture-driven verdicts for each row in the phase-5 fixtures ────

    TEST_CASE("fixture row 100001 (CMake config error) → Dispatch build-doctor / ci_build_failure") {
        const auto runs = LoadJson("check_runs_failed_sample.json");
        const auto ann = LoadJson("check_run_annotations_sample.json");
        const auto run = BuildCheckRun(runs[0]);
        const auto annots = BuildAnnotations(ann["100001"]["annotations"]);
        const std::string logTail = ann["100001"]["log_tail"].get<std::string>();
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(run, annots, logTail);
        CHECK(v.verdict == CheckRunVerdict::Dispatch);
        CHECK(v.dispatchTargetAgent == "build-doctor");
        CHECK(v.dispatchSource == "ci_build_failure");
    }

    TEST_CASE("fixture row 100002 (link error) → Dispatch build-doctor / ci_build_failure") {
        const auto runs = LoadJson("check_runs_failed_sample.json");
        const auto ann = LoadJson("check_run_annotations_sample.json");
        const auto run = BuildCheckRun(runs[1]);
        const auto annots = BuildAnnotations(ann["100002"]["annotations"]);
        const std::string logTail = ann["100002"]["log_tail"].get<std::string>();
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(run, annots, logTail);
        CHECK(v.verdict == CheckRunVerdict::Dispatch);
        CHECK(v.dispatchTargetAgent == "build-doctor");
        CHECK(v.dispatchSource == "ci_build_failure");
    }

    TEST_CASE("fixture row 100003 (ctest failure) → Dispatch debug-detective / ci_ctest_failure") {
        const auto runs = LoadJson("check_runs_failed_sample.json");
        const auto ann = LoadJson("check_run_annotations_sample.json");
        const auto run = BuildCheckRun(runs[2]);
        const auto annots = BuildAnnotations(ann["100003"]["annotations"]);
        const std::string logTail = ann["100003"]["log_tail"].get<std::string>();
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(run, annots, logTail);
        CHECK(v.verdict == CheckRunVerdict::Dispatch);
        CHECK(v.dispatchTargetAgent == "debug-detective");
        CHECK(v.dispatchSource == "ci_ctest_failure");
    }

    TEST_CASE("fixture row 100004 (sanitizer hit) → Dispatch debug-detective / ci_ctest_failure") {
        const auto runs = LoadJson("check_runs_failed_sample.json");
        const auto ann = LoadJson("check_run_annotations_sample.json");
        const auto run = BuildCheckRun(runs[3]);
        const auto annots = BuildAnnotations(ann["100004"]["annotations"]);
        const std::string logTail = ann["100004"]["log_tail"].get<std::string>();
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(run, annots, logTail);
        CHECK(v.verdict == CheckRunVerdict::Dispatch);
        CHECK(v.dispatchTargetAgent == "debug-detective");
        CHECK(v.dispatchSource == "ci_ctest_failure");
    }

    TEST_CASE("fixture row 100005 (transient flake) → RerunTransient with parsed runId") {
        const auto runs = LoadJson("check_runs_failed_sample.json");
        const auto ann = LoadJson("check_run_annotations_sample.json");
        const auto run = BuildCheckRun(runs[4]);
        const auto annots = BuildAnnotations(ann["100005"]["annotations"]);
        const std::string logTail = ann["100005"]["log_tail"].get<std::string>();
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(run, annots, logTail);
        CHECK(v.verdict == CheckRunVerdict::RerunTransient);
        // The fixture's details_url is …/actions/runs/100005/job/…
        CHECK(v.rerunWorkflowId == 100005);
    }

    TEST_CASE("fixture row 100006 (coverage gate) → Dispatch test-rig / ci_coverage_gate") {
        const auto runs = LoadJson("check_runs_failed_sample.json");
        const auto ann = LoadJson("check_run_annotations_sample.json");
        const auto run = BuildCheckRun(runs[5]);
        const auto annots = BuildAnnotations(ann["100006"]["annotations"]);
        const std::string logTail = ann["100006"]["log_tail"].get<std::string>();
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(run, annots, logTail);
        CHECK(v.verdict == CheckRunVerdict::Dispatch);
        CHECK(v.dispatchTargetAgent == "test-rig");
        CHECK(v.dispatchSource == "ci_coverage_gate");
    }

    TEST_CASE("fixture row 100007 (coverage advisory) → Skip (advisory until 2026-05-30)") {
        const auto runs = LoadJson("check_runs_failed_sample.json");
        const auto ann = LoadJson("check_run_annotations_sample.json");
        const auto run = BuildCheckRun(runs[6]);
        const auto annots = BuildAnnotations(ann["100007"]["annotations"]);
        const std::string logTail = ann["100007"]["log_tail"].get<std::string>();
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(run, annots, logTail);
        CHECK(v.verdict == CheckRunVerdict::Skip);
        CHECK(v.skipReason.find("advisory") != std::string::npos);
    }

    TEST_CASE("fixture row 100008 (unknown name) → Skip (add to name table)") {
        const auto runs = LoadJson("check_runs_failed_sample.json");
        const auto ann = LoadJson("check_run_annotations_sample.json");
        const auto run = BuildCheckRun(runs[7]);
        const auto annots = BuildAnnotations(ann["100008"]["annotations"]);
        const std::string logTail = ann["100008"]["log_tail"].get<std::string>();
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(run, annots, logTail);
        CHECK(v.verdict == CheckRunVerdict::Skip);
        CHECK(v.skipReason.find("unknown") != std::string::npos);
    }

    // ─── Non-failure conclusion (defensive belt-and-braces) ──────────────

    TEST_CASE("conclusion='success' → Skip regardless of name") {
        GitHubClient::CheckRun r;
        r.id = 1;
        r.name = "Windows + MSYS2 UCRT64";
        r.status = "completed";
        r.conclusion = "success";
        r.detailsUrl = "https://github.com/o/r/actions/runs/123/job/9";
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(r, {}, "");
        CHECK(v.verdict == CheckRunVerdict::Skip);
        CHECK(v.skipReason == "conclusion != failure");
    }

    TEST_CASE("conclusion='neutral' → Skip") {
        GitHubClient::CheckRun r;
        r.conclusion = "neutral";
        r.name = "Test-delta gate";
        CiFailureClassifier c;
        CHECK(c.Classify(r, {}, "").verdict == CheckRunVerdict::Skip);
    }

    // ─── Auto-dispatch flag gating ───────────────────────────────────────

    TEST_CASE("auto_dispatch_debug_detective = false → Skip on ctest failure") {
        GitHubClient::CheckRun r;
        r.id = 9001;
        r.name = "Windows + MSYS2 UCRT64";
        r.conclusion = "failure";
        r.detailsUrl = "https://github.com/o/r/actions/runs/9001/job/1";
        std::vector<GitHubClient::CheckRunAnnotation> annots;
        GitHubClient::CheckRunAnnotation a;
        a.message = "The following tests FAILED:\n   7 - smatchet_tests (Failed)";
        annots.push_back(a);
        CiFailureClassifier c;
        c.SetAutoDispatchDebugDetective(false);
        const CheckRunClassification v = c.Classify(r, annots, "");
        CHECK(v.verdict == CheckRunVerdict::Skip);
        CHECK(v.skipReason.find("debug-detective") != std::string::npos);
    }

    TEST_CASE("auto_dispatch_debug_detective = false → Skip on sanitizer hit") {
        GitHubClient::CheckRun r;
        r.id = 9002;
        r.name = "Windows + MSYS2 UCRT64";
        r.conclusion = "failure";
        r.detailsUrl = "https://github.com/o/r/actions/runs/9002/job/2";
        std::vector<GitHubClient::CheckRunAnnotation> annots;
        GitHubClient::CheckRunAnnotation a;
        a.message = "==12345==ERROR: AddressSanitizer: heap-use-after-free";
        annots.push_back(a);
        CiFailureClassifier c;
        c.SetAutoDispatchDebugDetective(false);
        CHECK(c.Classify(r, annots, "").verdict == CheckRunVerdict::Skip);
    }

    TEST_CASE("auto_dispatch_test_rig = false → Skip on coverage gate") {
        GitHubClient::CheckRun r;
        r.id = 9003;
        r.name = "Test-delta gate";
        r.conclusion = "failure";
        r.detailsUrl = "https://github.com/o/r/actions/runs/9003/job/3";
        CiFailureClassifier c;
        c.SetAutoDispatchTestRig(false);
        const CheckRunClassification v = c.Classify(r, {}, "");
        CHECK(v.verdict == CheckRunVerdict::Skip);
        CHECK(v.skipReason.find("test-rig") != std::string::npos);
    }

    TEST_CASE("auto_dispatch flags do NOT gate build-doctor (build failures always dispatch)") {
        GitHubClient::CheckRun r;
        r.id = 9004;
        r.name = "Windows + MSYS2 UCRT64";
        r.conclusion = "failure";
        r.detailsUrl = "https://github.com/o/r/actions/runs/9004/job/4";
        std::vector<GitHubClient::CheckRunAnnotation> annots;
        GitHubClient::CheckRunAnnotation a;
        a.message = "CMake Error at CMakeLists.txt:142";
        annots.push_back(a);
        CiFailureClassifier c;
        c.SetAutoDispatchDebugDetective(false);
        c.SetAutoDispatchTestRig(false);
        const CheckRunClassification v = c.Classify(r, annots, "");
        CHECK(v.verdict == CheckRunVerdict::Dispatch);
        CHECK(v.dispatchTargetAgent == "build-doctor");
    }

    // ─── Unfingerprinted CmakeOrCtest defaults to build-doctor ───────────

    TEST_CASE("CmakeOrCtest with no fingerprint match → defaults to build-doctor") {
        GitHubClient::CheckRun r;
        r.id = 9005;
        r.name = "Windows + MSYS2 UCRT64";
        r.conclusion = "failure";
        r.detailsUrl = "https://github.com/o/r/actions/runs/9005/job/5";
        // Annotations + log tail with no fingerprint hits.
        std::vector<GitHubClient::CheckRunAnnotation> annots;
        GitHubClient::CheckRunAnnotation a;
        a.message = "ninja: build stopped: subcommand failed.";
        annots.push_back(a);
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(r, annots, "");
        CHECK(v.verdict == CheckRunVerdict::Dispatch);
        CHECK(v.dispatchTargetAgent == "build-doctor");
        CHECK(v.dispatchSource == "ci_build_failure");
    }

    // ─── ParseRunIdFromDetailsUrl static helper ──────────────────────────

    TEST_CASE("ParseRunIdFromDetailsUrl: typical github URL") {
        std::string err;
        const std::int64_t id = CiFailureClassifier::ParseRunIdFromDetailsUrl(
            "https://github.com/owner/repo/actions/runs/4242/job/77", err);
        CHECK(id == 4242);
        CHECK(err.empty());
    }

    TEST_CASE("ParseRunIdFromDetailsUrl: with query string + fragment") {
        std::string err;
        const std::int64_t id = CiFailureClassifier::ParseRunIdFromDetailsUrl(
            "https://github.com/o/r/actions/runs/9999/job/1?tab=logs#step:3", err);
        CHECK(id == 9999);
    }

    TEST_CASE("ParseRunIdFromDetailsUrl: missing /actions/runs/ segment → 0 + error") {
        std::string err;
        const std::int64_t id =
            CiFailureClassifier::ParseRunIdFromDetailsUrl("https://github.com/o/r/pull/1", err);
        CHECK(id == 0);
        CHECK_FALSE(err.empty());
    }

    TEST_CASE("ParseRunIdFromDetailsUrl: non-numeric segment → 0 + error") {
        std::string err;
        const std::int64_t id =
            CiFailureClassifier::ParseRunIdFromDetailsUrl("https://github.com/o/r/actions/runs/abc/job/1", err);
        CHECK(id == 0);
        CHECK_FALSE(err.empty());
    }

    TEST_CASE("ParseRunIdFromDetailsUrl: empty input → 0 + error") {
        std::string err;
        const std::int64_t id = CiFailureClassifier::ParseRunIdFromDetailsUrl("", err);
        CHECK(id == 0);
        CHECK_FALSE(err.empty());
    }

    TEST_CASE("transient flake but malformed details_url → Skip (cannot fire rerun)") {
        GitHubClient::CheckRun r;
        r.id = 9006;
        r.name = "Windows + MSYS2 UCRT64";
        r.conclusion = "failure";
        // No /actions/runs/ segment — parse will fail.
        r.detailsUrl = "https://example.com/some-other-shape";
        std::vector<GitHubClient::CheckRunAnnotation> annots;
        GitHubClient::CheckRunAnnotation a;
        a.message = "curl: (28) Operation timed out";
        annots.push_back(a);
        CiFailureClassifier c;
        const CheckRunClassification v = c.Classify(r, annots, "");
        CHECK(v.verdict == CheckRunVerdict::Skip);
        CHECK(v.skipReason.find("details_url") != std::string::npos);
    }

    // ─── Default-knob accessors ──────────────────────────────────────────

    TEST_CASE("default knob values match phase-6 plan defaults") {
        CiFailureClassifier c;
        CHECK(c.GetAutoDispatchDebugDetective() == true);
        CHECK(c.GetAutoDispatchTestRig() == true);
    }

    TEST_CASE("knob setters round-trip") {
        CiFailureClassifier c;
        c.SetAutoDispatchDebugDetective(false);
        c.SetAutoDispatchTestRig(false);
        CHECK(c.GetAutoDispatchDebugDetective() == false);
        CHECK(c.GetAutoDispatchTestRig() == false);
        c.SetAutoDispatchDebugDetective(true);
        c.SetAutoDispatchTestRig(true);
        CHECK(c.GetAutoDispatchDebugDetective() == true);
        CHECK(c.GetAutoDispatchTestRig() == true);
    }
}

#endif // SMATCHET_WITH_AGENTIC
