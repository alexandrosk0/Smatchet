// GitHubClient_PrSurface.test.cpp — H7 wire-format coverage for the 6 new
// `GitHubClient` methods. The HTTP layer itself is not under test (cpr is
// already exercised by Bundle C's recorded-fixture pattern); this rig
// covers:
//   - URL-suffix builders (GitHubClientHelpers extensions)
//   - JSON body builders (CreatePullRequest payload, log tail clipper)
//   - Response-parse helpers (extracts html_url, check-runs array, etc.)
//   - Round-trip via recorded fixtures (parse the same JSON bytes the live
//     GitHub API emits so a wire-format drift fails the doctest before it
//     ships).

#include "GitHubClientHelpers.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef SMATCHET_TESTS_REPO_ROOT
#error "SMATCHET_TESTS_REPO_ROOT must be defined to locate tests/fixtures/*"
#endif

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.is_open());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const std::string kFixDir = std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures";

} // namespace

TEST_SUITE("GitHubClient_PrSurface") {

TEST_CASE("URL builders match GitHub REST docs verbatim") {
    CHECK(GitHubClientHelpers::BuildPullsCollectionSuffix("smatchet", "example") == "/repos/smatchet/example/pulls");

    CHECK(GitHubClientHelpers::BuildCheckRunsForCommitSuffix("smatchet", "example",
                                                              "0123456789abcdef0123456789abcdef01234567") ==
          "/repos/smatchet/example/commits/0123456789abcdef0123456789abcdef01234567/check-runs");

    CHECK(GitHubClientHelpers::BuildCheckRunAnnotationsSuffix("smatchet", "example", 88123457) ==
          "/repos/smatchet/example/check-runs/88123457/annotations");

    CHECK(GitHubClientHelpers::BuildActionsJobLogsSuffix("smatchet", "example", 42) ==
          "/repos/smatchet/example/actions/jobs/42/logs");

    CHECK(GitHubClientHelpers::BuildActionsRunRerunSuffix("smatchet", "example", 1024) ==
          "/repos/smatchet/example/actions/runs/1024/rerun");
}

TEST_CASE("BuildCreatePullRequestBody produces the GitHub-expected shape") {
    SUBCASE("draft + non-empty body emits every field") {
        const auto j = GitHubClientHelpers::BuildCreatePullRequestBody("feat: H7", "agent/42/h7", "develop",
                                                                       "PR body", true);
        CHECK(j["title"] == "feat: H7");
        CHECK(j["head"] == "agent/42/h7");
        CHECK(j["base"] == "develop");
        CHECK(j["body"] == "PR body");
        CHECK(j["draft"] == true);
    }
    SUBCASE("empty body omits the key to keep wire payload minimal") {
        const auto j = GitHubClientHelpers::BuildCreatePullRequestBody("feat: H7", "agent/42/h7", "develop", "", true);
        CHECK_FALSE(j.contains("body"));
        CHECK(j["draft"] == true);
    }
    SUBCASE("draft=false flows through") {
        const auto j = GitHubClientHelpers::BuildCreatePullRequestBody("t", "a", "b", "", false);
        CHECK(j["draft"] == false);
    }
}

TEST_CASE("ExtractCreatePullRequestHtmlUrl: happy + error shapes") {
    SUBCASE("happy path — fixture round-trip") {
        const std::string body = ReadFile(kFixDir + "/github_pr_create_response.json");
        std::string url;
        std::string err;
        REQUIRE(GitHubClientHelpers::ExtractCreatePullRequestHtmlUrl(body, url, err));
        CHECK(err.empty());
        CHECK(url == "https://github.com/smatchet/example/pull/512");
    }
    SUBCASE("malformed JSON") {
        std::string url;
        std::string err;
        CHECK_FALSE(GitHubClientHelpers::ExtractCreatePullRequestHtmlUrl("{not json", url, err));
        CHECK(!err.empty());
    }
    SUBCASE("missing html_url") {
        std::string url;
        std::string err;
        CHECK_FALSE(GitHubClientHelpers::ExtractCreatePullRequestHtmlUrl("{\"number\": 1}", url, err));
        CHECK(err.find("html_url") != std::string::npos);
    }
    SUBCASE("html_url present but not a string") {
        std::string url;
        std::string err;
        CHECK_FALSE(GitHubClientHelpers::ExtractCreatePullRequestHtmlUrl("{\"html_url\": 42}", url, err));
    }
    SUBCASE("empty body") {
        std::string url;
        std::string err;
        CHECK_FALSE(GitHubClientHelpers::ExtractCreatePullRequestHtmlUrl("", url, err));
    }
}

TEST_CASE("ClipLogTail clips by line, not byte") {
    const std::string body = "line1\nline2\nline3\nline4\nline5\n";
    SUBCASE("tail=0 returns verbatim") {
        CHECK(GitHubClientHelpers::ClipLogTail(body, 0) == body);
    }
    SUBCASE("tail=2 returns last 2 lines") {
        const auto tail = GitHubClientHelpers::ClipLogTail(body, 2);
        CHECK(tail == "line4\nline5\n");
    }
    SUBCASE("tail >= total returns verbatim") {
        CHECK(GitHubClientHelpers::ClipLogTail(body, 999) == body);
    }
    SUBCASE("body with no trailing newline") {
        // No trailing '\n' on the very last line — the algorithm counts
        // newline boundaries, so the "line3" tail is returned starting
        // from after the final '\n' (i.e. the last unterminated chunk).
        const std::string b = "a\nb\nc";
        CHECK(GitHubClientHelpers::ClipLogTail(b, 1) == "c");
    }
    SUBCASE("empty body") {
        CHECK(GitHubClientHelpers::ClipLogTail("", 5).empty());
    }
}

TEST_CASE("check-runs fixture parses into the expected shape") {
    const std::string body = ReadFile(kFixDir + "/github_check_runs_sample.json");
    const auto j = nlohmann::json::parse(body);
    REQUIRE(j.is_object());
    REQUIRE(j.contains("check_runs"));
    REQUIRE(j["check_runs"].is_array());
    REQUIRE(j["check_runs"].size() == 3);
    // Spot-check the three runs the GitHubClient::FetchCheckRuns parser
    // touches; the live method is exercised by integration / scenario,
    // this rig pins the wire shape.
    const auto& run0 = j["check_runs"][0];
    CHECK(run0["id"].get<std::int64_t>() == 88123456);
    CHECK(run0["name"].get<std::string>() == "build");
    CHECK(run0["status"].get<std::string>() == "completed");
    CHECK(run0["conclusion"].get<std::string>() == "success");

    const auto& run1 = j["check_runs"][1];
    CHECK(run1["conclusion"].get<std::string>() == "failure");

    const auto& run2 = j["check_runs"][2];
    CHECK(run2["status"].get<std::string>() == "in_progress");
    // Wire-shape contract: GitHub returns `conclusion: null` while a run
    // is still in progress. The parser must tolerate this — checking via
    // is_string() is what GitHubClient.cpp uses.
    CHECK(run2["conclusion"].is_null());
}

TEST_CASE("check-run annotations fixture parses into the expected shape") {
    const std::string body = ReadFile(kFixDir + "/github_check_run_annotations_sample.json");
    const auto arr = nlohmann::json::parse(body);
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 3);
    CHECK(arr[0]["annotation_level"].get<std::string>() == "warning");
    CHECK(arr[1]["annotation_level"].get<std::string>() == "failure");
    CHECK(arr[2]["annotation_level"].get<std::string>() == "notice");
    // start_line / end_line are integers; the third row carries 0 because
    // the annotation is file-level (no specific line).
    CHECK(arr[2]["start_line"].get<int>() == 0);
}

TEST_CASE("PR-create response fixture round-trips through the URL extractor") {
    const std::string body = ReadFile(kFixDir + "/github_pr_create_response.json");
    std::string url;
    std::string err;
    REQUIRE(GitHubClientHelpers::ExtractCreatePullRequestHtmlUrl(body, url, err));
    CHECK(url == "https://github.com/smatchet/example/pull/512");
}

TEST_CASE("PR comments fixture parses with same shape as issue-comments") {
    const std::string body = ReadFile(kFixDir + "/github_pr_comments_sample.json");
    const auto arr = nlohmann::json::parse(body);
    REQUIRE(arr.is_array());
    REQUIRE(arr.size() == 3);
    // PR comments carry an additional `pull_request_url` field absent on
    // bare issue comments; the parser ignores it — same code path either
    // way.
    CHECK(arr[0].contains("pull_request_url"));
    CHECK(arr[0]["user"]["login"].get<std::string>() == "alice");
    CHECK(arr[1]["user"]["login"].get<std::string>() == "smatchet-bot");
    // The middle comment is the bot reply — the PrCommentWatcher's
    // bot-filter must skip it on the next tick.
    CHECK(arr[1]["body"].get<std::string>().rfind("<!-- smatchet-handoff -->", 0) == 0);
}

TEST_CASE("rerun-workflow response fixture is GitHub's documented shape") {
    const std::string body = ReadFile(kFixDir + "/github_rerun_workflow_response.json");
    const auto j = nlohmann::json::parse(body);
    REQUIRE(j.is_object());
    CHECK(j["message"].get<std::string>() == "Workflow run rerun queued.");
}

TEST_CASE("actions-job-logs fixture is plain text") {
    const std::string body = ReadFile(kFixDir + "/github_actions_job_logs_sample.txt");
    CHECK(body.find("Build SmatchetStandalone") != std::string::npos);
    // ClipLogTail at 3 lines pulls the last 3 timestamp rows (the file
    // ends in a final '\n' so tail counts work cleanly).
    const auto tail = GitHubClientHelpers::ClipLogTail(body, 3);
    CHECK(tail.find("##[endgroup]") != std::string::npos);
    CHECK(tail.find("Build SmatchetStandalone") == std::string::npos);
}

} // TEST_SUITE

#else  // !SMATCHET_WITH_AGENTIC

// Empty TU when AGENTIC is OFF — keeps the source list happy.

#endif // SMATCHET_WITH_AGENTIC
