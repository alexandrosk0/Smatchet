#include <doctest/doctest.h>

#include "Tracker/JqlUserDisplayPure.h"

#include "Tracker/TrackerFieldSchema.h"

#include <string>
#include <vector>

namespace {

TrackerUser MakeUser(const std::string& accountId, const std::string& displayName) {
    TrackerUser u;
    u.AccountId = accountId;
    u.DisplayName = displayName;
    return u;
}

std::vector<TrackerUser> Catalog() {
    std::vector<TrackerUser> users;
    users.push_back(MakeUser("712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93", "Alex Konstantonis"));
    users.push_back(MakeUser("5b10ac8d82e05b22cc7d4ef5", "Jane Doe"));
    users.push_back(
        MakeUser("qm:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93:11bb5cd5-9acf-4b2e-bc03-efbb1215ef93", "Portal User"));
    return users;
}

} // namespace

TEST_CASE("jql_user_display::LooksLikeAccountId recognises the opaque id shapes") {
    using jql_user_display::LooksLikeAccountId;

    CHECK(LooksLikeAccountId("712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93"));
    CHECK(LooksLikeAccountId("qm:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93:11bb5cd5-9acf-4b2e-bc03-efbb1215ef93"));
    CHECK(LooksLikeAccountId("00aa4ab4-9acf-4b2e-bc03-efbb1215ef93"));
    CHECK(LooksLikeAccountId("5b10ac8d82e05b22cc7d4ef5"));

    CHECK_FALSE(LooksLikeAccountId(""));
    CHECK_FALSE(LooksLikeAccountId("Alex Konstantonis"));
    CHECK_FALSE(LooksLikeAccountId("currentUser()"));
    CHECK_FALSE(LooksLikeAccountId("SMAT-1234"));
    // Trailing text that is nearly a UUID must not pass — the shape test is exact.
    CHECK_FALSE(LooksLikeAccountId("712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef9"));
    CHECK_FALSE(LooksLikeAccountId(":00aa4ab4-9acf-4b2e-bc03-efbb1215ef93"));
}

TEST_CASE("jql_user_display::RenderQueryWithUserNames names a quoted accountId") {
    int replaced = -1;
    const std::string out = jql_user_display::RenderQueryWithUserNames(
        "assignee = \"712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93\"", Catalog(), &replaced);
    CHECK(out == "assignee = \"Alex Konstantonis\"");
    CHECK(replaced == 1);
}

TEST_CASE("jql_user_display::RenderQueryWithUserNames names a bare accountId") {
    int replaced = 0;
    const std::string out =
        jql_user_display::RenderQueryWithUserNames("reporter = 5b10ac8d82e05b22cc7d4ef5", Catalog(), &replaced);
    CHECK(out == "reporter = \"Jane Doe\"");
    CHECK(replaced == 1);
}

TEST_CASE("jql_user_display::RenderQueryWithUserNames rewrites every occurrence") {
    int replaced = 0;
    const std::string out = jql_user_display::RenderQueryWithUserNames(
        "assignee in (\"712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93\", 5b10ac8d82e05b22cc7d4ef5) ORDER BY created DESC",
        Catalog(), &replaced);
    CHECK(out == "assignee in (\"Alex Konstantonis\", \"Jane Doe\") ORDER BY created DESC");
    CHECK(replaced == 2);
}

TEST_CASE("jql_user_display::RenderQueryWithUserNames leaves unknown / non-user text alone") {
    int replaced = -1;
    const std::string jql = "project = SMAT AND status = \"In Progress\" AND assignee = currentUser()";
    CHECK(jql_user_display::RenderQueryWithUserNames(jql, Catalog(), &replaced) == jql);
    CHECK(replaced == 0);

    // A well-formed id that is not in the catalog stays verbatim — a half-resolved echo
    // would be worse than the raw query.
    const std::string unknown = "assignee = \"999999:11bb5cd5-9acf-4b2e-bc03-efbb1215ef93\"";
    CHECK(jql_user_display::RenderQueryWithUserNames(unknown, Catalog(), &replaced) == unknown);
    CHECK(replaced == 0);
}

TEST_CASE("jql_user_display::RenderQueryWithUserNames quotes only when the name needs it") {
    std::vector<TrackerUser> users;
    users.push_back(MakeUser("5b10ac8d82e05b22cc7d4ef5", "jdoe"));
    int replaced = 0;
    CHECK(jql_user_display::RenderQueryWithUserNames("assignee = 5b10ac8d82e05b22cc7d4ef5", users, &replaced) ==
          "assignee = jdoe");
    CHECK(replaced == 1);

    // A name carrying a quote is escaped, so the echo is still readable as a query.
    std::vector<TrackerUser> quoted;
    quoted.push_back(MakeUser("5b10ac8d82e05b22cc7d4ef5", "Jane \"JD\" Doe"));
    CHECK(jql_user_display::RenderQueryWithUserNames("assignee = 5b10ac8d82e05b22cc7d4ef5", quoted, &replaced) ==
          "assignee = \"Jane \\\"JD\\\" Doe\"");
}

TEST_CASE("jql_user_display::RenderQueryWithUserNames tolerates degenerate input") {
    int replaced = -1;
    CHECK(jql_user_display::RenderQueryWithUserNames("", Catalog(), &replaced) == "");
    CHECK(replaced == 0);

    // Empty catalog: nothing resolvable, query returned untouched.
    const std::string jql = "assignee = \"712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93\"";
    CHECK(jql_user_display::RenderQueryWithUserNames(jql, std::vector<TrackerUser>(), &replaced) == jql);
    CHECK(replaced == 0);

    // Unterminated quote: the scan stops at end-of-input without dropping bytes.
    const std::string open = "assignee = \"712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93";
    CHECK(jql_user_display::RenderQueryWithUserNames(open, Catalog(), &replaced) == "assignee = \"Alex Konstantonis\"");
    CHECK(replaced == 1);

    // A null out-parameter is legal.
    CHECK(jql_user_display::RenderQueryWithUserNames(jql, Catalog(), nullptr) == "assignee = \"Alex Konstantonis\"");
}
