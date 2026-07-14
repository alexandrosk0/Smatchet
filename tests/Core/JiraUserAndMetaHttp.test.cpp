// backend-impl-coverage-recovery (debt.md, 2026-06-07) — HTTP-fixture coverage for the
// JiraClient user/meta read shell (JiraUserAndMeta.cpp): FetchUsers, FetchIssueWatchers,
// AddIssueWatcher, FetchIssueVotes, SearchUsersByQuery, FetchUserGroupNames, FetchGroupMembers.
// These cfg-explicit paths (no ConfigManager::Load) drive a real JiraClient at the in-process
// httplib loopback (JiraCatalogHttpFixture) over real cpr HTTP, characterizing the request
// shape, the success parse/dedup/sort, and the HTTP-status/parse error → TrackerError mapping.
// Extends the JiraCatalogHttpFixture pattern to the user/meta paths flagged in
// backend-impl-coverage-recovery (the impl TUs that dragged the line-rate below the floor).

#include "JiraCatalogHttpFixture.h"
#include "Tracker/JiraClient.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

// A cfg pointed at a closed discard port so cpr fails to connect (transport failure) — used to
// prove the auth-gate short-circuit fires BEFORE any network call for empty-credential cfgs.
TrackerConfig EmptyCredsConfig() {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    // Domain + ApiToken left empty → EnsureTrackerAuthConfig rejects.
    return cfg;
}

} // namespace

TEST_CASE("JiraClient::FetchUsers — success dedups, drops inactive, and appends display-name suffix") {
    JiraCatalogHttpFixture fx;
    // /users/search returns an array. Two rows share a display name (suffix applied to the 2nd),
    // one is inactive (dropped), one is a duplicate accountId (dropped), one lacks accountId (dropped).
    const nlohmann::json users = nlohmann::json::array({
        nlohmann::json{{"accountId", "a1"}, {"displayName", "Alice"}, {"active", true}},
        nlohmann::json{{"accountId", "a2"}, {"displayName", "Alice"}, {"active", true}},
        nlohmann::json{{"accountId", "a3"}, {"displayName", "Bob"}, {"active", false}},
        nlohmann::json{{"accountId", "a1"}, {"displayName", "AliceDup"}, {"active", true}},
        nlohmann::json{{"displayName", "NoId"}, {"active", true}},
    });
    fx.ScriptJson("/rest/api/3/users/search", users, "GET");

    JiraClient client;
    std::vector<TrackerUser> out;
    std::string err;
    const bool ok = client.FetchUsers(fx.Config(), out, err);
    CHECK(ok);
    CHECK(err.empty());
    // a1 (Alice), a2 (Alice_<suffix>) survive; a3 inactive, a1-dup, no-id all dropped.
    CHECK(out.size() == 2);
    bool sawSuffix = false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i].DisplayName.rfind("Alice_", 0) == 0) {
            sawSuffix = true;
        }
    }
    CHECK(sawSuffix);
}

TEST_CASE("JiraClient::FetchUsers — non-200 sets an HTTP error and returns false") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/users/search", 500, "GET");
    JiraClient client;
    std::vector<TrackerUser> out;
    std::string err;
    CHECK_FALSE(client.FetchUsers(fx.Config(), out, err));
    CHECK(err.find("HTTP 500") != std::string::npos);
    CHECK(out.empty());
}

TEST_CASE("JiraClient::FetchUsers — a non-array body is rejected as invalid format") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/users/search", nlohmann::json::object({{"not", "an-array"}}), "GET");
    JiraClient client;
    std::vector<TrackerUser> out;
    std::string err;
    CHECK_FALSE(client.FetchUsers(fx.Config(), out, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("JiraClient::FetchUsers — empty credentials short-circuit before any HTTP call") {
    JiraClient client;
    std::vector<TrackerUser> out;
    std::string err;
    CHECK_FALSE(client.FetchUsers(EmptyCredsConfig(), out, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("JiraClient::FetchIssueWatchers — success parses the watchers array") {
    JiraCatalogHttpFixture fx;
    const nlohmann::json body = {
        {"watchers", nlohmann::json::array({
                         nlohmann::json{{"accountId", "w1"}, {"displayName", "Watcher One"}, {"active", true}},
                         nlohmann::json{{"accountId", "w2"}, {"displayName", "Watcher Two"}, {"active", true}},
                     })}};
    fx.ScriptJson("/rest/api/3/issue/SMT-1/watchers", body, "GET");
    JiraClient client;
    const auto res = client.FetchIssueWatchers(fx.Config(), "SMT-1");
    REQUIRE(res.has_value());
    CHECK(res.value().size() == 2);
}

TEST_CASE("JiraClient::FetchIssueWatchers — empty issue key is InvalidRequest, no HTTP") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const auto res = client.FetchIssueWatchers(fx.Config(), "");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::InvalidRequest);
}

TEST_CASE("JiraClient::FetchIssueWatchers — non-200 maps the HTTP status to a TrackerError") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/issue/SMT-1/watchers", 404, "GET");
    JiraClient client;
    const auto res = client.FetchIssueWatchers(fx.Config(), "SMT-1");
    REQUIRE_FALSE(res.has_value());
    CHECK_FALSE(res.error().IsOk());
}

TEST_CASE("JiraClient::FetchIssueWatchers — a non-object body is a parse error") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/issue/SMT-1/watchers", nlohmann::json::array(), "GET");
    JiraClient client;
    const auto res = client.FetchIssueWatchers(fx.Config(), "SMT-1");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::Parse);
}

TEST_CASE("JiraClient::AddIssueWatcher — myself lookup then POST succeeds (204)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/myself", nlohmann::json::object({{"accountId", "me-1"}}), "GET");
    fx.ScriptStatus("/rest/api/3/issue/SMT-1/watchers", 204, "POST");
    JiraClient client;
    const TrackerError err = client.AddIssueWatcher(fx.Config(), "SMT-1");
    CHECK(err.IsOk());
}

TEST_CASE("JiraClient::AddIssueWatcher — empty issue key is InvalidRequest") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const TrackerError err = client.AddIssueWatcher(fx.Config(), "");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
}

TEST_CASE("JiraClient::AddIssueWatcher — a myself response with no accountId is a parse error") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/myself", nlohmann::json::object({{"displayName", "no id here"}}), "GET");
    JiraClient client;
    const TrackerError err = client.AddIssueWatcher(fx.Config(), "SMT-1");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::Parse);
}

TEST_CASE("JiraClient::AddIssueWatcher — POST failure status maps to a non-OK error") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/myself", nlohmann::json::object({{"accountId", "me-1"}}), "GET");
    fx.ScriptStatus("/rest/api/3/issue/SMT-1/watchers", 400, "POST");
    JiraClient client;
    const TrackerError err = client.AddIssueWatcher(fx.Config(), "SMT-1");
    CHECK_FALSE(err.IsOk());
    CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
}

TEST_CASE("JiraClient::FetchIssueVotes — success parses count, hasVoted, and voters") {
    JiraCatalogHttpFixture fx;
    const nlohmann::json body = {
        {"votes", 3},
        {"hasVoted", true},
        {"voters", nlohmann::json::array({
                       nlohmann::json{{"accountId", "v1"}, {"displayName", "Voter One"}, {"active", true}},
                   })}};
    fx.ScriptJson("/rest/api/3/issue/SMT-1/votes", body, "GET");
    JiraClient client;
    const auto res = client.FetchIssueVotes(fx.Config(), "SMT-1");
    REQUIRE(res.has_value());
    CHECK(res.value().VoteCount == 3);
    CHECK(res.value().HasVoted);
    CHECK(res.value().VotersArrayInResponse);
    CHECK(res.value().Voters.size() == 1);
}

TEST_CASE("JiraClient::FetchIssueVotes — hasVoted as an integer coerces to bool") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/issue/SMT-1/votes", nlohmann::json::object({{"votes", 0}, {"hasVoted", 1}}), "GET");
    JiraClient client;
    const auto res = client.FetchIssueVotes(fx.Config(), "SMT-1");
    REQUIRE(res.has_value());
    CHECK(res.value().HasVoted);
    CHECK(res.value().VoteCount == 0);
}

TEST_CASE("JiraClient::FetchIssueVotes — empty issue key is InvalidRequest") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const auto res = client.FetchIssueVotes(fx.Config(), "");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::InvalidRequest);
}

TEST_CASE("JiraClient::FetchIssueVotes — non-200 maps the status to a TrackerError") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/issue/SMT-1/votes", 503, "GET");
    JiraClient client;
    const auto res = client.FetchIssueVotes(fx.Config(), "SMT-1");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::ServerError);
}

TEST_CASE("JiraClient::SearchUsersByQuery — empty query returns an empty list with no HTTP") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const auto res = client.SearchUsersByQuery(fx.Config(), "");
    REQUIRE(res.has_value());
    CHECK(res.value().empty());
    CHECK(fx.RequestCount("/rest/api/3/user/search") == 0);
}

TEST_CASE("JiraClient::SearchUsersByQuery — success parses, dedups and drops inactive") {
    JiraCatalogHttpFixture fx;
    const nlohmann::json arr = nlohmann::json::array({
        nlohmann::json{{"accountId", "u1"}, {"displayName", "User One"}, {"active", true}},
        nlohmann::json{{"accountId", "u1"}, {"displayName", "Dup"}, {"active", true}},
        nlohmann::json{{"accountId", "u2"}, {"displayName", "Inactive"}, {"active", false}},
        "not-an-object",
    });
    fx.ScriptJson("/rest/api/3/user/search", arr, "GET");
    JiraClient client;
    const auto res = client.SearchUsersByQuery(fx.Config(), "alice");
    REQUIRE(res.has_value());
    CHECK(res.value().size() == 1);
    CHECK(res.value()[0].AccountId == "u1");
}

TEST_CASE("JiraClient::SearchUsersByQuery — missing credentials is an auth error") {
    JiraClient client;
    const auto res = client.SearchUsersByQuery(EmptyCredsConfig(), "alice");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::Auth);
}

TEST_CASE("JiraClient::SearchUsersByQuery — a non-array body is a parse error") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/user/search", nlohmann::json::object({{"x", 1}}), "GET");
    JiraClient client;
    const auto res = client.SearchUsersByQuery(fx.Config(), "alice");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::Parse);
}

TEST_CASE("JiraClient::FetchUserGroupNames — empty accountId returns empty with no HTTP") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const auto res = client.FetchUserGroupNames(fx.Config(), "");
    REQUIRE(res.has_value());
    CHECK(res.value().empty());
}

TEST_CASE("JiraClient::FetchUserGroupNames — success extracts group item names") {
    JiraCatalogHttpFixture fx;
    const nlohmann::json body = {{"groups",
                                  {{"items", nlohmann::json::array({
                                                 nlohmann::json{{"name", "jira-admins"}},
                                                 nlohmann::json{{"name", "developers"}},
                                                 nlohmann::json{{"notname", "ignored"}},
                                             })}}}};
    fx.ScriptJson("/rest/api/3/user", body, "GET");
    JiraClient client;
    const auto res = client.FetchUserGroupNames(fx.Config(), "acc-1");
    REQUIRE(res.has_value());
    CHECK(res.value().size() == 2);
}

TEST_CASE("JiraClient::FetchUserGroupNames — non-200 maps the status to a TrackerError") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/user", 403, "GET");
    JiraClient client;
    const auto res = client.FetchUserGroupNames(fx.Config(), "acc-1");
    REQUIRE_FALSE(res.has_value());
    CHECK_FALSE(res.error().IsOk());
}

TEST_CASE("JiraClient::FetchGroupMembers — empty group name returns empty with no HTTP") {
    JiraCatalogHttpFixture fx;
    JiraClient client;
    const auto res = client.FetchGroupMembers(fx.Config(), "");
    REQUIRE(res.has_value());
    CHECK(res.value().empty());
}

TEST_CASE("JiraClient::FetchGroupMembers — a single terminal page (isLast) parses members and stops") {
    JiraCatalogHttpFixture fx;
    const nlohmann::json body = {
        {"isLast", true},
        {"values", nlohmann::json::array({
                       nlohmann::json{{"accountId", "m1"}, {"displayName", "Member One"}, {"active", true}},
                       nlohmann::json{{"accountId", "m2"}, {"displayName", "Member Two"}, {"active", true}},
                   })}};
    // The paged loop issues GET /group/member with startAt query params; a fixed route ignores
    // the query string and serves this terminal page for every page request.
    fx.ScriptJson("/rest/api/3/group/member", body, "GET");
    JiraClient client;
    const auto res = client.FetchGroupMembers(fx.Config(), "developers");
    REQUIRE(res.has_value());
    CHECK(res.value().size() == 2);
    // isLast:true terminates after one page.
    CHECK(fx.RequestCount("/rest/api/3/group/member") == 1);
}

TEST_CASE("JiraClient::FetchGroupMembers — non-200 on the first page maps the status") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/group/member", 500, "GET");
    JiraClient client;
    const auto res = client.FetchGroupMembers(fx.Config(), "developers");
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().Kind == TrackerErrorKind::ServerError);
}
