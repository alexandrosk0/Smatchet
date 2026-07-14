// backend-impl-coverage-recovery (debt.md, 2026-06-07) — HTTP-fixture coverage for the
// JiraClient search shell (JiraIssueSearch.cpp): ProbeIssueExists, FetchIssueKeysForView, and
// the FetchIssuesStreamed page-loop (the JQL search path). All three accept an explicit config
// (configOverride / cfg param) so they drive a real JiraClient at the in-process httplib loopback
// (JiraCatalogHttpFixture) over real cpr HTTP with no ConfigManager::Load. Characterizes the
// request shape, the page-loop success/isLast termination, and the HTTP-status → TrackerError
// mapping for the reconcile probes. Extends the JiraCatalogHttpFixture pattern to the search
// paths flagged in backend-impl-coverage-recovery.

#include "JiraCatalogHttpFixture.h"
#include "Config/ConfigManager.h"
#include "Tracker/JiraClient.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

const char* const kSearchPath = "/rest/api/3/search/jql";

} // namespace

TEST_CASE("JiraClient::ProbeIssueExists — 200 is Ok(true) (issue present)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/issue/SMT-1", 200, "GET");
    JiraClient client;
    const auto res = client.ProbeIssueExists(fx.Config(), "SMT-1");
    REQUIRE(res.has_value());
    CHECK(res.value());
}

TEST_CASE("JiraClient::ProbeIssueExists — 404 is Ok(false) (issue deleted)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/issue/SMT-1", 404, "GET");
    JiraClient client;
    const auto res = client.ProbeIssueExists(fx.Config(), "SMT-1");
    REQUIRE(res.has_value());
    CHECK_FALSE(res.value());
}

TEST_CASE("JiraClient::ProbeIssueExists — other non-200 is an inconclusive Err (cache row kept)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/issue/SMT-1", 503, "GET");
    JiraClient client;
    const auto res = client.ProbeIssueExists(fx.Config(), "SMT-1");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::ServerError);
}

TEST_CASE("JiraClient::ProbeIssueExists — empty issue key is InvalidRequest, no HTTP") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const auto res = client.ProbeIssueExists(fx.Config(), "");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::InvalidRequest);
    CHECK(fx.RequestCount("/rest/api/3/issue/") == 0);
}

TEST_CASE("JiraClient::ProbeIssueExists — missing credentials is an auth error") {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira"; // Domain + ApiToken empty → auth gate rejects.
    JiraClient client;
    const auto res = client.ProbeIssueExists(cfg, "SMT-1");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::Auth);
}

TEST_CASE("JiraClient::FetchIssueKeysForView — a single terminal page projects out the issue keys") {
    JiraCatalogHttpFixture fx;
    // fields=*none ⇒ each issue rides back as just its key; the mapper projects issue["key"] → id.
    const nlohmann::json page = {{"isLast", true},
                                 {"issues", nlohmann::json::array({
                                                nlohmann::json{{"key", "SMT-1"}},
                                                nlohmann::json{{"key", "SMT-2"}},
                                            })}};
    fx.ScriptJson(kSearchPath, page, "GET");
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "project = SMT";
    const ViewsStore views; // unused by the impl (keys-only membership snapshot)
    const auto res = client.FetchIssueKeysForView(cfg, views);
    REQUIRE(res.has_value());
    CHECK(res.value().size() == 2);
    CHECK(res.value()[0] == "SMT-1");
}

TEST_CASE("JiraClient::FetchIssueKeysForView — a search response missing 'issues' is a fetch error") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson(kSearchPath, nlohmann::json::object({{"isLast", true}}), "GET");
    JiraClient client;
    const ViewsStore views;
    const auto res = client.FetchIssueKeysForView(fx.Config(), views);
    REQUIRE_FALSE(res.has_value());
    CHECK_FALSE(res.error().IsOk());
}

TEST_CASE("JiraClient::FetchIssueKeysForView — missing credentials is an auth error") {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    JiraClient client;
    const ViewsStore views;
    const auto res = client.FetchIssueKeysForView(cfg, views);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::Auth);
}

TEST_CASE("JiraClient::FetchIssuesStreamed — a terminal page streams a batch and completes the sync") {
    JiraCatalogHttpFixture fx;
    const nlohmann::json page = {{"isLast", true},
                                 {"issues", nlohmann::json::array({
                                                nlohmann::json{{"key", "SMT-10"}, {"fields", nlohmann::json::object()}},
                                                nlohmann::json{{"key", "SMT-11"}, {"fields", nlohmann::json::object()}},
                                            })}};
    fx.ScriptJson(kSearchPath, page, "GET");

    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "assignee = currentUser()";
    const ViewsStore views;

    std::vector<CachedTicket> collected;
    auto onBatch = [&](std::vector<CachedTicket>&& batch) {
        for (std::size_t i = 0; i < batch.size(); ++i) {
            collected.push_back(std::move(batch[i]));
        }
    };
    auto noCancel = []() { return false; };

    const TrackerIssueFetchSummary summary = client.FetchIssuesStreamed(onBatch, noCancel, &cfg, &views);
    CHECK(summary.FetchError.empty());
    CHECK(summary.FetchedCount == 2);
    CHECK(summary.FullSyncCompleted);
    CHECK(collected.size() == 2);
}

TEST_CASE("JiraClient::FetchIssuesStreamed — missing credentials is skipped with an InvalidRequest summary") {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira"; // Domain + ApiToken empty.
    JiraClient client;
    const ViewsStore views;
    auto onBatch = [](std::vector<CachedTicket>&&) {};
    auto noCancel = []() { return false; };
    const TrackerIssueFetchSummary summary = client.FetchIssuesStreamed(onBatch, noCancel, &cfg, &views);
    CHECK_FALSE(summary.FetchError.empty());
    CHECK(summary.Error.Kind == TrackerErrorKind::InvalidRequest);
    CHECK(summary.FetchedCount == 0);
}

TEST_CASE("JiraClient::FetchIssuesStreamed — an unreadable page body surfaces a fetch error") {
    JiraCatalogHttpFixture fx;
    // A 200 with a non-JSON body drives the page-loop invalid-JSON early exit.
    fx.ScriptRaw(kSearchPath, 200, "<html>not json</html>", "text/html", "GET");
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    const ViewsStore views;
    auto onBatch = [](std::vector<CachedTicket>&&) {};
    auto noCancel = []() { return false; };
    const TrackerIssueFetchSummary summary = client.FetchIssuesStreamed(onBatch, noCancel, &cfg, &views);
    CHECK_FALSE(summary.FetchError.empty());
    CHECK(summary.FetchedCount == 0);
}
