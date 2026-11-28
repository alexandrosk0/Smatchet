// query_facets doctest — the clause editor behind the grid's facet chips
// (docs/plans/input-ux-w3-filter-without-jql.md S1).

#include "Tracker/QueryFacetClausesPure.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

using namespace query_facets;

namespace {

FacetParse Parse(const std::string& q, FacetDialect d) { return ParseFacetClauses(q, d, FacetFieldIds(d)); }

FacetClause Clause(const char* field, const char* value, FacetOp op = FacetOp::Eq) {
    FacetClause c;
    c.FieldId = field;
    c.Op = op;
    c.Values.push_back(value);
    return c;
}

FacetClause Me(const char* field) {
    FacetClause c;
    c.FieldId = field;
    c.CurrentUser = true;
    return c;
}

std::string Text(const std::string& q, const std::pair<std::size_t, std::size_t>& span) {
    return q.substr(span.first, span.second);
}

bool Locks(const FacetParse& p, const char* field) {
    return std::find(p.LockedFieldIds.begin(), p.LockedFieldIds.end(), field) != p.LockedFieldIds.end();
}

const FacetDialect kDialects[] = {FacetDialect::Jql, FacetDialect::GitHubSubset, FacetDialect::LinearSubset};

} // namespace

TEST_CASE("query_facets: parse, add, remove returns the original bytes") {
    const char* const queries[] = {"",
                                   "assignee = currentUser()",
                                   "assignee=currentUser()",
                                   "assignee=currentUser() ORDER BY updated DESC",
                                   "labels = \"bug\"   ORDER BY created",
                                   "ORDER BY updated"};
    for (FacetDialect d : kDialects) {
        for (const char* q : queries) {
            CAPTURE(static_cast<int>(d));
            CAPTURE(q);
            REQUIRE(Parse(q, d).Chippable);
            const std::string added = AddFacetClause(q, d, Clause("status", "Open"));
            CHECK(added != q);
            const FacetParse p = Parse(added, d);
            REQUIRE(p.Chippable);
            const auto it = std::find_if(p.Clauses.begin(), p.Clauses.end(),
                                         [](const FacetClause& c) { return c.FieldId == "status"; });
            REQUIRE(it != p.Clauses.end());
            CHECK(RemoveFacetClause(added, it->At, it->Len) == q);
        }
    }
}

TEST_CASE("query_facets: an added clause goes before the ORDER BY tail") {
    const FacetClause open = Clause("status", "Open");
    CHECK(AddFacetClause("", FacetDialect::Jql, open) == "status = \"Open\"");
    CHECK(AddFacetClause("   ", FacetDialect::Jql, open) == "status = \"Open\"");
    CHECK(AddFacetClause("project = A order by key", FacetDialect::Jql, open) ==
          "project = A AND status = \"Open\" order by key");
    CHECK(AddFacetClause("ORDER BY key", FacetDialect::LinearSubset, open) == "status = \"Open\" ORDER BY key");
}

TEST_CASE("query_facets: removing a first, middle or last clause drops one AND") {
    const std::string q = "status = a AND priority = b AND labels = c ORDER BY key";
    const FacetParse p = Parse(q, FacetDialect::Jql);
    REQUIRE(p.Clauses.size() == 3);
    CHECK(RemoveFacetClause(q, p.Clauses[0].At, p.Clauses[0].Len) == "priority = b AND labels = c ORDER BY key");
    CHECK(RemoveFacetClause(q, p.Clauses[1].At, p.Clauses[1].Len) == "status = a AND labels = c ORDER BY key");
    CHECK(RemoveFacetClause(q, p.Clauses[2].At, p.Clauses[2].Len) == "status = a AND priority = b ORDER BY key");
    // A value that is the word "and" is not a connector.
    const std::string words = "labels = \"and\" AND status = x";
    const FacetParse w = Parse(words, FacetDialect::Jql);
    REQUIRE(w.Clauses.size() == 2);
    CHECK(RemoveFacetClause(words, w.Clauses[1].At, w.Clauses[1].Len) == "labels = \"and\"");
}

TEST_CASE("query_facets: a span outside the query changes nothing") {
    const std::string q = "status = a";
    CHECK(RemoveFacetClause(q, 99, 1) == q);
    CHECK(RemoveFacetClause(q, 0, 99) == q);
    CHECK(RemoveFacetClause(q, 0, 0) == q);
    CHECK(ReplaceFacetClause(q, FacetDialect::Jql, 99, 1, Clause("status", "b")) == q);
    CHECK(ReplaceFacetClause(q, FacetDialect::Jql, 1, 99, Clause("status", "b")) == q);
}

TEST_CASE("query_facets: a replace rewrites the clause at its span only") {
    const std::string q = "status = \"Open\" AND status = \"Closed\"";
    const FacetParse p = Parse(q, FacetDialect::Jql);
    REQUIRE(p.Clauses.size() == 2);
    CHECK(ReplaceFacetClause(q, FacetDialect::Jql, p.Clauses[1].At, p.Clauses[1].Len, Clause("status", "Done")) ==
          "status = \"Open\" AND status = \"Done\"");
}

TEST_CASE("query_facets: a second GitHub label is added, never replaced") {
    CHECK(AddFacetClause("labels = \"a\"", FacetDialect::GitHubSubset, Clause("labels", "b")) ==
          "labels = \"a\" AND labels = \"b\"");
    CHECK(FieldRepeats(FacetDialect::GitHubSubset, "labels"));
    CHECK_FALSE(FieldRepeats(FacetDialect::GitHubSubset, "status"));
    CHECK_FALSE(FieldRepeats(FacetDialect::Jql, "labels"));
    CHECK_FALSE(FieldRepeats(FacetDialect::LinearSubset, "labels"));
}

TEST_CASE("query_facets: currentUser() is the one function a chip can hold") {
    for (FacetDialect d : kDialects) {
        for (const char* q : {"assignee = currentUser()", "assignee=currentUser()"}) {
            const FacetParse p = Parse(q, d);
            REQUIRE(p.Chippable);
            REQUIRE(p.Clauses.size() == 1);
            CHECK(p.Clauses[0].CurrentUser);
            CHECK(p.Clauses[0].Values.empty());
            CHECK(p.Clauses[0].Len == std::string(q).size());
        }
        CHECK_FALSE(Parse("assignee in membersOf(\"x\")", d).Chippable);
        CHECK_FALSE(Parse("status in openStatuses()", d).Chippable);
    }
    CHECK(AddFacetClause("", FacetDialect::LinearSubset, Me("assignee")) == "assignee = currentUser()");
    CHECK(AddFacetClause("", FacetDialect::LinearSubset, Me("status")).empty());
    const FacetParse list = Parse("assignee in (currentUser(), bob)", FacetDialect::Jql);
    REQUIRE(list.Clauses.size() == 1);
    CHECK(list.Clauses[0].CurrentUser);
    CHECK(list.Clauses[0].Values == std::vector<std::string>{"bob"});
    // Linear reads a quoted or spaced spelling differently from the translator's point of view.
    const FacetParse quoted = Parse("assignee = \"currentUser()\"", FacetDialect::LinearSubset);
    REQUIRE(quoted.Clauses.size() == 1);
    CHECK(quoted.Clauses[0].CurrentUser);
    const FacetParse spaced = Parse("assignee = currentUser ()", FacetDialect::LinearSubset);
    REQUIRE(spaced.Clauses.size() == 1);
    CHECK_FALSE(spaced.Clauses[0].CurrentUser);
}

TEST_CASE("query_facets: a top-level OR or NOT is not chippable") {
    for (FacetDialect d : kDialects) {
        CHECK_FALSE(Parse("status = a OR status = b", d).Chippable);
        CHECK_FALSE(Parse("NOT status = a", d).Chippable);
        CHECK_FALSE(Parse("status = a AND", d).Chippable);
        CHECK_FALSE(Parse("AND status = a", d).Chippable);
        CHECK_FALSE(Parse("summary ~ x", d).Chippable);
        CHECK_FALSE(Parse("just words", d).Chippable);
    }
    const FacetParse p = Parse("a = 1 OR b = 2 ORDER BY key", FacetDialect::Jql);
    CHECK_FALSE(p.Chippable);
    CHECK(p.Clauses.empty());
    CHECK(p.OtherSpans.empty());
    CHECK(p.OrderByAt == std::string("a = 1 OR b = 2 ").size());
}

TEST_CASE("query_facets: a group is one opaque clause in Jql only") {
    const std::string q = "(status = a OR status = b) AND labels = c";
    const FacetParse p = Parse(q, FacetDialect::Jql);
    REQUIRE(p.Chippable);
    REQUIRE(p.OtherSpans.size() == 1);
    CHECK(Text(q, p.OtherSpans[0]) == "(status = a OR status = b)");
    REQUIRE(p.Clauses.size() == 1);
    CHECK_FALSE(Parse(q, FacetDialect::GitHubSubset).Chippable);
    CHECK_FALSE(Parse(q, FacetDialect::LinearSubset).Chippable);
}

TEST_CASE("query_facets: a custom Jql query is wrapped once, ORDER BY left outside") {
    const std::string q = "status = a OR status = b ORDER BY updated DESC";
    const std::string wrapped = AddFacetClause(q, FacetDialect::Jql, Clause("labels", "x"));
    CHECK(wrapped == "(status = a OR status = b) AND labels = \"x\" ORDER BY updated DESC");
    const FacetParse p = Parse(wrapped, FacetDialect::Jql);
    REQUIRE(p.Chippable);
    REQUIRE(p.OtherSpans.size() == 1);
    REQUIRE(p.Clauses.size() == 1);
    CHECK(p.Clauses[0].FieldId == "labels");
    CHECK(AddFacetClause(wrapped, FacetDialect::Jql, Clause("priority", "High")) ==
          "(status = a OR status = b) AND labels = \"x\" AND priority = \"High\" ORDER BY updated DESC");
    CHECK(RemoveFacetClause(wrapped, p.Clauses[0].At, p.Clauses[0].Len) ==
          "(status = a OR status = b) ORDER BY updated DESC");
}

TEST_CASE("query_facets: a query that cannot be edited safely stays as it is") {
    const FacetClause open = Clause("status", "Open");
    CHECK(AddFacetClause("a = 1 OR b = 2", FacetDialect::GitHubSubset, open) == "a = 1 OR b = 2");
    CHECK(AddFacetClause("a = 1 OR b = 2", FacetDialect::LinearSubset, open) == "a = 1 OR b = 2");
    for (const char* q : {"(a = 1", "a = 1)", "a = \"unclosed"}) {
        CAPTURE(q);
        for (FacetDialect d : kDialects) {
            CHECK_FALSE(Parse(q, d).Chippable);
            CHECK(AddFacetClause(q, d, open) == q);
        }
    }
}

TEST_CASE("query_facets: the ORDER BY tail is kept and a quoted one is not a tail") {
    const FacetParse p = Parse("labels = \"order by\" AND status = a ORDER BY key", FacetDialect::Jql);
    REQUIRE(p.Chippable);
    CHECK(p.Clauses.size() == 2);
    CHECK(p.OrderByAt == std::string("labels = \"order by\" AND status = a ").size());
}

TEST_CASE("query_facets: Linear splits and strips on single spaces only") {
    CHECK_FALSE(Parse("status = a\nAND labels = b", FacetDialect::LinearSubset).Chippable);
    CHECK_FALSE(Parse("status = a AND\tlabels = b", FacetDialect::LinearSubset).Chippable);
    CHECK_FALSE(Parse("status = a\nORDER BY key", FacetDialect::LinearSubset).Chippable);
    CHECK_FALSE(Parse("status = a ORDER  BY key", FacetDialect::LinearSubset).Chippable);
    CHECK(Parse("status = a  AND  labels = b", FacetDialect::LinearSubset).Chippable);
    CHECK(Parse("status = a\nAND labels = b", FacetDialect::Jql).Chippable);
}

TEST_CASE("query_facets: Linear values the translator would read differently are not chips and lock the field") {
    const std::string q = "labels in (\"frontend,backend\") AND priority in (\"High\", \"Unknown\")";
    const FacetParse p = Parse(q, FacetDialect::LinearSubset);
    REQUIRE(p.Chippable);
    CHECK(p.Clauses.empty());
    CHECK(p.OtherSpans.size() == 2);
    CHECK(Locks(p, "labels"));
    CHECK(Locks(p, "priority"));
}

TEST_CASE("query_facets: a GitHub token the tracker keeps stays an opaque clause") {
    const std::string q = "type:pr AND is:pr AND is:merged AND assignee = currentUser()";
    const FacetParse p = Parse(q, FacetDialect::GitHubSubset);
    REQUIRE(p.Chippable);
    REQUIRE(p.OtherSpans.size() == 3);
    CHECK(Text(q, p.OtherSpans[0]) == "type:pr");
    CHECK(Text(q, p.OtherSpans[1]) == "is:pr");
    CHECK(p.Clauses.size() == 1);
    CHECK(p.LockedFieldIds.empty());
}

TEST_CASE("query_facets: GitHub shorthand reads as facet clauses with canonical ids") {
    const std::string q = "status:closed AND is:open AND label:bug AND assignee:@me AND author:octocat";
    const FacetParse p = Parse(q, FacetDialect::GitHubSubset);
    REQUIRE(p.Chippable);
    REQUIRE(p.Clauses.size() == 5);
    CHECK(p.Clauses[0].FieldId == "status");
    CHECK(p.Clauses[1].FieldId == "status");
    CHECK(p.Clauses[1].Values == std::vector<std::string>{"Open"});
    CHECK(p.Clauses[2].FieldId == "labels");
    CHECK(p.Clauses[3].FieldId == "assignee");
    CHECK(p.Clauses[3].Values == std::vector<std::string>{"@me"});
    CHECK(p.Clauses[4].FieldId == "reporter");
    const FacetParse closed = Parse("is:closed", FacetDialect::GitHubSubset);
    REQUIRE(closed.Clauses.size() == 1);
    CHECK(closed.Clauses[0].Values == std::vector<std::string>{"Closed"});
    CHECK(ReplaceFacetClause("is:closed", FacetDialect::GitHubSubset, closed.Clauses[0].At, closed.Clauses[0].Len,
                             Clause("status", "Open")) == "status = \"Open\"");
}

TEST_CASE("query_facets: a GitHub clause the tracker drops is not a chip") {
    for (const char* q :
         {"assignee != \"bob\"", "status = \"Blocked\"", "labels in (\"a\", \"b\")", "status != \"Closed\""}) {
        CAPTURE(q);
        const FacetParse p = Parse(q, FacetDialect::GitHubSubset);
        REQUIRE(p.Chippable);
        CHECK(p.Clauses.empty());
        CHECK(p.OtherSpans.size() == 1);
        CHECK(p.LockedFieldIds.size() == 1);
    }
}

TEST_CASE("query_facets: bytes the GitHub translator skips make the query custom") {
    CHECK_FALSE(Parse("label:ui/ux", FacetDialect::GitHubSubset).Chippable);
    CHECK_FALSE(Parse("assignee = jos\xC3\xA9", FacetDialect::GitHubSubset).Chippable);
    CHECK(Parse("labels = \"ui/ux\"", FacetDialect::GitHubSubset).Chippable);
}

TEST_CASE("query_facets: quoting is per dialect") {
    CHECK(AddFacetClause("", FacetDialect::Jql, Clause("labels", "say \"hi\"")) == "labels = \"say \\\"hi\\\"\"");
    const FacetParse back = Parse("labels = \"say \\\"hi\\\"\"", FacetDialect::Jql);
    REQUIRE(back.Clauses.size() == 1);
    CHECK(back.Clauses[0].Values == std::vector<std::string>{"say \"hi\""});
    for (FacetDialect d : {FacetDialect::GitHubSubset, FacetDialect::LinearSubset}) {
        CHECK(AddFacetClause("", d, Clause("labels", "a\\b")) == "labels = \"a\\b\"");
        CHECK_FALSE(ValueSupported(d, "say \"hi\"", false));
        CHECK_FALSE(ValueSupported(d, "", false));
        CHECK(AddFacetClause("", d, Clause("labels", "say \"hi\"")).empty());
    }
    CHECK_FALSE(ValueSupported(FacetDialect::LinearSubset, "a,b", true));
    CHECK(ValueSupported(FacetDialect::LinearSubset, "a,b", false));
    CHECK(ValueSupported(FacetDialect::GitHubSubset, "a,b", true));
    CHECK(ValueSupported(FacetDialect::Jql, "a,\"b", true));
}

TEST_CASE("query_facets: several values write a list") {
    FacetClause c = Clause("status", "Todo");
    c.Values.push_back("In Progress");
    CHECK(AddFacetClause("", FacetDialect::LinearSubset, c) == "status in (\"Todo\", \"In Progress\")");
    c.Op = FacetOp::NotEq;
    CHECK(AddFacetClause("", FacetDialect::Jql, c) == "status not in (\"Todo\", \"In Progress\")");
    CHECK(AddFacetClause("", FacetDialect::GitHubSubset, c).empty());
    CHECK(AddFacetClause("x = 1", FacetDialect::Jql, FacetClause()) == "x = 1");
}

TEST_CASE("query_facets: DialectSupports follows what each picker writes") {
    CHECK_FALSE(DialectSupports(FacetDialect::GitHubSubset, "labels", FacetOp::In, 2));
    CHECK_FALSE(DialectSupports(FacetDialect::GitHubSubset, "assignee", FacetOp::NotEq, 1));
    CHECK_FALSE(DialectSupports(FacetDialect::GitHubSubset, "priority", FacetOp::Eq, 1));
    CHECK(DialectSupports(FacetDialect::GitHubSubset, "status", FacetOp::Eq, 1));
    CHECK(DialectSupports(FacetDialect::LinearSubset, "status", FacetOp::In, 2));
    CHECK_FALSE(DialectSupports(FacetDialect::LinearSubset, "assignee", FacetOp::In, 2));
    CHECK_FALSE(DialectSupports(FacetDialect::LinearSubset, "status", FacetOp::Eq, 2));
    CHECK_FALSE(DialectSupports(FacetDialect::Jql, "status", FacetOp::Eq, 0));
    CHECK_FALSE(DialectSupports(FacetDialect::Jql, "sprint", FacetOp::Eq, 1));
    CHECK(DialectSupports(FacetDialect::Jql, "fixVersion", FacetOp::NotIn, 3));
}

TEST_CASE("query_facets: every clause a picker may write is applied whole") {
    const FacetOp ops[] = {FacetOp::Eq, FacetOp::NotEq, FacetOp::In, FacetOp::NotIn};
    const char* const plain[] = {"bug",    "needs review",       "a and b", "x or y", "p in q",      "a != b",
                                 "(core)", "work order by hand", "it's",    "ui/ux",  "jos\xC3\xA9", "a\\b",
                                 "x,y"};
    const char* const named[] = {"Open", "Closed", "Blocked", "Urgent", "High", "Medium", "Low", "No priority", "P1"};
    for (FacetDialect d : {FacetDialect::GitHubSubset, FacetDialect::LinearSubset}) {
        for (const std::string& field : FacetFieldIds(d)) {
            std::vector<std::string> values(std::begin(plain), std::end(plain));
            values.insert(values.end(), std::begin(named), std::end(named));
            int written = 0;
            for (FacetOp op : ops) {
                const bool list = op == FacetOp::In || op == FacetOp::NotIn;
                for (const std::string& v : values) {
                    if (!DialectSupports(d, field, op, 1) || !ValueSupported(d, v, list)) {
                        continue;
                    }
                    FacetClause c = Clause(field.c_str(), v.c_str(), op);
                    const std::string q = AddFacetClause("", d, c);
                    CAPTURE(q);
                    if (q.empty()) {
                        continue; // outside the field's own names; the cases below pin which are
                    }
                    ++written;
                    CHECK(IgnoredClauseNote(q, d).empty());
                    const FacetParse p = Parse(q + " ORDER BY key", d);
                    REQUIRE(p.Clauses.size() == 1);
                    CHECK(p.Clauses[0].Values == c.Values);
                }
            }
            CAPTURE(field);
            CHECK(written > 0);
            if ((field == "assignee" || field == "reporter")) {
                const std::string q = AddFacetClause("", d, Me(field.c_str()));
                CAPTURE(q);
                CHECK(IgnoredClauseNote(q, d).empty());
                CHECK(Parse(q, d).Clauses.size() == 1);
            }
        }
    }
}

TEST_CASE("query_facets: a value the translator would drop is never written") {
    const FacetDialect gh = FacetDialect::GitHubSubset;
    const FacetDialect linear = FacetDialect::LinearSubset;
    CHECK(AddFacetClause("", gh, Clause("status", "Blocked")).empty());
    CHECK(AddFacetClause("", linear, Clause("priority", "P1")).empty());
    CHECK(AddFacetClause("", gh, Clause("status", "Closed")) == "status = \"Closed\"");
    CHECK(AddFacetClause("", linear, Clause("priority", "High")) == "priority = \"High\"");
    const std::string q = "status = \"Open\"";
    CHECK(ReplaceFacetClause(q, gh, 0, q.size(), Clause("status", "Blocked")) == q);
}

TEST_CASE("query_facets: a Jql value that a rewrite would change is not a chip") {
    for (const char* q : {"status in (EMPTY, Done)", "assignee in (empty, bob)", "fixVersion = NULL",
                          "labels in (\"a\\tb\", c)", "labels = 'a\\u0041'", "labels = a\\tb"}) {
        CAPTURE(q);
        const FacetParse p = Parse(q, FacetDialect::Jql);
        REQUIRE(p.Chippable);
        CHECK(p.Clauses.empty());
        REQUIRE(p.OtherSpans.size() == 1);
        CHECK(Text(q, p.OtherSpans[0]) == q);
        CHECK(p.LockedFieldIds.empty());
    }
    // Quoted, EMPTY is a name; the three plain escapes survive a rewrite.
    for (const char* q : {"status = \"EMPTY\"", "labels = \"a \\\"b\\\" \\\\ c\""}) {
        CAPTURE(q);
        const FacetParse p = Parse(q, FacetDialect::Jql);
        REQUIRE(p.Clauses.size() == 1);
        CHECK(ReplaceFacetClause(q, FacetDialect::Jql, p.Clauses[0].At, p.Clauses[0].Len, p.Clauses[0]) == q);
    }
}

TEST_CASE("query_facets: Jql type is the issue type") {
    const FacetParse p = Parse("type = \"Bug\"", FacetDialect::Jql);
    REQUIRE(p.Clauses.size() == 1);
    CHECK(p.Clauses[0].FieldId == "issuetype");
    CHECK(ReplaceFacetClause("type = \"Bug\"", FacetDialect::Jql, p.Clauses[0].At, p.Clauses[0].Len,
                             Clause("issuetype", "Story")) == "issuetype = \"Story\"");
    CHECK(CanonicalFacetField(FacetDialect::GitHubSubset, "type").empty());
    CHECK(CanonicalFacetField(FacetDialect::LinearSubset, "type").empty());
}

TEST_CASE("query_facets: two clauses on one field are chips on Jira, custom on Linear") {
    const char* const q = "status = \"Open\" AND status = \"Closed\"";
    CHECK(Parse(q, FacetDialect::Jql).Clauses.size() == 2);
    CHECK(Parse(q, FacetDialect::GitHubSubset).Clauses.size() == 2);
    CHECK_FALSE(Parse(q, FacetDialect::LinearSubset).Chippable);
    CHECK_FALSE(Parse("status = a AND state = b", FacetDialect::LinearSubset).Chippable);
    CHECK_FALSE(Parse("State = a AND STATUS = b", FacetDialect::LinearSubset).Chippable);
    CHECK_FALSE(Parse("team = a AND team = b", FacetDialect::LinearSubset).Chippable);
}

TEST_CASE("query_facets: aliases and letter case resolve to one facet id") {
    for (FacetDialect d : {FacetDialect::GitHubSubset, FacetDialect::LinearSubset}) {
        const FacetParse p = Parse("label = \"x\"", d);
        REQUIRE(p.Clauses.size() == 1);
        CHECK(p.Clauses[0].FieldId == "labels");
        CHECK(ReplaceFacetClause("label = \"x\"", d, p.Clauses[0].At, p.Clauses[0].Len, Clause("labels", "y")) ==
              "labels = \"y\"");
    }
    CHECK(CanonicalFacetField(FacetDialect::LinearSubset, "State") == "status");
    CHECK(CanonicalFacetField(FacetDialect::LinearSubset, "STATUS") == "status");
    CHECK(CanonicalFacetField(FacetDialect::LinearSubset, "creator") == "reporter");
    CHECK(CanonicalFacetField(FacetDialect::GitHubSubset, "author") == "reporter");
    CHECK(CanonicalFacetField(FacetDialect::GitHubSubset, "state").empty());
    CHECK(CanonicalFacetField(FacetDialect::Jql, "FIXVERSION") == "fixVersion");
    CHECK(CanonicalFacetField(FacetDialect::Jql, "label").empty());
    CHECK(CanonicalFacetField(FacetDialect::Jql, "state").empty());
    const FacetParse quotedField = Parse("\"status\" = a", FacetDialect::Jql);
    REQUIRE(quotedField.Clauses.size() == 1);
    CHECK(quotedField.Clauses[0].FieldId == "status");
}

TEST_CASE("query_facets: only the asked-for facet fields become chips") {
    const FacetParse p = ParseFacetClauses("status = a AND labels = b", FacetDialect::Jql, {"labels"});
    REQUIRE(p.Clauses.size() == 1);
    CHECK(p.Clauses[0].FieldId == "labels");
    CHECK(p.OtherSpans.size() == 1);
}

TEST_CASE("query_facets: IgnoredClauseNote speaks for the translators") {
    CHECK(IgnoredClauseNote("status = Open OR status = Done", FacetDialect::GitHubSubset).find("OR") !=
          std::string::npos);
    CHECK(IgnoredClauseNote("sprint = 5", FacetDialect::LinearSubset).find("unsupported field") != std::string::npos);
    for (FacetDialect d : kDialects) {
        CHECK(IgnoredClauseNote("ORDER BY updated DESC", d).empty());
        CHECK(IgnoredClauseNote("assignee = currentUser() ORDER BY updated", d).empty());
        CHECK(IgnoredClauseNote("", d).empty());
    }
    CHECK(IgnoredClauseNote("sprint = 5 OR x = 1", FacetDialect::Jql).empty());
    CHECK_FALSE(IgnoredClauseNote("labels = \"unclosed", FacetDialect::GitHubSubset).empty());
}
