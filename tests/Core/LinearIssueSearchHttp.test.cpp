// TEST_COVERAGE_GAP_MAP.md Tier 2 (coverage-gap-tier2-backend-shell-fixtures.md Slice 1) —
// fixture coverage for the security-flagged `LinearIssueSearch.cpp` GraphQL shell. The
// free functions `smatchet::linear::FetchIssuesViaGraphQl` / `FetchIssuesForKeysViaGraphQl`
// are driven at an in-process httplib loopback over real cpr HTTP, pinning what the pure
// helper suites (LinearClientHelpers / LinearIssueMappingPure / LinearQueryFromJql) cannot
// see: cursor pagination + the page cap, the onPage/isLast streaming protocol (including
// the terminal-emit guarantee on failure), 200-with-`errors[]` fatal classification, the
// request-body wire shape that actually reaches the server, and the key-fetch
// or-disjunction with its `first` clamp. Characterization tests — they pin CURRENT
// behaviour of the shipped shell.

#include "JiraCatalogHttpFixture.h"
#include "LinearIssueSearch.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

const std::string kGraphQlPath = "/graphql";

// One minimal issue node the pure mapper accepts.
nlohmann::json IssueNode(int number, const std::string& title) {
    return nlohmann::json{{"id", std::string("uuid-") + std::to_string(number)},
                          {"identifier", std::string("LIN-") + std::to_string(number)},
                          {"number", number},
                          {"title", title},
                          {"state", {{"id", "s1"}, {"name", "Todo"}, {"type", "unstarted"}}},
                          {"team", {{"id", "t1"}, {"key", "LIN"}, {"name", "Linear Team"}}}};
}

nlohmann::json IssuesPage(const nlohmann::json& nodes, bool hasNext, const std::string& endCursor) {
    return nlohmann::json{
        {"data", {{"issues", {{"nodes", nodes}, {"pageInfo", {{"hasNextPage", hasNext}, {"endCursor", endCursor}}}}}}}};
}

// Thread-safe capture of the GraphQL request bodies the shell actually sends —
// asserted on the test thread after the call returns (doctest is not thread-safe).
struct BodyCapture {
    std::mutex Mutex;
    std::vector<std::string> Bodies;

    void Push(const std::string& body) {
        std::lock_guard<std::mutex> lock(Mutex);
        Bodies.push_back(body);
    }
    std::vector<std::string> Snapshot() {
        std::lock_guard<std::mutex> lock(Mutex);
        return Bodies;
    }
};

std::string LoopbackUrl(const JiraCatalogHttpFixture& fx) {
    return "http://127.0.0.1:" + std::to_string(fx.Port()) + kGraphQlPath;
}

} // namespace

TEST_CASE("Linear fetch — two-page pagination, onPage/isLast protocol, cursor threading") {
    JiraCatalogHttpFixture fx;
    BodyCapture capture;
    fx.ScriptHandler(
        kGraphQlPath,
        [&capture](const httplib::Request& req) {
            capture.Push(req.body);
            const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
            const nlohmann::json after =
                body.is_object() ? body["variables"].value("after", nlohmann::json()) : nlohmann::json();
            if (after.is_string() && after.get<std::string>() == "cursor-2") {
                return IssuesPage(nlohmann::json::array({IssueNode(3, "third")}), false, "");
            }
            return IssuesPage(nlohmann::json::array({IssueNode(1, "first"), IssueNode(2, "second")}), true, "cursor-2");
        },
        "POST");

    bool fullSync = false;
    std::string fetchError;
    std::string warning;
    std::vector<std::pair<size_t, bool>> emissions; // (page size, isLast) per onPage call
    const std::vector<CachedTicket> tickets = smatchet::linear::FetchIssuesViaGraphQl(
        LoopbackUrl(fx), "api-key", "t1", "LIN", "", &fullSync, &fetchError, &warning,
        [&emissions](const std::vector<CachedTicket>& page, bool isLast) {
            emissions.push_back({page.size(), isLast});
        });

    CHECK(fetchError.empty());
    CHECK(warning.empty());
    CHECK(fullSync);
    REQUIRE(tickets.size() == 3);
    CHECK(tickets[0].id == "LIN-1");
    CHECK(tickets[2].id == "LIN-3");
    REQUIRE(emissions.size() == 2);
    CHECK(emissions[0] == std::pair<size_t, bool>{2, false});
    CHECK(emissions[1] == std::pair<size_t, bool>{1, true});
    CHECK(fx.RequestCount(kGraphQlPath) == 2);

    // Wire shape: page 1 sends after=null + the team-key scope; page 2 threads the cursor.
    const std::vector<std::string> bodies = capture.Snapshot();
    REQUIRE(bodies.size() == 2);
    const nlohmann::json first = nlohmann::json::parse(bodies[0]);
    CHECK(first["variables"]["after"].is_null());
    CHECK(first["variables"]["first"].get<int>() == 50);
    CHECK(first["variables"]["filter"]["team"]["key"]["eq"].get<std::string>() == "LIN");
    const nlohmann::json second = nlohmann::json::parse(bodies[1]);
    CHECK(second["variables"]["after"].get<std::string>() == "cursor-2");
}

TEST_CASE("Linear fetch — fatal page classification still emits the terminal page") {
    JiraCatalogHttpFixture fx;
    std::string fetchError;
    std::vector<std::pair<size_t, bool>> emissions;
    const auto onPage = [&emissions](const std::vector<CachedTicket>& page, bool isLast) {
        emissions.push_back({page.size(), isLast});
    };

    SUBCASE("GraphQL errors[] on HTTP 200 is a failure with the joined message") {
        fx.ScriptJson(kGraphQlPath, nlohmann::json{{"errors", {{{"message", "Rate limit exceeded"}}}}}, "POST");
        smatchet::linear::FetchIssuesViaGraphQl(LoopbackUrl(fx), "k", "", "", "", nullptr, &fetchError, nullptr,
                                                onPage);
        CHECK(fetchError == "Rate limit exceeded");
    }
    SUBCASE("non-200 without errors[] falls back to the HTTP status message") {
        fx.ScriptRaw(kGraphQlPath, 400, "{}", "application/json", "POST");
        smatchet::linear::FetchIssuesViaGraphQl(LoopbackUrl(fx), "k", "", "", "", nullptr, &fetchError, nullptr,
                                                onPage);
        CHECK(fetchError == "HTTP 400");
    }
    SUBCASE("invalid JSON is rejected") {
        fx.ScriptRaw(kGraphQlPath, 200, "{\"data\": {truncated", "application/json", "POST");
        smatchet::linear::FetchIssuesViaGraphQl(LoopbackUrl(fx), "k", "", "", "", nullptr, &fetchError, nullptr,
                                                onPage);
        CHECK(fetchError == "Linear returned invalid JSON");
    }
    SUBCASE("missing data.issues is rejected") {
        fx.ScriptJson(kGraphQlPath, nlohmann::json{{"data", nlohmann::json::object()}}, "POST");
        smatchet::linear::FetchIssuesViaGraphQl(LoopbackUrl(fx), "k", "", "", "", nullptr, &fetchError, nullptr,
                                                onPage);
        CHECK(fetchError == "Linear GraphQL response missing data.issues");
    }

    // The streaming contract: consumers always see exactly one terminal (isLast) page,
    // even when page 1 fails outright.
    REQUIRE(emissions.size() == 1);
    CHECK(emissions[0] == std::pair<size_t, bool>{0, true});
    CHECK_FALSE(fetchError.empty());
}

TEST_CASE("Linear fetch — endCursor missing while hasNextPage=true stops early with a warning") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson(kGraphQlPath,
                  IssuesPage(nlohmann::json::array({IssueNode(1, "only")}), /*hasNext*/ true, /*endCursor*/ ""),
                  "POST");

    bool fullSync = true;
    std::string warning;
    const std::vector<CachedTicket> tickets = smatchet::linear::FetchIssuesViaGraphQl(
        LoopbackUrl(fx), "k", "", "", "", &fullSync, nullptr, &warning, nullptr);

    CHECK(tickets.size() == 1);
    CHECK_FALSE(fullSync); // never reached a short page
    CHECK(warning.find("endCursor missing") != std::string::npos);
    CHECK(fx.RequestCount(kGraphQlPath) == 1); // no second request without a cursor
}

TEST_CASE("Linear fetch — page cap (20) bounds an always-hasNextPage server with a warning") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson(kGraphQlPath, IssuesPage(nlohmann::json::array({IssueNode(1, "loop")}), true, "same-cursor"), "POST");

    bool fullSync = true;
    std::string warning;
    const std::vector<CachedTicket> tickets = smatchet::linear::FetchIssuesViaGraphQl(
        LoopbackUrl(fx), "k", "", "", "", &fullSync, nullptr, &warning, nullptr);

    CHECK(tickets.size() == 20); // one node per page, capped
    CHECK_FALSE(fullSync);
    CHECK(warning.find("page cap (20)") != std::string::npos);
    CHECK(fx.RequestCount(kGraphQlPath) == 20);
}

TEST_CASE("Linear key fetch — invalid key is an InvalidRequest error with zero HTTP calls") {
    JiraCatalogHttpFixture fx;
    const auto result = smatchet::linear::FetchIssuesForKeysViaGraphQl(LoopbackUrl(fx), "k", {"LIN-1", "not-a-key"});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().Kind == TrackerErrorKind::InvalidRequest);
    CHECK(result.error().Detail.find("not-a-key") != std::string::npos);
    CHECK(fx.RequestCount(kGraphQlPath) == 0);
}

TEST_CASE("Linear key fetch — or-disjunction wire shape and result mapping") {
    JiraCatalogHttpFixture fx;
    BodyCapture capture;
    fx.ScriptHandler(
        kGraphQlPath,
        [&capture](const httplib::Request& req) {
            capture.Push(req.body);
            return IssuesPage(nlohmann::json::array({IssueNode(7, "seventh"), IssueNode(9, "ninth")}), false, "");
        },
        "POST");

    const auto result = smatchet::linear::FetchIssuesForKeysViaGraphQl(LoopbackUrl(fx), "k", {"LIN-7", "ABC-9"});

    REQUIRE(result.has_value());
    REQUIRE(result.value().size() == 2);
    CHECK(result.value()[0].id == "LIN-7");

    const std::vector<std::string> bodies = capture.Snapshot();
    REQUIRE(bodies.size() == 1);
    const nlohmann::json body = nlohmann::json::parse(bodies[0]);
    const nlohmann::json& orClauses = body["variables"]["filter"]["or"];
    REQUIRE(orClauses.is_array());
    REQUIRE(orClauses.size() == 2);
    CHECK(orClauses[0]["team"]["key"]["eq"].get<std::string>() == "LIN");
    CHECK(orClauses[0]["number"]["eq"].get<int>() == 7);
    CHECK(orClauses[1]["team"]["key"]["eq"].get<std::string>() == "ABC");
    CHECK(orClauses[1]["number"]["eq"].get<int>() == 9);
    CHECK(body["variables"]["first"].get<int>() == 2);
    CHECK(body["variables"]["after"].is_null());
}
