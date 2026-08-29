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

TEST_CASE("jql_user_display::LooksLikeAccountId accepts the alphanumeric legacy 24-char id") {
    using jql_user_display::LooksLikeAccountId;

    // Atlassian's own documented example — not pure hex (ends in 'g').
    CHECK(LooksLikeAccountId("5b10a2844c20165700ede21g"));
    // 24 letters with no digit is a word, not an id.
    CHECK_FALSE(LooksLikeAccountId("abcdefghijklmnopqrstuvwx"));
    CHECK_FALSE(LooksLikeAccountId("5b10a2844c20165700ede21"));   // 23 chars
    CHECK_FALSE(LooksLikeAccountId("5b10a2844c20165700ede21g7")); // 25 chars
}

TEST_CASE("jql_user_display::CollectUnresolvedAccountIds lists only unnamed ids, deduped") {
    using jql_user_display::CollectUnresolvedAccountIds;

    const std::string jql = "assignee in (\"712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93\", "
                            "5b10a2844c20165700ede21g) AND reporter = 5b10a2844c20165700ede21g "
                            "AND project = SMAT";
    // The catalog names the Cloud id; the legacy id (appearing twice) is left to fetch once.
    std::vector<std::string> pending = CollectUnresolvedAccountIds(jql, Catalog());
    REQUIRE(pending.size() == 1);
    CHECK(pending[0] == "5b10a2844c20165700ede21g");

    // Empty catalog: both ids pending, first-appearance order.
    pending = CollectUnresolvedAccountIds(jql, std::vector<TrackerUser>());
    REQUIRE(pending.size() == 2);
    CHECK(pending[0] == "712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93");
    CHECK(pending[1] == "5b10a2844c20165700ede21g");

    // No ids at all -> empty, and an empty query is fine.
    CHECK(CollectUnresolvedAccountIds("project = SMAT AND assignee = currentUser()", Catalog()).empty());
    CHECK(CollectUnresolvedAccountIds("", Catalog()).empty());
}

TEST_CASE("jql_user_display::UnquoteValueToken strips one quoting level") {
    using jql_user_display::UnquoteValueToken;

    CHECK(UnquoteValueToken("\"712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93\"") ==
          "712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93");
    CHECK(UnquoteValueToken("\"Jane \\\"JD\\\" Doe\"") == "Jane \"JD\" Doe");
    // Unquoted text passes through, including the empty string and a lone quote.
    CHECK(UnquoteValueToken("5b10a2844c20165700ede21g") == "5b10a2844c20165700ede21g");
    CHECK(UnquoteValueToken("") == "");
    CHECK(UnquoteValueToken("\"") == "\"");
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

namespace {

TrackerField MakeUserField(const std::string& id, const std::string& name) {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    f.IsUserType = true;
    return f;
}

std::vector<TrackerField> Fields() {
    std::vector<TrackerField> fields;
    fields.push_back(MakeUserField("assignee", "Assignee"));
    fields.push_back(MakeUserField("reporter", "Reporter"));
    TrackerField status;
    status.Id = "status";
    status.Name = "Status";
    fields.push_back(status); // non-user field: values never map
    return fields;
}

} // namespace

TEST_CASE("jql_user_display::RenderQueryWithAccountIds maps a display name in a user field") {
    int replaced = -1;
    CHECK(jql_user_display::RenderQueryWithAccountIds("assignee = \"Jane Doe\"", Fields(), Catalog(), &replaced) ==
          "assignee = 5b10ac8d82e05b22cc7d4ef5");
    CHECK(replaced == 1);

    // Case-insensitive on both the field token and the name; ids with ':' re-quote.
    CHECK(jql_user_display::RenderQueryWithAccountIds("ASSIGNEE = \"alex konstantonis\"", Fields(), Catalog(),
                                                      &replaced) ==
          "ASSIGNEE = \"712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93\"");
    CHECK(replaced == 1);
}

TEST_CASE("jql_user_display::RenderQueryWithAccountIds leaves non-user fields and ids untouched") {
    int replaced = -1;
    // Same literal under a non-user field: not a name position.
    const std::string status = "status = \"Jane Doe\"";
    CHECK(jql_user_display::RenderQueryWithAccountIds(status, Fields(), Catalog(), &replaced) == status);
    CHECK(replaced == 0);

    // Already an account id: passthrough, no double-mapping.
    const std::string asId = "assignee = \"5b10ac8d82e05b22cc7d4ef5\"";
    CHECK(jql_user_display::RenderQueryWithAccountIds(asId, Fields(), Catalog(), &replaced) == asId);
    CHECK(replaced == 0);

    // Unknown name: reaches the backend as typed (fails loudly there, never silently guessed).
    const std::string unknown = "assignee = \"Nobody Known\"";
    CHECK(jql_user_display::RenderQueryWithAccountIds(unknown, Fields(), Catalog(), &replaced) == unknown);
    CHECK(replaced == 0);
}

TEST_CASE("jql_user_display::RenderQueryWithAccountIds walks an IN list and stops at clause breaks") {
    int replaced = -1;
    CHECK(jql_user_display::RenderQueryWithAccountIds("assignee IN (\"Jane Doe\", \"Alex Konstantonis\")", Fields(),
                                                      Catalog(), &replaced) ==
          "assignee IN (5b10ac8d82e05b22cc7d4ef5, \"712020:00aa4ab4-9acf-4b2e-bc03-efbb1215ef93\")");
    CHECK(replaced == 2);

    // AND ends the value position; the next clause's non-user value stays as typed.
    CHECK(jql_user_display::RenderQueryWithAccountIds("assignee = \"Jane Doe\" AND status = \"Jane Doe\"", Fields(),
                                                      Catalog(), &replaced) ==
          "assignee = 5b10ac8d82e05b22cc7d4ef5 AND status = \"Jane Doe\"");
    CHECK(replaced == 1);

    // ORDER BY ends it too — a sort key sharing a user's name is not a value.
    CHECK(jql_user_display::RenderQueryWithAccountIds("assignee WAS \"Jane Doe\" ORDER BY created", Fields(),
                                                      Catalog(), &replaced) ==
          "assignee WAS 5b10ac8d82e05b22cc7d4ef5 ORDER BY created");
    CHECK(replaced == 1);

    // Back-to-back user clauses: a bare field token switches the state without an explicit AND.
    CHECK(jql_user_display::RenderQueryWithAccountIds("assignee = \"Jane Doe\" reporter = \"Jane Doe\"", Fields(),
                                                      Catalog(), &replaced) ==
          "assignee = 5b10ac8d82e05b22cc7d4ef5 reporter = 5b10ac8d82e05b22cc7d4ef5");
    CHECK(replaced == 2);

    // Operator keywords keep the value position open without reading as names.
    const std::string isEmpty = "assignee IS NOT EMPTY";
    CHECK(jql_user_display::RenderQueryWithAccountIds(isEmpty, Fields(), Catalog(), &replaced) == isEmpty);
    CHECK(replaced == 0);
}

TEST_CASE("jql_user_display::RenderQueryWithAccountIds resolves names only when unique") {
    // Two DIFFERENT accounts sharing a display name: ambiguous, left as typed.
    std::vector<TrackerUser> twins = Catalog();
    twins.push_back(MakeUser("999999:duplicate-name-account", "Jane Doe"));
    int replaced = -1;
    const std::string jql = "assignee = \"Jane Doe\"";
    CHECK(jql_user_display::RenderQueryWithAccountIds(jql, Fields(), twins, &replaced) == jql);
    CHECK(replaced == 0);

    // The SAME account listed twice (catalog + search-resolved merge): still one match.
    std::vector<TrackerUser> merged = Catalog();
    merged.push_back(MakeUser("5b10ac8d82e05b22cc7d4ef5", "Jane Doe"));
    CHECK(jql_user_display::RenderQueryWithAccountIds(jql, Fields(), merged, &replaced) ==
          "assignee = 5b10ac8d82e05b22cc7d4ef5");
    CHECK(replaced == 1);
}

TEST_CASE("jql_user_display::RenderQueryWithAccountIds tolerates degenerate input") {
    int replaced = -1;
    CHECK(jql_user_display::RenderQueryWithAccountIds("", Fields(), Catalog(), &replaced) == "");
    CHECK(replaced == 0);

    // Empty users: nothing resolvable, query returned untouched.
    const std::string jql = "assignee = \"Jane Doe\"";
    CHECK(jql_user_display::RenderQueryWithAccountIds(jql, Fields(), std::vector<TrackerUser>(), &replaced) == jql);
    CHECK(replaced == 0);

    // A null out-parameter is legal.
    CHECK(jql_user_display::RenderQueryWithAccountIds(jql, Fields(), Catalog(), nullptr) ==
          "assignee = 5b10ac8d82e05b22cc7d4ef5");
}
