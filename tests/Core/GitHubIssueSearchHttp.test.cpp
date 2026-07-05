// TEST_COVERAGE_GAP_MAP.md Tier 2 (coverage-gap-tier2-backend-shell-fixtures.md Slice 2) —
// fixture coverage for the `GitHubIssueSearch.cpp` shell (the largest untested backend
// shell). The free functions `smatchet::github::FetchIssuesViaRestApi` /
// `FetchIssuesForKeysViaRestApi` are driven at an in-process httplib loopback over real
// cpr HTTP, pinning what the pure suites (GitHubQueryFromJql / GitHubFetchPlan /
// GitHubIssueSearchMapping) cannot see: the GraphQL search page loop + 10-page cap, the
// two-source (search + REST commits) orchestration with its single-terminal-emit
// contract, fatal page classification, the `key =` post-filter, and the per-key single
// -issue GET loop with its error taxonomy. Characterization tests — they pin CURRENT
// behaviour of the shipped shell.

#include "GitHubIssueSearch.h"
#include "HttpRequestCapture.h"
#include "JiraCatalogHttpFixture.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

using smatchet::github::FetchIssuesForKeysViaRestApi;
using smatchet::github::FetchIssuesViaRestApi;
using smatchet_tests::HttpRequestCapture;

namespace {

const std::string kGraphQlPath = "/graphql";
const std::string kCommitsPath = "/repos/o/r/commits";

// One minimal GraphQL search node the mapper accepts (ticket id = o/r#<number>).
nlohmann::json IssueNode(int number, const std::string& title) {
    return nlohmann::json{{"__typename", "Issue"},
                          {"number", number},
                          {"title", title},
                          {"state", "OPEN"},
                          {"repository", {{"name", "r"}, {"owner", {{"login", "o"}}}}}};
}

nlohmann::json SearchPage(const nlohmann::json& nodes, bool hasNext, const std::string& endCursor) {
    return nlohmann::json{{"data",
                           {{"search",
                             {{"issueCount", 0},
                              {"nodes", nodes},
                              {"pageInfo", {{"hasNextPage", hasNext}, {"endCursor", endCursor}}}}}}}};
}

std::string Loopback(const JiraCatalogHttpFixture& fx) { return "http://127.0.0.1:" + std::to_string(fx.Port()); }

} // namespace

TEST_CASE("GitHub search — PAT missing fails fast with zero HTTP calls") {
    JiraCatalogHttpFixture fx;
    std::string fetchError;
    bool fullSync = true;
    const std::vector<CachedTicket> tickets =
        FetchIssuesViaRestApi(Loopback(fx), "", "o", "r", "", &fullSync, &fetchError, nullptr);
    CHECK(tickets.empty());
    CHECK_FALSE(fullSync);
    CHECK(fetchError.find("GitHub PAT not configured") != std::string::npos);
    CHECK(fx.RequestCount(kGraphQlPath) == 0);
}

TEST_CASE("GitHub search — two-page GraphQL pagination, cursor threading, wire shape") {
    JiraCatalogHttpFixture fx;
    HttpRequestCapture capture;
    fx.ScriptHandler(
        kGraphQlPath,
        [&capture](const httplib::Request& req) {
            capture.Push(req.body);
            const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
            const nlohmann::json after =
                body.is_object() ? body["variables"].value("after", nlohmann::json()) : nlohmann::json();
            if (after.is_string() && after.get<std::string>() == "c2") {
                return SearchPage(nlohmann::json::array({IssueNode(3, "third")}), false, "");
            }
            return SearchPage(nlohmann::json::array({IssueNode(1, "first"), IssueNode(2, "second")}), true, "c2");
        },
        "POST");

    bool fullSync = false;
    std::string fetchError;
    std::string warning;
    std::vector<std::pair<size_t, bool>> emissions;
    const std::vector<CachedTicket> tickets =
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "", &fullSync, &fetchError, &warning,
                              [&emissions](const std::vector<CachedTicket>& page, bool isLast) {
                                  emissions.push_back({page.size(), isLast});
                              });

    CHECK(fetchError.empty());
    CHECK(warning.empty());
    CHECK(fullSync);
    REQUIRE(tickets.size() == 3);
    CHECK(tickets[0].id == "o/r#1");
    CHECK(tickets[2].id == "o/r#3");
    REQUIRE(emissions.size() == 2);
    CHECK(emissions[0] == std::pair<size_t, bool>{2, false});
    CHECK(emissions[1] == std::pair<size_t, bool>{1, true});
    CHECK(fx.RequestCount(kGraphQlPath) == 2);

    const std::vector<std::string> bodies = capture.Snapshot();
    REQUIRE(bodies.size() == 2);
    const nlohmann::json first = nlohmann::json::parse(bodies[0]);
    CHECK(first["variables"]["after"].is_null());
    CHECK(first["variables"]["first"].get<int>() == 100);
    CHECK(first["variables"]["q"].get<std::string>().find("repo:o/r") != std::string::npos);
    const nlohmann::json second = nlohmann::json::parse(bodies[1]);
    CHECK(second["variables"]["after"].get<std::string>() == "c2");
}

TEST_CASE("GitHub search — fatal page classification still emits exactly one terminal page") {
    JiraCatalogHttpFixture fx;
    std::string fetchError;
    bool fullSync = true;
    std::vector<std::pair<size_t, bool>> emissions;
    const auto onPage = [&emissions](const std::vector<CachedTicket>& page, bool isLast) {
        emissions.push_back({page.size(), isLast});
    };

    SUBCASE("GraphQL errors[] on HTTP 200 is fatal with the joined message") {
        fx.ScriptJson(kGraphQlPath, nlohmann::json{{"errors", {{{"message", "Bad cursor"}}}}}, "POST");
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "", &fullSync, &fetchError, nullptr, onPage);
        CHECK(fetchError == "GitHub GraphQL error: Bad cursor");
    }
    SUBCASE("non-200 extracts the GitHub message field") {
        fx.ScriptRaw(kGraphQlPath, 401, R"({"message":"Bad credentials"})", "application/json", "POST");
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "", &fullSync, &fetchError, nullptr, onPage);
        CHECK(fetchError == "GitHub fetch error (HTTP 401): Bad credentials");
    }
    SUBCASE("invalid JSON is fatal") {
        fx.ScriptRaw(kGraphQlPath, 200, "{\"data\": {truncated", "application/json", "POST");
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "", &fullSync, &fetchError, nullptr, onPage);
        CHECK(fetchError == "GitHub returned invalid JSON");
    }
    SUBCASE("missing data.search is fatal") {
        fx.ScriptJson(kGraphQlPath, nlohmann::json{{"data", nlohmann::json::object()}}, "POST");
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "", &fullSync, &fetchError, nullptr, onPage);
        CHECK(fetchError == "GitHub GraphQL response missing data.search");
    }

    CHECK_FALSE(fullSync);
    CHECK_FALSE(fetchError.empty());
    // The single-terminal-emit contract: a fatal first page still closes the stream.
    REQUIRE(emissions.size() == 1);
    CHECK(emissions[0] == std::pair<size_t, bool>{0, true});
}

TEST_CASE("GitHub search — endCursor missing while hasNextPage=true stops early with a warning") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson(kGraphQlPath, SearchPage(nlohmann::json::array({IssueNode(1, "only")}), true, ""), "POST");

    bool fullSync = true;
    std::string warning;
    const std::vector<CachedTicket> tickets =
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "", &fullSync, nullptr, &warning);

    CHECK(tickets.size() == 1);
    CHECK_FALSE(fullSync); // never reached a short page
    CHECK(warning.find("endCursor missing") != std::string::npos);
    CHECK(fx.RequestCount(kGraphQlPath) == 1);
}

TEST_CASE("GitHub search — page cap (10) bounds an always-hasNextPage server with a warning") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson(kGraphQlPath, SearchPage(nlohmann::json::array({IssueNode(1, "loop")}), true, "same"), "POST");

    bool fullSync = true;
    std::string warning;
    const std::vector<CachedTicket> tickets =
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "", &fullSync, nullptr, &warning);

    CHECK(tickets.size() == 10); // one node per page, capped
    CHECK_FALSE(fullSync);
    CHECK(warning.find("page cap (10)") != std::string::npos);
    CHECK(fx.RequestCount(kGraphQlPath) == 10);
}

TEST_CASE("GitHub search — `key =` clause narrows streamed pages and the aggregate") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson(kGraphQlPath,
                  SearchPage(nlohmann::json::array({IssueNode(1, "first"), IssueNode(2, "second")}), false, ""),
                  "POST");

    std::vector<std::pair<size_t, bool>> emissions;
    const std::vector<CachedTicket> tickets =
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "key = \"o/r#2\"", nullptr, nullptr, nullptr,
                              [&emissions](const std::vector<CachedTicket>& page, bool isLast) {
                                  emissions.push_back({page.size(), isLast});
                              });

    REQUIRE(tickets.size() == 1);
    CHECK(tickets[0].id == "o/r#2");
    REQUIRE(emissions.size() == 1);
    CHECK(emissions[0] == std::pair<size_t, bool>{1, true}); // page narrowed before emission
}

TEST_CASE("GitHub search — commits-only view uses the REST commit source, not GraphQL") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson(kCommitsPath,
                  nlohmann::json::array({{{"sha", "abc1234def"}, {"commit", {{"message", "fix: things"}}}},
                                         {{"sha", "9876543210"}, {"commit", {{"message", "feat: stuff"}}}}}));

    bool fullSync = false;
    std::string fetchError;
    std::string warning;
    std::vector<std::pair<size_t, bool>> emissions;
    const std::vector<CachedTicket> tickets =
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "type = commit", &fullSync, &fetchError, &warning,
                              [&emissions](const std::vector<CachedTicket>& page, bool isLast) {
                                  emissions.push_back({page.size(), isLast});
                              });

    CHECK(fetchError.empty());
    CHECK(warning.empty());
    CHECK(fullSync);
    REQUIRE(tickets.size() == 2);
    CHECK(tickets[0].id == "o/r@abc1234def");
    CHECK(fx.RequestCount(kGraphQlPath) == 0);
    CHECK(fx.RequestCount(kCommitsPath) == 1);
    REQUIRE(emissions.size() == 1);
    CHECK(emissions[0] == std::pair<size_t, bool>{2, true});
}

TEST_CASE("GitHub search — commit-fetch failure is a soft warning; issue rows survive") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson(kGraphQlPath, SearchPage(nlohmann::json::array({IssueNode(1, "issue")}), false, ""), "POST");
    fx.ScriptRaw(kCommitsPath, 404, R"({"message":"Not Found"})", "application/json");

    bool fullSync = true;
    std::string fetchError;
    std::string warning;
    std::vector<std::pair<size_t, bool>> emissions;
    const std::vector<CachedTicket> tickets =
        FetchIssuesViaRestApi(Loopback(fx), "pat", "o", "r", "type = all", &fullSync, &fetchError, &warning,
                              [&emissions](const std::vector<CachedTicket>& page, bool isLast) {
                                  emissions.push_back({page.size(), isLast});
                              });

    CHECK(fetchError.empty()); // commit failure is non-fatal by design
    CHECK(warning.find("cached commit rows preserved") != std::string::npos);
    CHECK_FALSE(fullSync); // but never claim a full sync (would prune cached commit rows)
    REQUIRE(tickets.size() == 1);
    CHECK(tickets[0].id == "o/r#1");
    // Issue page emitted non-terminal (commits owned isLast), commit source closed the stream.
    REQUIRE(emissions.size() == 2);
    CHECK(emissions[0] == std::pair<size_t, bool>{1, false});
    CHECK(emissions[1] == std::pair<size_t, bool>{0, true});
}

TEST_CASE("GitHub key fetch — error taxonomy of the single-issue GET loop") {
    JiraCatalogHttpFixture fx;

    SUBCASE("empty key list is Ok with zero HTTP calls") {
        const auto r = FetchIssuesForKeysViaRestApi(Loopback(fx), "pat", {});
        REQUIRE(r.has_value());
        CHECK(r.value().empty());
    }
    SUBCASE("missing PAT is an Auth error") {
        const auto r = FetchIssuesForKeysViaRestApi(Loopback(fx), "", {"o/r#1"});
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().Kind == TrackerErrorKind::Auth);
    }
    SUBCASE("malformed key is InvalidRequest naming the key") {
        const auto r = FetchIssuesForKeysViaRestApi(Loopback(fx), "pat", {"not-a-key"});
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().Kind == TrackerErrorKind::InvalidRequest);
        CHECK(r.error().Detail.find("not-a-key") != std::string::npos);
    }
    SUBCASE("404 maps to NotFound with the GitHub message and the key in the detail") {
        fx.ScriptRaw("/repos/o/r/issues/9", 404, R"({"message":"Not Found"})", "application/json");
        const auto r = FetchIssuesForKeysViaRestApi(Loopback(fx), "pat", {"o/r#9"});
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().Kind == TrackerErrorKind::NotFound);
        CHECK(r.error().Detail.find("o/r#9") != std::string::npos);
        CHECK(r.error().Detail.find("Not Found") != std::string::npos);
    }
    SUBCASE("invalid JSON on 200 is a Parse error") {
        fx.ScriptRaw("/repos/o/r/issues/3", 200, "{truncated", "application/json");
        const auto r = FetchIssuesForKeysViaRestApi(Loopback(fx), "pat", {"o/r#3"});
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().Kind == TrackerErrorKind::Parse);
    }
    SUBCASE("good key maps the REST issue to a ticket") {
        fx.ScriptJson("/repos/o/r/issues/7", nlohmann::json{{"number", 7}, {"title", "seventh"}, {"state", "open"}});
        const auto r = FetchIssuesForKeysViaRestApi(Loopback(fx), "pat", {"o/r#7"});
        REQUIRE(r.has_value());
        REQUIRE(r.value().size() == 1);
        CHECK(r.value()[0].id == "o/r#7");
        CHECK(fx.RequestCount("/repos/o/r/issues/7") == 1);
    }
}
