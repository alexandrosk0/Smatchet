#include <doctest/doctest.h>

#include "Tracker/JqlQueryValidatePure.h"

#include <string>
#include <vector>

using namespace JqlQueryValidate;
using TrackerParseExplain::ParseMessage;
using TrackerParseExplain::ParseMessageId;

namespace {

std::vector<TrackerField> Catalog() {
    std::vector<TrackerField> fields;
    for (const char* id : {"assignee", "customfield_10010", "storypoints"}) {
        TrackerField f;
        f.Id = id;
        f.Name = id;
        fields.push_back(f);
    }
    fields[1].Name = "Epic Link";
    return fields;
}

ParseMessage Jql(const char* text) {
    const std::vector<TrackerField> fields = Catalog();
    return ValidateQuery(text, QueryDialect::Jql, &fields);
}

} // namespace

TEST_CASE("ValidateQuery accepts well-formed queries") {
    const char* kOk[] = {
        "",
        "   ",
        "project = ABC",
        "project = ABC AND status = \"In Progress\"",
        "assignee = currentUser() ORDER BY created DESC",
        "(status = Open OR status = Reopened) AND assignee is EMPTY",
        "status in (Open, \"In Progress\", Done)",
        "status not in (Done)",
        "summary ~ \"it's\"",
        "summary ~ 'say \"hi\"'",
        "text ~ \"a \\\" quote\"",
        "status changed",
        "status changed from Open to Done",
        "resolution is not EMPTY",
        "updated >= -7d",
        "\"Epic Link\" = ABC-1",
        "cf[10010] = 5",
        "labels = été",
        "ORDER BY created",
        "key = ABC-1",
    };
    for (const char* q : kOk) {
        INFO("query: " << std::string(q));
        CHECK_FALSE(Jql(q).IsError());
    }
}

TEST_CASE("ValidateQuery refuses unbalanced parentheses and says which one") {
    const ParseMessage open = Jql("(status = Open AND (assignee = x)");
    CHECK(open.Id == ParseMessageId::QueryUnbalancedOpen);
    CHECK(open.At == 0); // the outer ( that never closes
    CHECK(open.Len == 1);

    const ParseMessage close = Jql("status = Open)");
    CHECK(close.Id == ParseMessageId::QueryUnbalancedClose);
    CHECK(close.At == 13);

    CHECK(Jql("((a = 1)").Id == ParseMessageId::QueryUnbalancedOpen);
    CHECK(Jql(")(").Id == ParseMessageId::QueryUnbalancedClose);
    // A parenthesis inside a quoted value is text, not structure.
    CHECK_FALSE(Jql("summary ~ \"fix (again\"").IsError());
    CHECK_FALSE(Jql("summary ~ \"oops)\"").IsError());
}

TEST_CASE("ValidateQuery refuses a quote that is never closed") {
    const ParseMessage dq = Jql("summary ~ \"login crash");
    CHECK(dq.Id == ParseMessageId::QueryUnclosedQuote);
    CHECK(dq.Arg == "\"");
    CHECK(dq.At == 10);
    const ParseMessage sq = Jql("summary ~ 'login");
    CHECK(sq.Id == ParseMessageId::QueryUnclosedQuote);
    CHECK(sq.Arg == "'");
    // An escaped quote does not close the string.
    CHECK(Jql("summary ~ \"a \\\"").Id == ParseMessageId::QueryUnclosedQuote);
    // The other quote kind inside a string is just text.
    CHECK_FALSE(Jql("summary ~ \"it's\"").IsError());
}

TEST_CASE("ValidateQuery refuses a query that ends on an operator or a keyword that needs more") {
    struct Case {
        const char* Query;
        const char* Arg;
    };
    const Case kDangling[] = {
        {"status =", "="},
        {"status !=", "!="},
        {"updated >=", ">="},
        {"summary ~", "~"},
        {"status = Open AND", "AND"},
        {"status = Open and", "AND"},
        {"status = Open OR", "OR"},
        {"status is", "IS"},
        {"status is not", "NOT"},
        {"status in", "IN"},
        {"labels in Open,", ","},
        {"ORDER BY", "ORDER BY"},
        {"status = Open ORDER", "ORDER"},
        {"status was", "WAS"},
        {"status changed from", "FROM"},
    };
    for (const Case& c : kDangling) {
        INFO("query: " << std::string(c.Query));
        const ParseMessage m = Jql(c.Query);
        CHECK(m.Id == ParseMessageId::QueryDanglingOperator);
        CHECK(m.Arg == c.Arg);
        CHECK(IsBlockingQueryError(m));
    }
    // An open parenthesis is reported before the operator that follows it.
    CHECK(Jql("(status =").Id == ParseMessageId::QueryUnbalancedOpen);
    CHECK(Jql("status in (Open,").Id == ParseMessageId::QueryUnbalancedOpen);
}

TEST_CASE("ValidateQuery: an unknown field is a warning against the catalog, not a blocker") {
    const ParseMessage m = Jql("projct = ABC");
    CHECK(m.Id == ParseMessageId::QueryUnknownField);
    CHECK(m.Arg == "projct");
    CHECK(m.At == 0);
    CHECK_FALSE(IsBlockingQueryError(m));

    CHECK(Jql("status = Open AND storypoint > 3").Id == ParseMessageId::QueryUnknownField);
    CHECK(Jql("(foo is EMPTY)").Id == ParseMessageId::QueryUnknownField);
    CHECK(Jql("foo not in (a)").Id == ParseMessageId::QueryUnknownField);

    // Known by id, by display name (any case), or as a built-in the catalog may not list.
    CHECK_FALSE(Jql("assignee = x").IsError());
    CHECK_FALSE(Jql("ASSIGNEE = x").IsError());
    CHECK_FALSE(Jql("customfield_10010 = x").IsError());
    CHECK_FALSE(Jql("project = ABC").IsError());
    CHECK_FALSE(Jql("fixVersion = 1.2").IsError());
    CHECK_FALSE(Jql("cf[10010] = x").IsError());
    // Only a word that leads a clause is checked: a bare word is a value and the tracker knows it.
    CHECK_FALSE(Jql("status = Open AND assignee = zzz").IsError());
    CHECK_FALSE(Jql("summary ~ nonsense").IsError());
    CHECK_FALSE(Jql("status = Open ORDER BY whatever").IsError());
}

TEST_CASE("ValidateQuery skips the field check without a catalog, and for the field:value dialect") {
    CHECK_FALSE(ValidateQuery("projct = ABC", QueryDialect::Jql, nullptr).IsError());
    const std::vector<TrackerField> none;
    CHECK_FALSE(ValidateQuery("projct = ABC", QueryDialect::Jql, &none).IsError());
    const std::vector<TrackerField> fields = Catalog();
    CHECK_FALSE(ValidateQuery("projct = ABC", QueryDialect::FieldValue, &fields).IsError());
    // Structure is still checked in every dialect.
    CHECK(ValidateQuery("state:open AND (label:bug", QueryDialect::FieldValue, &fields).Id ==
          ParseMessageId::QueryUnbalancedOpen);
    CHECK(ValidateQuery("label:\"bug", QueryDialect::FieldValue, nullptr).Id == ParseMessageId::QueryUnclosedQuote);
    CHECK_FALSE(ValidateQuery("state:open label:bug", QueryDialect::FieldValue, nullptr).IsError());
}

TEST_CASE("ValidateQuery: in the field:value dialects an apostrophe and a trailing 'in' are plain text") {
    // GitHub, Plane and Linear searches carry no JQL keywords: these were refused before.
    for (const char* q : {"is:open sign in", "state:done order", "is:open label:bug can't login", "label:bug by",
                          "is:pr or", "is:open 'tis broken", "is:open login crash!", "label:bug urgent~"}) {
        INFO("query: " << std::string(q));
        CHECK_FALSE(ValidateQuery(q, QueryDialect::FieldValue, nullptr).IsError());
    }
    // The same text is still a JQL fault: there the words are keywords and the apostrophe opens a string.
    CHECK(ValidateQuery("status = Open and", QueryDialect::Jql, nullptr).Id == ParseMessageId::QueryDanglingOperator);
    CHECK(ValidateQuery("summary ~ can't login", QueryDialect::Jql, nullptr).Id == ParseMessageId::QueryUnclosedQuote);
    CHECK(ValidateQuery("summary ~ 'tis broken", QueryDialect::Jql, nullptr).Id == ParseMessageId::QueryUnclosedQuote);
    // What is structural stays structural in every dialect.
    CHECK(ValidateQuery("label:\"bug", QueryDialect::FieldValue, nullptr).Id == ParseMessageId::QueryUnclosedQuote);
    CHECK(ValidateQuery("state:open (label:bug", QueryDialect::FieldValue, nullptr).Id ==
          ParseMessageId::QueryUnbalancedOpen);
    CHECK(ValidateQuery("state =", QueryDialect::FieldValue, nullptr).Id == ParseMessageId::QueryDanglingOperator);
    // In JQL a trailing "!" is an unfinished "!=": the same text there is refused.
    CHECK(ValidateQuery("status crash!", QueryDialect::Jql, nullptr).Id == ParseMessageId::QueryDanglingOperator);
}

TEST_CASE("IsBlockingQueryError: structural problems block, everything else does not") {
    for (ParseMessageId id : {ParseMessageId::QueryUnbalancedOpen, ParseMessageId::QueryUnbalancedClose,
                              ParseMessageId::QueryUnclosedQuote, ParseMessageId::QueryDanglingOperator}) {
        ParseMessage m;
        m.Id = id;
        CHECK(IsBlockingQueryError(m));
    }
    for (ParseMessageId id : {ParseMessageId::None, ParseMessageId::QueryUnknownField, ParseMessageId::DurationNegative,
                              ParseMessageId::DateUnrecognised}) {
        ParseMessage m;
        m.Id = id;
        CHECK_FALSE(IsBlockingQueryError(m));
    }
}

TEST_CASE("ValidateQuery survives hostile input without throwing") {
    const std::string wall(20000, '(');
    CHECK_NOTHROW(ValidateQuery(wall, QueryDialect::Jql, nullptr));
    CHECK_FALSE(ValidateQuery(wall, QueryDialect::Jql, nullptr).IsError()); // over the scan cap: left to the tracker
    const std::string deep = std::string(3000, '(') + std::string(3000, ')');
    CHECK_NOTHROW(ValidateQuery(deep, QueryDialect::Jql, nullptr));
    CHECK_FALSE(ValidateQuery(deep, QueryDialect::Jql, nullptr).IsError());
    CHECK_NOTHROW(ValidateQuery(std::string("\xff\xfe \"\x80", 5), QueryDialect::Jql, nullptr));
    CHECK_NOTHROW(ValidateQuery(std::string("a\0b = ", 6), QueryDialect::Jql, nullptr));
    CHECK_NOTHROW(ValidateQuery(std::string(7000, '"'), QueryDialect::Jql, nullptr));
    CHECK(ValidateQuery("\"", QueryDialect::Jql, nullptr).Id == ParseMessageId::QueryUnclosedQuote);
}
