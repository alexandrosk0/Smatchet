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
#include "Tracker/SearchFetchGuards.h"

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

TEST_CASE("JiraClient::FetchIssuesStreamed — a result cut short by the page cap carries a warning") {
    // Every page claims more results, so the loop stops at its page cap. The rows fetched stay valid,
    // but the sync must show a caveat rather than a plain success.
    JiraCatalogHttpFixture fx;
    fx.ScriptHandler(
        kSearchPath,
        [](const httplib::Request&) -> nlohmann::json {
            nlohmann::json body = nlohmann::json::object();
            body["isLast"] = false;
            body["nextPageToken"] = "more";
            body["issues"] = nlohmann::json::array();
            return body;
        },
        "GET");
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "project = SMT";
    const ViewsStore views;
    auto onBatch = [](std::vector<CachedTicket>&&) {};
    auto noCancel = []() { return false; };

    const TrackerIssueFetchSummary summary = client.FetchIssuesStreamed(onBatch, noCancel, &cfg, &views);
    CHECK(summary.FetchError.empty());
    CHECK_FALSE(summary.FullSyncCompleted);
    CHECK(summary.Warning.find("page cap") != std::string::npos);
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

TEST_CASE("JiraClient::FetchIssueKeysForView — a clean 2-page walk is a complete snapshot, not a partial walk") {
    // Regression: the page loop broke on the clean isLast page BEFORE carrying that page's (empty)
    // cursor, so page 1's nextPageToken survived and `tokenLeftover` read true — every query with
    // more than one page (>100 issues) was rejected as "returned a partial page walk".
    JiraCatalogHttpFixture fx;
    fx.ScriptHandler(
        kSearchPath,
        [](const httplib::Request& req) -> nlohmann::json {
            nlohmann::json body = nlohmann::json::object();
            if (req.get_param_value("nextPageToken").empty()) {
                body["isLast"] = false;
                body["nextPageToken"] = "page-2";
                body["issues"] = nlohmann::json::array({nlohmann::json{{"key", "SMT-1"}}});
            } else {
                body["isLast"] = true;
                body["issues"] = nlohmann::json::array({nlohmann::json{{"key", "SMT-2"}}});
            }
            return body;
        },
        "GET");
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "project = SMT";
    const ViewsStore views;
    const auto res = client.FetchIssueKeysForView(cfg, views);
    REQUIRE(res.has_value());
    REQUIRE(res.value().size() == 2);
    CHECK(res.value()[0] == "SMT-1");
    CHECK(res.value()[1] == "SMT-2");
    CHECK(fx.RequestCount(kSearchPath) == 2);
}

TEST_CASE("JiraClient::FetchIssueKeysForView — hitting the page cap with a token pending is a partial walk") {
    // Every page claims more results — the loop exhausts its 50-page safety cap with a cursor
    // still pending, which must NOT be treated as an authoritative membership snapshot.
    JiraCatalogHttpFixture fx;
    fx.ScriptHandler(
        kSearchPath,
        [](const httplib::Request&) -> nlohmann::json {
            nlohmann::json body = nlohmann::json::object();
            body["isLast"] = false;
            body["nextPageToken"] = "more";
            body["issues"] = nlohmann::json::array();
            return body;
        },
        "GET");
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "project = SMT";
    const ViewsStore views;
    const auto res = client.FetchIssueKeysForView(cfg, views);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Detail.find("partial page walk") != std::string::npos);
    CHECK(fx.RequestCount(kSearchPath) == 50);
}

// --- delta-sync Phase 2: the request carries updated, and expand=changelog only for the history column ---

namespace {
// Streams one empty terminal page and returns the (fields, expand) query params the search sent.
void CaptureSearchParams(const ViewsStore& views, std::string& outFields, std::string& outExpand, bool& outHasExpand) {
    JiraCatalogHttpFixture fx;
    std::string fields;
    std::string expand;
    bool hasExpand = false;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request& req) {
        fields = req.get_param_value("fields");
        hasExpand = req.has_param("expand");
        expand = req.get_param_value("expand");
        return nlohmann::json{{"isLast", true}, {"issues", nlohmann::json::array()}};
    });
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "project = SMT";
    client.FetchIssuesStreamed([](std::vector<CachedTicket>&&) {}, []() { return false; }, &cfg, &views);
    outFields = fields;
    outExpand = expand;
    outHasExpand = hasExpand;
}

ViewsStore ViewWithFields(const std::vector<std::string>& fields) {
    ViewsStore store;
    ViewDefinition view;
    view.Id = "v1";
    view.Fields = fields;
    store.Views.push_back(view);
    store.ActiveViewId = "v1";
    return store;
}
} // namespace

TEST_CASE("JiraClient::FetchIssuesStreamed — no expand without the history column; updated is always requested") {
    std::string fields;
    std::string expand;
    bool hasExpand = true;
    CaptureSearchParams(ViewWithFields({"summary", "status"}), fields, expand, hasExpand);
    CHECK_FALSE(hasExpand);
    CHECK(fields.find("updated") != std::string::npos);
    CHECK(fields.find("changelog") == std::string::npos);
}

TEST_CASE("JiraClient::FetchIssuesStreamed — the history column asks Jira to expand the changelog") {
    std::string fields;
    std::string expand;
    bool hasExpand = false;
    CaptureSearchParams(ViewWithFields({"summary", "history"}), fields, expand, hasExpand);
    CHECK(hasExpand);
    CHECK(expand == "changelog");
}

TEST_CASE("JiraClient::FetchIssuesForKeys — the keyed fetch follows the same expand rule") {
    JiraCatalogHttpFixture fx;
    bool hasExpand = true;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request& req) {
        hasExpand = req.has_param("expand");
        return nlohmann::json{{"isLast", true}, {"issues", nlohmann::json::array()}};
    });
    JiraClient client;
    const ViewsStore views = ViewWithFields({"summary"});
    const auto res = client.FetchIssuesForKeys(fx.Config(), {"SMT-1", "SMT-2"}, views);
    REQUIRE(res.has_value());
    CHECK_FALSE(hasExpand);
}

// --- delta-sync Phase 3: the roster request (keys + updated only) -----------------------------------

TEST_CASE("JiraClient::FetchViewRoster — asks for updated only: no bodies, no expand, 100 per page") {
    JiraCatalogHttpFixture fx;
    std::string fields;
    std::string maxResults;
    bool hasExpand = true;
    std::string jql;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request& req) {
        fields = req.get_param_value("fields");
        maxResults = req.get_param_value("maxResults");
        hasExpand = req.has_param("expand");
        jql = req.get_param_value("jql");
        return nlohmann::json{
            {"isLast", true},
            {"issues", nlohmann::json::array({nlohmann::json{{"key", "SMT-1"}, {"fields", {{"updated", "T1"}}}},
                                              nlohmann::json{{"key", "SMT-2"}, {"fields", {{"updated", "T2"}}}}})}};
    });
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "project = SMT";
    const ViewsStore views;
    CHECK(client.SupportsVersionedRoster());
    const auto res = client.FetchViewRoster(cfg, views, []() { return false; });
    REQUIRE(res.has_value());
    CHECK(fields == "updated");
    CHECK(maxResults == "100");
    CHECK_FALSE(hasExpand);
    CHECK(jql == "project = SMT");
    REQUIRE(res.value().Entries.size() == 2u);
    CHECK(res.value().Entries[0].Key == "SMT-1");
    CHECK(res.value().Entries[0].Updated == "T1");
    CHECK(res.value().Entries[1].Updated == "T2");
    CHECK(res.value().Complete);
    CHECK(res.value().Warning.empty());
    CHECK(fx.RequestCount(kSearchPath) == 1); // the whole reload costs ONE request here
}

TEST_CASE("JiraClient::FetchViewRoster — follows the page token and is complete after the last page") {
    JiraCatalogHttpFixture fx;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request& req) {
        if (req.get_param_value("nextPageToken").empty()) {
            return nlohmann::json{{"isLast", false},
                                  {"nextPageToken", "tok2"},
                                  {"issues", nlohmann::json::array({nlohmann::json{{"key", "SMT-1"}}})}};
        }
        return nlohmann::json{{"isLast", true}, {"issues", nlohmann::json::array({nlohmann::json{{"key", "SMT-2"}}})}};
    });
    JiraClient client;
    const ViewsStore views;
    const auto res = client.FetchViewRoster(fx.Config(), views, []() { return false; });
    REQUIRE(res.has_value());
    CHECK(res.value().Entries.size() == 2u);
    CHECK(res.value().Complete);
}

TEST_CASE("JiraClient::FetchViewRoster — hitting the roster page cap yields an incomplete roster with a warning") {
    JiraCatalogHttpFixture fx;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request&) {
        return nlohmann::json{{"isLast", false},
                              {"nextPageToken", "more"},
                              {"issues", nlohmann::json::array({nlohmann::json{{"key", "SMT-1"}}})}};
    });
    JiraClient client;
    const ViewsStore views;
    const auto res = client.FetchViewRoster(fx.Config(), views, []() { return false; });
    REQUIRE(res.has_value());
    CHECK_FALSE(res.value().Complete);
    CHECK_FALSE(res.value().Warning.empty());
    CHECK(fx.RequestCount(kSearchPath) == static_cast<int>(smatchet::search_guards::kJiraMaxRosterPages));
}

TEST_CASE("JiraClient::FetchViewRoster — failures keep their kind; a cancel is Cancelled; no credentials is Auth") {
    const ViewsStore views;
    {
        JiraCatalogHttpFixture fx;
        fx.ScriptStatus(kSearchPath, 503, "GET");
        JiraClient client;
        const auto res = client.FetchViewRoster(fx.Config(), views, []() { return false; });
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().Kind == TrackerErrorKind::ServerError);
    }
    {
        JiraCatalogHttpFixture fx;
        fx.ScriptJson(kSearchPath, nlohmann::json{{"isLast", true}, {"issues", nlohmann::json::array()}});
        JiraClient client;
        const auto res = client.FetchViewRoster(fx.Config(), views, []() { return true; });
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().Kind == TrackerErrorKind::Cancelled);
    }
    {
        TrackerConfig cfg;
        cfg.TrackerType = "Jira";
        JiraClient client;
        const auto res = client.FetchViewRoster(cfg, views, []() { return false; });
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().Kind == TrackerErrorKind::Auth);
    }
}
