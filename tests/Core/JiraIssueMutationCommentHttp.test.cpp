// backend-impl-coverage-recovery (debt.md, 2026-06-07) — HTTP-fixture coverage for the
// JiraClient comment/worklog write shell (JiraIssueMutation.cpp): AddIssueCommentPlain,
// AddWorklog, AddIssueCommentAnnotateContext. These cfg-explicit write paths (no
// ConfigManager::Load) drive a real JiraClient at the in-process httplib loopback
// (JiraCatalogHttpFixture) over real cpr HTTP, exercising the ADF body build, the
// BackendAuditTrail begin/result wiring on both success and failure, and the
// HTTP-status → TrackerError classification. Extends the JiraCatalogHttpFixture pattern to
// the mutation comment/worklog paths flagged in backend-impl-coverage-recovery.

#include "JiraCatalogHttpFixture.h"
#include "Tracker/JiraClient.h"

#include <doctest/doctest.h>

#include <string>

namespace {

const char* const kCommentPath = "/rest/api/3/issue/SMT-1/comment";
const char* const kWorklogPath = "/rest/api/3/issue/SMT-1/worklog";

} // namespace

TEST_CASE("JiraClient::AddIssueCommentPlain — 201 is Ok and posts to the comment endpoint") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kCommentPath, 201, "POST");
    JiraClient client;
    const TrackerError err = client.AddIssueCommentPlain(fx.Config(), "SMT-1", "hello world");
    CHECK(err.IsOk());
    CHECK(fx.RequestCount(kCommentPath) == 1);
}

TEST_CASE("JiraClient::AddIssueCommentPlain — empty issue key is InvalidRequest, no HTTP") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const TrackerError err = client.AddIssueCommentPlain(fx.Config(), "", "text");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
    CHECK(fx.RequestCount(kCommentPath) == 0);
}

TEST_CASE("JiraClient::AddIssueCommentPlain — missing credentials is an auth error") {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira"; // Domain + ApiToken empty.
    JiraClient client;
    const TrackerError err = client.AddIssueCommentPlain(cfg, "SMT-1", "text");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::Auth);
}

TEST_CASE("JiraClient::AddIssueCommentPlain — a 400 classifies as InvalidRequest (non-retryable)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kCommentPath, 400, "POST");
    JiraClient client;
    const TrackerError err = client.AddIssueCommentPlain(fx.Config(), "SMT-1", "text");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
    CHECK_FALSE(err.IsRetryable());
}

TEST_CASE("JiraClient::AddIssueCommentPlain — a 503 classifies as ServerError (retryable)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kCommentPath, 503, "POST");
    JiraClient client;
    const TrackerError err = client.AddIssueCommentPlain(fx.Config(), "SMT-1", "text");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::ServerError);
    CHECK(err.IsRetryable());
}

TEST_CASE("JiraClient::AddWorklog — 201 is Ok and posts to the worklog endpoint") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kWorklogPath, 201, "POST");
    JiraClient client;
    const TrackerError err =
        client.AddWorklog(fx.Config(), "SMT-1", "1h", "", "auto", "did some work", "2026-06-01T09:00:00.000+0000");
    CHECK(err.IsOk());
    CHECK(fx.RequestCount(kWorklogPath) == 1);
}

TEST_CASE("JiraClient::AddWorklog — the 'new' adjust-estimate branch still posts and succeeds") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kWorklogPath, 200, "POST");
    JiraClient client;
    // adjustEstimate=="new" with a non-empty timeRemaining appends &newEstimate to the URL.
    const TrackerError err =
        client.AddWorklog(fx.Config(), "SMT-1", "2h", "3h", "new", "", "2026-06-01T09:00:00.000+0000");
    CHECK(err.IsOk());
    CHECK(fx.RequestCount(kWorklogPath) == 1);
}

TEST_CASE("JiraClient::AddWorklog — empty issue key is InvalidRequest, no HTTP") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const TrackerError err = client.AddWorklog(fx.Config(), "", "1h", "", "", "", "2026-06-01T09:00:00.000+0000");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
    CHECK(fx.RequestCount(kWorklogPath) == 0);
}

TEST_CASE("JiraClient::AddWorklog — a 500 classifies as ServerError (retryable)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kWorklogPath, 500, "POST");
    JiraClient client;
    const TrackerError err = client.AddWorklog(fx.Config(), "SMT-1", "1h", "", "", "", "2026-06-01T09:00:00.000+0000");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::ServerError);
    CHECK(err.IsRetryable());
}

TEST_CASE("JiraClient::AddIssueCommentAnnotateContext — 201 is Ok (ADF codeBlock body built + posted)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kCommentPath, 201, "POST");
    JiraClient client;
    const TrackerError err = client.AddIssueCommentAnnotateContext(fx.Config(), "SMT-1", "p4user", "MyFunction",
                                                                   "src/foo.cpp", 42, "12345", "2026-06-01",
                                                                   /*approximated=*/true, "int foo() { return 0; }");
    CHECK(err.IsOk());
    CHECK(fx.RequestCount(kCommentPath) == 1);
}

TEST_CASE("JiraClient::AddIssueCommentAnnotateContext — empty issue key is InvalidRequest, no HTTP") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const TrackerError err = client.AddIssueCommentAnnotateContext(fx.Config(), "", "p4user", "fn", "f.cpp", 1, "1",
                                                                   "2026-06-01", false, "code");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
    CHECK(fx.RequestCount(kCommentPath) == 0);
}

TEST_CASE("JiraClient::AddIssueCommentAnnotateContext — a 400 classifies as InvalidRequest") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kCommentPath, 400, "POST");
    JiraClient client;
    const TrackerError err = client.AddIssueCommentAnnotateContext(fx.Config(), "SMT-1", "p4user", "fn", "f.cpp", 1,
                                                                   "1", "2026-06-01", false, "code");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
}
