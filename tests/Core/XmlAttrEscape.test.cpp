// Pure-logic coverage for smatchet::ai::pure::EscapeXmlAttr (#826), extracted
// from AiAssistantController.h into the standalone pure header
// "AiXmlAttrEscape.h" so the doctest rig can exercise it without the controller
// TU's AppController / cpr / Ui-session dependency chain.
//
// Contract: XML-attribute-escape a context-block name before it is interpolated
// into `<smatchet_context block="...">`. `&` is replaced FIRST so the entity-
// introducing ampersands emitted for `"`/`<`/`>` are not themselves re-escaped.
// `&`->`&amp;`, `"`->`&quot;`, `<`->`&lt;`, `>`->`&gt;`.

#include "AiXmlAttrEscape.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::ai::pure::EscapeXmlAttr;

TEST_CASE("EscapeXmlAttr: empty input yields empty output") { CHECK(EscapeXmlAttr(std::string()) == std::string()); }

TEST_CASE("EscapeXmlAttr: no escapable chars is byte-identical") {
    SUBCASE("plain alphanumeric") { CHECK(EscapeXmlAttr("BlockName123") == "BlockName123"); }
    SUBCASE("punctuation that is not escaped passes through") {
        // ' (apostrophe), space, slash, equals, colon are not in the escape set.
        CHECK(EscapeXmlAttr("a b/c=d:e'f") == "a b/c=d:e'f");
    }
}

TEST_CASE("EscapeXmlAttr: each escapable char individually") {
    SUBCASE("ampersand") { CHECK(EscapeXmlAttr("&") == "&amp;"); }
    SUBCASE("double quote") { CHECK(EscapeXmlAttr("\"") == "&quot;"); }
    SUBCASE("less-than") { CHECK(EscapeXmlAttr("<") == "&lt;"); }
    SUBCASE("greater-than") { CHECK(EscapeXmlAttr(">") == "&gt;"); }
}

TEST_CASE("EscapeXmlAttr: ampersand is escaped FIRST, never double-escaped") {
    // The load-bearing ordering invariant: `a&b` must become `a&amp;b`, NOT
    // `a&amp;amp;b`. A naive "escape & after the others" pass would re-escape
    // the ampersand that `&quot;`/`&lt;`/`&gt;` introduce.
    CHECK(EscapeXmlAttr("a&b") == "a&amp;b");
    // An input that already looks like an entity is escaped literally (the
    // function does not try to detect pre-existing entities).
    CHECK(EscapeXmlAttr("&amp;") == "&amp;amp;");
    CHECK(EscapeXmlAttr("&lt;") == "&amp;lt;");
}

TEST_CASE("EscapeXmlAttr: combined href-style name escapes all classes") {
    // `<a href="x&y">` exercises <, >, ", and & in one pass.
    CHECK(EscapeXmlAttr("<a href=\"x&y\">") == "&lt;a href=&quot;x&amp;y&quot;&gt;");
}

TEST_CASE("EscapeXmlAttr: multiple occurrences of the same char all escape") {
    CHECK(EscapeXmlAttr("&&&") == "&amp;&amp;&amp;");
    CHECK(EscapeXmlAttr("<<>>") == "&lt;&lt;&gt;&gt;");
    CHECK(EscapeXmlAttr("\"\"") == "&quot;&quot;");
}

TEST_CASE("EscapeXmlAttr: escapable chars interleaved with passthrough text") {
    CHECK(EscapeXmlAttr("pre&mid<post") == "pre&amp;mid&lt;post");
    CHECK(EscapeXmlAttr("name=\"v\" & flag") == "name=&quot;v&quot; &amp; flag");
}
