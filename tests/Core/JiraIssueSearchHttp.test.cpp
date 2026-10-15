// backend-impl-coverage-recovery (debt.md, 2026-06-07) — HTTP-fixture coverage for the
// JiraClient search shell (JiraIssueSearch.cpp): ProbeIssueExists, FetchIssueKeysForView, and
// the FetchIssuesStreamed page-loop (the JQL search path). All three accept an explicit config
// (configOverride / cfg param) so they drive a real JiraClient at the in-process httplib loopback
// (JiraCatalogHttpFixture) over real cpr HTTP with no ConfigManager::Load. Characterizes the
// request shape, the page-loop success/isLast termination, and the HTTP-status → TrackerError
// mapping for the reconcile probes. Extends the JiraCatalogHttpFixture pattern to the search
// paths flagged in backend-impl-coverage-recovery.

#include "../support/ScopedLimits.h"
#include "JiraCatalogHttpFixture.h"
#include "Config/ConfigManager.h"
#include "Tracker/JiraClient.h"
#include "Tracker/SearchFetchGuards.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
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

namespace {
// Scripts /search/jql as `totalRows` issues in 100-row pages chained by nextPageToken "pN" (the last page
// is isLast with no token). Returns the number of pages served via `pagesServed`.
void ScriptPagedSearch(JiraCatalogHttpFixture& fx, int totalRows, std::shared_ptr<int> pagesServed) {
    fx.ScriptHandler(
        kSearchPath,
        [totalRows, pagesServed](const httplib::Request& req) -> nlohmann::json {
            const std::string token = req.get_param_value("nextPageToken");
            const int page = token.empty() ? 1 : std::stoi(token.substr(1));
            ++*pagesServed;
            const int first = (page - 1) * 100;
            const int last = std::min(totalRows, first + 100);
            nlohmann::json issues = nlohmann::json::array();
            for (int i = first; i < last; ++i) {
                issues.push_back(
                    nlohmann::json{{"key", "SMT-" + std::to_string(i + 1)}, {"fields", nlohmann::json::object()}});
            }
            nlohmann::json body = nlohmann::json::object();
            body["issues"] = std::move(issues);
            const bool isLast = last >= totalRows;
            body["isLast"] = isLast;
            if (!isLast) {
                body["nextPageToken"] = "p" + std::to_string(page + 1);
            }
            return body;
        },
        "GET");
}

TrackerIssueFetchSummary RunPagedSearch(JiraCatalogHttpFixture& fx) {
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "project = SMT";
    const ViewsStore views;
    auto onBatch = [](std::vector<CachedTicket>&&) {};
    auto noCancel = []() { return false; };
    return client.FetchIssuesStreamed(onBatch, noCancel, &cfg, &views);
}
} // namespace

TEST_CASE("JiraClient::FetchIssuesStreamed — a view of exactly the result cap completes without a warning") {
    // 500 rows on the last page (default cap 500): nothing was left behind, so it is NOT truncated.
    JiraCatalogHttpFixture fx;
    auto pages = std::make_shared<int>(0);
    ScriptPagedSearch(fx, 500, pages);
    const TrackerIssueFetchSummary summary = RunPagedSearch(fx);
    CHECK(summary.FetchError.empty());
    CHECK(summary.FetchedCount == 500);
    CHECK(summary.FullSyncCompleted);
    CHECK(summary.Warning.empty());
    CHECK(*pages == 5);
}

TEST_CASE("JiraClient::FetchIssuesStreamed — a view larger than the result cap stops at the cap with a warning") {
    // 700 rows, default cap 500: the walk stops once page 5 brings the total to 500 with more pages pending.
    JiraCatalogHttpFixture fx;
    auto pages = std::make_shared<int>(0);
    ScriptPagedSearch(fx, 700, pages);
    const TrackerIssueFetchSummary summary = RunPagedSearch(fx);
    CHECK(summary.FetchError.empty());
    CHECK(summary.FetchedCount == 500);
    CHECK_FALSE(summary.FullSyncCompleted);
    CHECK(summary.Warning.find("result count reached 500") != std::string::npos);
    CHECK(*pages == 5);
}

TEST_CASE("JiraClient::FetchIssueKeysForView — a view over the interactive cap still reconciles (probe caps)") {
    // 700 keys exceeds the 500-row interactive cap but is inside the membership-probe cap (5000), so the
    // keys-only walk is complete and the disappearance reconcile can run.
    JiraCatalogHttpFixture fx;
    auto pages = std::make_shared<int>(0);
    ScriptPagedSearch(fx, 700, pages);
    JiraClient client;
    TrackerConfig cfg = fx.Config();
    cfg.JqlQuery = "project = SMT";
    const ViewsStore views;
    const auto res = client.FetchIssueKeysForView(cfg, views);
    REQUIRE(res.has_value());
    CHECK(res.value().size() == 700);
    CHECK(*pages == 7);
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
    CHECK(fx.RequestCount(kSearchPath) == smatchet::search_guards::JiraMaxPages());
}

TEST_CASE("JiraClient::FetchViewRoster — hitting the result cap yields an incomplete roster with a warning") {
    // The roster shares the reload's row ceiling (JiraCaps), so the change monitor can never learn about more
    // rows than a reload would hold; a view past it reports incomplete and the monitor takes its legacy path.
    const smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.JiraSearchMaxResults = 2; });
    JiraCatalogHttpFixture fx;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request&) {
        return nlohmann::json{
            {"isLast", false},
            {"nextPageToken", "more"},
            {"issues", nlohmann::json::array({nlohmann::json{{"key", "SMT-1"}}, nlohmann::json{{"key", "SMT-2"}},
                                              nlohmann::json{{"key", "SMT-3"}}})}};
    });
    JiraClient client;
    const ViewsStore views;
    const auto res = client.FetchViewRoster(fx.Config(), views, []() { return false; });
    REQUIRE(res.has_value());
    CHECK_FALSE(res.value().Complete);
    CHECK(res.value().Warning.find("result count") != std::string::npos);
    CHECK(fx.RequestCount(kSearchPath) == 1); // stopped at the cap, no further page
}

TEST_CASE("JiraClient::FetchViewRoster — a view exactly at the result cap is still complete") {
    const smatchet_tests::ScopedLimits limits([](smatchet::limits::LimitsConfig& l) { l.JiraSearchMaxResults = 2; });
    JiraCatalogHttpFixture fx;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request&) {
        return nlohmann::json{
            {"isLast", true},
            {"issues", nlohmann::json::array({nlohmann::json{{"key", "SMT-1"}}, nlohmann::json{{"key", "SMT-2"}}})}};
    });
    JiraClient client;
    const ViewsStore views;
    const auto res = client.FetchViewRoster(fx.Config(), views, []() { return false; });
    REQUIRE(res.has_value());
    CHECK(res.value().Complete);
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

// --- delta-sync Phase 5: roster for keys / children of keys ---------------------------------------------

TEST_CASE("JiraClient::FetchRosterForKeys — key-in query, updated only, 40 keys per request") {
    JiraCatalogHttpFixture fx;
    std::vector<std::string> jqls;
    std::vector<std::string> fieldParams;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request& req) {
        jqls.push_back(req.get_param_value("jql"));
        fieldParams.push_back(req.get_param_value("fields"));
        return nlohmann::json{
            {"isLast", true},
            {"issues", nlohmann::json::array({nlohmann::json{{"key", "SMT-1"}, {"fields", {{"updated", "T1"}}}}})}};
    });
    JiraClient client;
    std::vector<std::string> keys;
    for (int i = 1; i <= 45; ++i) {
        keys.push_back("SMT-" + std::to_string(i));
    }
    const auto res = client.FetchRosterForKeys(fx.Config(), keys, []() { return false; });
    REQUIRE(res.has_value());
    REQUIRE(jqls.size() == 2u); // 40 + 5
    CHECK(jqls[0].find("key in (") == 0);
    CHECK(fieldParams[0] == "updated");
    CHECK(res.value().Entries.size() == 2u); // one scripted row per request
    CHECK(res.value().Complete);
}

TEST_CASE("JiraClient::FetchRosterForKeys — a refused batch is an error so the caller can isolate keys") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus(kSearchPath, 400, "GET");
    JiraClient client;
    const auto res = client.FetchRosterForKeys(fx.Config(), {"SMT-1"}, []() { return false; });
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::InvalidRequest);
}

TEST_CASE("JiraClient::FetchChildRosterOfKeys — parent-in query returns the children's versions") {
    JiraCatalogHttpFixture fx;
    std::string jql;
    fx.ScriptHandler(kSearchPath, [&](const httplib::Request& req) {
        jql = req.get_param_value("jql");
        return nlohmann::json{
            {"isLast", true},
            {"issues", nlohmann::json::array({nlohmann::json{{"key", "SMT-2"}, {"fields", {{"updated", "T9"}}}}})}};
    });
    JiraClient client;
    const auto res = client.FetchChildRosterOfKeys(fx.Config(), {"SMT-1"}, []() { return false; });
    REQUIRE(res.has_value());
    CHECK(jql.find("parent = ") == 0);
    REQUIRE(res.value().Entries.size() == 1u);
    CHECK(res.value().Entries[0].Updated == "T9");
    CHECK(res.value().Complete);
}
