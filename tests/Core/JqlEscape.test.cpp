#include <doctest/doctest.h>

#include "Tracker/JqlEscape.h"

#include <string>

using tracker_jql::QuoteLiteral;

// Build the full quoted literal the production callers emit: a leading `"`, the escaped
// inner content, and a trailing `"`. Mirrors how BuildJqlUserInsert / BuildKeyInJql /
// BuildActivityDiscoveryJql wrap QuoteLiteral's result.
static std::string Quoted(const std::string& raw) { return "\"" + QuoteLiteral(raw) + "\""; }

TEST_CASE("QuoteLiteral: leaves benign content untouched") {
    CHECK(QuoteLiteral("") == "");
    CHECK(QuoteLiteral("PROJ-123") == "PROJ-123");
    CHECK(QuoteLiteral("Ada Lovelace") == "Ada Lovelace");
    // A typical Jira Cloud accountId.
    CHECK(QuoteLiteral("5b10ac8d82e05b22cc7d4ef5") == "5b10ac8d82e05b22cc7d4ef5");
}

TEST_CASE("QuoteLiteral: escapes the double-quote that would break out of the literal") {
    // The H3 / E1 break-out payload: a `"` that, unescaped, would close the literal and
    // let the server append arbitrary JQL clauses.
    CHECK(QuoteLiteral("a\"b") == "a\\\"b");
    // Full break-out attempt from the audit: `FOO" OR project=SECRET OR key="BAR`.
    const std::string attack = "FOO\" OR project=SECRET OR key=\"BAR";
    const std::string escaped = QuoteLiteral(attack);
    // Every raw `"` becomes `\"`; none survives as a bare quote.
    CHECK(escaped == "FOO\\\" OR project=SECRET OR key=\\\"BAR");
}

TEST_CASE("QuoteLiteral: escapes the backslash first so an embedded escape can't be smuggled") {
    // Backslash must be doubled, otherwise `\"` in the input would yield a stray escape.
    CHECK(QuoteLiteral("a\\b") == "a\\\\b");
    // A `\"` in the input must become `\\\"` (escaped backslash + escaped quote), NOT `\"`
    // (which would still terminate the literal after one escape consumes the backslash).
    CHECK(QuoteLiteral("x\\\"y") == "x\\\\\\\"y");
}

TEST_CASE("QuoteLiteral: the wrapped literal is balanced (no unescaped closing quote)") {
    // Property: after wrapping, the only quote characters that are NOT preceded by a
    // backslash are the outermost opening + closing quotes. This is the invariant that
    // guarantees a server-supplied value cannot break out.
    auto noUnescapedInnerQuote = [](const std::string& raw) -> bool {
        const std::string lit = Quoted(raw);
        // Inner content is lit[1 .. size-2). Walk it; a `"` must always be escaped.
        bool prevBackslash = false;
        for (std::size_t i = 1; i + 1 < lit.size(); ++i) {
            const char c = lit[i];
            if (c == '"' && !prevBackslash) {
                return false; // unescaped inner quote — break-out possible
            }
            prevBackslash = (c == '\\') && !prevBackslash;
        }
        return true;
    };

    CHECK(noUnescapedInnerQuote("FOO\" OR project=SECRET OR key=\"BAR"));
    CHECK(noUnescapedInnerQuote("name with \" quote"));
    CHECK(noUnescapedInnerQuote("trailing backslash\\"));
    CHECK(noUnescapedInnerQuote("\\\"")); // escaped-quote payload
    CHECK(noUnescapedInnerQuote("plain"));

    // Concrete spot-checks of the produced literal.
    CHECK(Quoted("PROJ-1") == "\"PROJ-1\"");
    CHECK(Quoted("a\"b") == "\"a\\\"b\"");
}
