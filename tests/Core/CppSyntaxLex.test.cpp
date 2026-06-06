// Bucket-A unit tests for the pure (ImGui-free) lexers extracted from
// CppSyntaxHighlight.cpp during the function-size decomposition. Goldens were
// captured from real runs of the linked lexer (see the GOLDEN-CAPTURE block in
// the PR) — never hand-computed from source. Where exact spans are awkward the
// asserts pin shape/invariants instead.

#include "Ui/CppSyntaxLex.h"

#include <doctest/doctest.h>

#include <cstring>
#include <string>

namespace {

// Reconstruct the lexed text from spans — every byte of the input must be
// covered exactly once, in order, by concatenating the spans. This is the
// load-bearing behaviour invariant: the draw layer emits spans back-to-back,
// so the spans must tile the input with no gaps and no overlaps.
std::string Reassemble(const char* line, const std::vector<CppLexToken>& toks) {
    std::string out;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        out.append(line + toks[i].offset, toks[i].length);
    }
    return out;
}

std::string ReassembleCs(const char* line, const std::vector<CallstackLexToken>& toks) {
    std::string out;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        out.append(line + toks[i].offset, toks[i].length);
    }
    return out;
}

// Assert spans are contiguous and ordered (offset[k] == offset[k-1]+len[k-1]).
template <typename Tok> void AssertTiling(const std::vector<Tok>& toks) {
    std::size_t expect = 0;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        CHECK(toks[i].offset == expect);
        CHECK(toks[i].length > 0); // lexer never emits empty spans
        expect = toks[i].offset + toks[i].length;
    }
}

} // namespace

TEST_CASE("CppLexIsKeyword recognises reserved words and rejects identifiers") {
    CHECK(CppLexIsKeyword("int"));
    CHECK(CppLexIsKeyword("class"));
    CHECK(CppLexIsKeyword("constexpr"));
    CHECK(CppLexIsKeyword("nullptr"));
    CHECK(CppLexIsKeyword("uint32_t"));
    CHECK(CppLexIsKeyword("size_t"));
    CHECK_FALSE(CppLexIsKeyword("foo"));
    CHECK_FALSE(CppLexIsKeyword("Int")); // case-sensitive
    CHECK_FALSE(CppLexIsKeyword(""));
    CHECK_FALSE(CppLexIsKeyword("intx"));
}

TEST_CASE("CppLexLine: empty / null input yields no tokens") {
    CHECK(CppLexLine(nullptr, 0).empty());
    const char* s = "";
    CHECK(CppLexLine(s, 0).empty());
}

TEST_CASE("CppLexLine: tiling invariant holds for varied input") {
    const char* samples[] = {
        "int x = 42;",     "  return foo(bar, 0xFF) + 3.14; // trailing", "#include \"CppSyntaxLex.h\"",
        "char c = '\\n';", "auto p = /* block */ ptr->member;",           "x86_64 += value123",
    };
    for (std::size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        const char* s = samples[i];
        const std::size_t n = std::strlen(s);
        const std::vector<CppLexToken> toks = CppLexLine(s, n);
        AssertTiling(toks);
        CHECK(Reassemble(s, toks) == std::string(s));
    }
}

TEST_CASE("CppLexLine: golden classification for a representative line") {
    // GOLDEN (captured from real run of CppLexLine): "int x = 42; // hi"
    // yields 10 spans — single chars are their own Default tokens, the line
    // comment runs to EOL.
    const char* s = "int x = 42; // hi";
    const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
    REQUIRE(t.size() == 10);
    CHECK(t[0].kind == CppLexKind::Keyword); // "int"
    CHECK(t[0].offset == 0);
    CHECK(t[0].length == 3);
    CHECK(t[1].kind == CppLexKind::Default);    // " "
    CHECK(t[2].kind == CppLexKind::Identifier); // "x"
    CHECK(t[3].kind == CppLexKind::Default);    // " "
    CHECK(t[4].kind == CppLexKind::Default);    // "="
    CHECK(t[5].kind == CppLexKind::Default);    // " "
    CHECK(t[6].kind == CppLexKind::Number);     // "42"
    CHECK(t[6].length == 2);
    CHECK(t[7].kind == CppLexKind::Default); // ";"
    CHECK(t[8].kind == CppLexKind::Default); // " "
    CHECK(t[9].kind == CppLexKind::Comment); // "// hi"
    CHECK(t[9].offset + t[9].length == std::strlen(s));
}

TEST_CASE("CppLexLine: line comment consumes to end of line") {
    const char* s = "a // b /* not nested */ c";
    const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
    // last token is the comment, runs to EOL
    REQUIRE_FALSE(t.empty());
    CHECK(t.back().kind == CppLexKind::Comment);
    CHECK(t.back().offset + t.back().length == std::strlen(s));
}

TEST_CASE("CppLexLine: block comment is one span, lexing resumes after") {
    const char* s = "a/*c*/b";
    const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
    REQUIRE(t.size() == 3);
    CHECK(t[0].kind == CppLexKind::Identifier); // a
    CHECK(t[1].kind == CppLexKind::Comment);    // /*c*/
    CHECK(std::string(s + t[1].offset, t[1].length) == "/*c*/");
    CHECK(t[2].kind == CppLexKind::Identifier); // b
}

TEST_CASE("CppLexLine: unterminated block comment runs to end") {
    const char* s = "x/*unterminated";
    const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
    REQUIRE(t.size() == 2);
    CHECK(t[1].kind == CppLexKind::Comment);
    CHECK(t[1].offset + t[1].length == std::strlen(s));
}

TEST_CASE("CppLexLine: string and char literals with escapes") {
    const char* s = "\"a\\\"b\" '\\''";
    const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
    AssertTiling(t);
    CHECK(t.front().kind == CppLexKind::String);
    CHECK(t.back().kind == CppLexKind::String);
}

TEST_CASE("CppLexLine: hex/float number literals") {
    const char* s = "0xFFu 3.14 12345L";
    const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
    REQUIRE(t.size() == 5);
    CHECK(t[0].kind == CppLexKind::Number);
    CHECK(std::string(s + t[0].offset, t[0].length) == "0xFFu");
    CHECK(t[2].kind == CppLexKind::Number);
    CHECK(std::string(s + t[2].offset, t[2].length) == "3.14");
    CHECK(t[4].kind == CppLexKind::Number);
    CHECK(std::string(s + t[4].offset, t[4].length) == "12345L");
}

TEST_CASE("CppLexLine: C++14 digit separators and case-insensitive suffixes (Issue #818)") {
    // Each sample's leading number literal must tokenise as one Number span
    // covering the whole literal — digit separators (') wedged between digits
    // and the full integer-suffix set (incl. uppercase U) are part of the token.
    struct Sample {
        const char* line;
        const char* expectNumber;
    };
    const Sample samples[] = {
        {"1'000", "1'000"},
        {"0xFF'FFU", "0xFF'FFU"},
        {"0b1010'0101", "0b1010'0101"},
        {"42U", "42U"},
        {"100UL", "100UL"},
        {"1'000'000ULL", "1'000'000ULL"},
        {"1'000'000", "1'000'000"},
        {"0xDEAD'BEEFull", "0xDEAD'BEEFull"},
    };
    for (std::size_t k = 0; k < sizeof(samples) / sizeof(samples[0]); ++k) {
        const char* s = samples[k].line;
        const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
        AssertTiling(t);
        CHECK(Reassemble(s, t) == std::string(s));
        REQUIRE_FALSE(t.empty());
        CHECK(t[0].kind == CppLexKind::Number);
        CHECK(std::string(s + t[0].offset, t[0].length) == std::string(samples[k].expectNumber));
    }
}

TEST_CASE("CppLexLine: stray quote is NOT swallowed into a number (Issue #818)") {
    // A trailing separator with no following digit stops the number; the ' is
    // left for the quote scanner. "1'" + non-digit must yield Number("1") then
    // a String/other token starting at the quote, never Number("1'...").
    {
        const char* s = "1'+2";
        const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
        AssertTiling(t);
        CHECK(Reassemble(s, t) == std::string(s));
        REQUIRE_FALSE(t.empty());
        CHECK(t[0].kind == CppLexKind::Number);
        CHECK(std::string(s + t[0].offset, t[0].length) == "1"); // just "1", quote not swallowed
    }
    {
        // A char literal adjacent to a number: 'a' is a String, never folded
        // into a neighbouring Number token.
        const char* s = "x = 'a'";
        const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
        AssertTiling(t);
        CHECK(Reassemble(s, t) == std::string(s));
        CHECK(t.back().kind == CppLexKind::String);
        CHECK(std::string(s + t.back().offset, t.back().length) == "'a'");
    }
    {
        // Bare separator between non-digits must not start/extend a number; the
        // leading char here is a digit but the quote is immediately doubled.
        const char* s = "1''0";
        const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
        AssertTiling(t);
        CHECK(Reassemble(s, t) == std::string(s));
        REQUIRE_FALSE(t.empty());
        CHECK(t[0].kind == CppLexKind::Number);
        CHECK(std::string(s + t[0].offset, t[0].length) == "1"); // doubled '' is not a separator
    }
}

TEST_CASE("CppLexLine: preprocessor directive run") {
    const char* s = "#define FOO 1";
    const std::vector<CppLexToken> t = CppLexLine(s, std::strlen(s));
    REQUIRE_FALSE(t.empty());
    CHECK(t.front().kind == CppLexKind::Preprocessor);
    // stops at '/' or '"' — here neither, so the whole '#define FOO 1' run.
    CHECK(std::string(s + t.front().offset, t.front().length) == "#define FOO 1");
}

TEST_CASE("CppLexLooksLikeCallstack: positive and negative shapes") {
    const char* yes1 = "Smatchet.exe!Foo::Bar(int) [C:\\x\\file.cpp:18]";
    CHECK(CppLexLooksLikeCallstack(yes1, std::strlen(yes1)));
    const char* yes2 = "mod!sym [frame]";
    CHECK(CppLexLooksLikeCallstack(yes2, std::strlen(yes2)));
    const char* no1 = "just a plain line";
    CHECK_FALSE(CppLexLooksLikeCallstack(no1, std::strlen(no1)));
    const char* no2 = "foo!bar"; // bang but no ( or [
    CHECK_FALSE(CppLexLooksLikeCallstack(no2, std::strlen(no2)));
    const char* no3 = "trailing!"; // bang at very end
    CHECK_FALSE(CppLexLooksLikeCallstack(no3, std::strlen(no3)));
}

TEST_CASE("CppLexCallstackLine: tiling invariant holds") {
    const char* s = "Smatchet.exe!ns::Foo::Bar(int a) [C:\\src\\file.cpp:18]";
    const std::vector<CallstackLexToken> t = CppLexCallstackLine(s, std::strlen(s));
    AssertTiling(t);
    CHECK(ReassembleCs(s, t) == std::string(s));
}

TEST_CASE("CppLexCallstackLine: golden span kinds for canonical frame") {
    // GOLDEN (captured from real run):
    //   "App.exe!Foo::Bar(int) [C:\\src\\file.cpp:18]"
    const char* s = "App.exe!Foo::Bar(int) [C:\\src\\file.cpp:18]";
    const std::vector<CallstackLexToken> t = CppLexCallstackLine(s, std::strlen(s));
    AssertTiling(t);
    CHECK(ReassembleCs(s, t) == std::string(s));

    // Module "App.exe" then "!" — both Default.
    CHECK(t[0].kind == CallstackLexKind::Default);
    CHECK(std::string(s + t[0].offset, t[0].length) == "App.exe");
    CHECK(t[1].kind == CallstackLexKind::Default);
    CHECK(std::string(s + t[1].offset, t[1].length) == "!");
    // "Foo" Class, "::" Default, "Bar" Method.
    CHECK(t[2].kind == CallstackLexKind::Class);
    CHECK(std::string(s + t[2].offset, t[2].length) == "Foo");
    CHECK(t[3].kind == CallstackLexKind::Default);
    CHECK(std::string(s + t[3].offset, t[3].length) == "::");
    CHECK(t[4].kind == CallstackLexKind::Method);
    CHECK(std::string(s + t[4].offset, t[4].length) == "Bar");

    // Somewhere a Filename span equals "file.cpp" and a LineNum equals ":18".
    bool sawFilename = false;
    bool sawLineNum = false;
    bool sawPath = false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const std::string piece(s + t[i].offset, t[i].length);
        if (t[i].kind == CallstackLexKind::Filename && piece == "file.cpp") {
            sawFilename = true;
        }
        if (t[i].kind == CallstackLexKind::LineNum && piece == ":18") {
            sawLineNum = true;
        }
        if (t[i].kind == CallstackLexKind::Path && piece == "C:\\src\\") {
            sawPath = true;
        }
    }
    CHECK(sawFilename);
    CHECK(sawLineNum);
    CHECK(sawPath);
}

TEST_CASE("CppLexCallstackLine: final segment without paren is Class not Method") {
    // No '(' after the symbol chain — final segment treated as class-like.
    // GOLDEN (captured from real run): the chain runs up to the '[', so the
    // final Class segment is "ntdll " (trailing space included — matches the
    // pre-refactor draw helper's chainEnd-at-bracket behaviour byte-for-byte).
    const char* s = "kernel32!ntdll [x]";
    const std::vector<CallstackLexToken> t = CppLexCallstackLine(s, std::strlen(s));
    AssertTiling(t);
    CHECK(ReassembleCs(s, t) == std::string(s));
    REQUIRE(t.size() == 6);
    CHECK(t[0].kind == CallstackLexKind::Default); // "kernel32"
    CHECK(std::string(s + t[0].offset, t[0].length) == "kernel32");
    CHECK(t[1].kind == CallstackLexKind::Default); // "!"
    CHECK(t[2].kind == CallstackLexKind::Class);   // "ntdll " (incl. trailing space)
    CHECK(std::string(s + t[2].offset, t[2].length) == "ntdll ");
    CHECK(t[3].kind == CallstackLexKind::Default);  // "["
    CHECK(t[4].kind == CallstackLexKind::Filename); // "x"
    CHECK(t[5].kind == CallstackLexKind::Default);  // "]"
}

TEST_CASE("CppLexCallstackLine: empty input yields no tokens") {
    CHECK(CppLexCallstackLine(nullptr, 0).empty());
    const char* s = "";
    CHECK(CppLexCallstackLine(s, 0).empty());
}
