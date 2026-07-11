#include "Ui/P4vLaunchArgQuotePure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using P4vLaunch::QuoteWinArgWidePure;

// Minimal portable re-implementation of the CommandLineToArgvW token parser
// for a SINGLE quoted-or-bare argument, used to prove the round-trip property:
// QuoteWinArgWidePure(s) must parse back to exactly {s}. Implements the same
// backslash/quote rules CommandLineToArgvW applies (post-argv[0]):
//   * 2N backslashes + " -> N backslashes, " toggles quoted state
//   * 2N+1 backslashes + " -> N backslashes + literal "
//   * backslashes not followed by " are literal
// Unquoted whitespace ends the argument; we assert the whole input is one arg.
std::vector<std::wstring> ParseCommandLineArgvW(const std::wstring& cmd) {
    std::vector<std::wstring> args;
    std::wstring cur;
    bool inArg = false;
    bool quoted = false;
    size_t i = 0;
    const size_t n = cmd.size();
    while (i < n) {
        const wchar_t c = cmd[i];
        if (!inArg && (c == L' ' || c == L'\t')) {
            ++i;
            continue;
        }
        inArg = true;
        if (c == L'\\') {
            size_t bs = 0;
            while (i < n && cmd[i] == L'\\') {
                ++bs;
                ++i;
            }
            if (i < n && cmd[i] == L'"') {
                cur.append(bs / 2, L'\\');
                if (bs % 2 == 0) {
                    quoted = !quoted;
                } else {
                    cur.push_back(L'"');
                }
                ++i;
            } else {
                cur.append(bs, L'\\');
            }
            continue;
        }
        if (c == L'"') {
            quoted = !quoted;
            ++i;
            continue;
        }
        if (!quoted && (c == L' ' || c == L'\t')) {
            args.push_back(cur);
            cur.clear();
            inArg = false;
            ++i;
            continue;
        }
        cur.push_back(c);
        ++i;
    }
    if (inArg) {
        args.push_back(cur);
    }
    return args;
}

void RequireRoundTrip(const std::wstring& original) {
    const std::wstring quoted = QuoteWinArgWidePure(original);
    const std::vector<std::wstring> parsed = ParseCommandLineArgvW(quoted);
    REQUIRE(parsed.size() == 1u);
    CHECK(parsed[0] == original);
}

} // namespace

TEST_SUITE("P4vLaunch::QuoteWinArgWidePure") {
    TEST_CASE("bare token without whitespace or quotes passes through verbatim") {
        CHECK(QuoteWinArgWidePure(L"//depot/foo.cpp") == L"//depot/foo.cpp");
        CHECK(QuoteWinArgWidePure(L"12345") == L"12345");
        // A trailing backslash on an UNQUOTED token is harmless (no wrap quote).
        CHECK(QuoteWinArgWidePure(L"C:\\path\\") == L"C:\\path\\");
    }

    TEST_CASE("whitespace forces quoting") { CHECK(QuoteWinArgWidePure(L"a b") == L"\"a b\""); }

    TEST_CASE("trailing backslash before a wrap quote is doubled (the injection bug)") {
        // Value ending in a single backslash, with a space to force quoting.
        // Expected: open-quote, content, backslash doubled, close-quote.
        CHECK(QuoteWinArgWidePure(L"C:\\my path\\") == L"\"C:\\my path\\\\\"");
        // Two trailing backslashes -> four before the closing quote.
        CHECK(QuoteWinArgWidePure(L"a \\\\") == L"\"a \\\\\\\\\"");
    }

    TEST_CASE("embedded double-quote is escaped") {
        // a"b (with a space to force quoting) -> "a\"b"
        CHECK(QuoteWinArgWidePure(L"a \"b") == L"\"a \\\"b\"");
    }

    TEST_CASE("backslash run before an embedded quote is 2N+1") {
        // a\" (space forces quoting): the single backslash before the quote
        // becomes 2*1+1 = 3 backslashes then \".
        CHECK(QuoteWinArgWidePure(L"a \\\"") == L"\"a \\\\\\\"\"");
    }

    TEST_CASE("CommandLineToArgvW round-trip — the security property") {
        // Each of these must parse back to exactly itself as a single argument.
        RequireRoundTrip(L"C:\\path with space\\"); // trailing backslash + space
        RequireRoundTrip(L"//depot/dir with space/f.cpp");
        RequireRoundTrip(L"value\\"); // bare trailing backslash
        RequireRoundTrip(L"a b\\");
        RequireRoundTrip(L"embedded \" quote");
        RequireRoundTrip(L"q\\\"x with space"); // backslash + quote + space
        RequireRoundTrip(L"trailing\\\\");      // two trailing backslashes (bare)
        RequireRoundTrip(L"a \\\\");            // two trailing backslashes (quoted)
        RequireRoundTrip(L"\" --inject /flag"); // attempted flag injection
        RequireRoundTrip(L"normal");
        RequireRoundTrip(L""); // empty argument (CPP_CODE_AUDIT.md #33)
    }

    TEST_CASE("empty input is wrapped as \"\" so it survives as its own argv slot") {
        // CPP_CODE_AUDIT.md #33: an empty string used to take the bare-token fast path
        // (no whitespace/quote to force wrapping) and return "" (zero characters) — but
        // CommandLineToArgvW requires the literal two-character `""` to preserve an
        // empty argument as a distinct slot; returning nothing makes the argument
        // vanish entirely once the caller joins args with spaces, silently shifting
        // every argument after it. P4vLaunch never passes an empty file/cl through this
        // helper on the direct-p4vc path today, but this is the exact bug class the
        // pure helper exists to prevent for any future non-trailing caller.
        CHECK(QuoteWinArgWidePure(L"") == L"\"\"");
    }
}

TEST_SUITE("P4vLaunch::P4vCustomCommandFieldRejected") {
    using P4vLaunch::P4vCustomCommandFieldRejected;

    TEST_CASE("safe field values pass") {
        CHECK_FALSE(P4vCustomCommandFieldRejected("//depot/foo.cpp"));
        CHECK_FALSE(P4vCustomCommandFieldRejected("C:\\src\\foo.cpp")); // interior backslashes are fine
        CHECK_FALSE(P4vCustomCommandFieldRejected("12345"));
        CHECK_FALSE(P4vCustomCommandFieldRejected(""));      // empty ends with no backslash
        CHECK_FALSE(P4vCustomCommandFieldRejected("a b c")); // spaces are the template author's concern, not injection
    }

    TEST_CASE("an embedded double-quote is rejected (wrap-quote closure / arg injection)") {
        CHECK(P4vCustomCommandFieldRejected("foo\"bar"));
        CHECK(P4vCustomCommandFieldRejected("\"")); // lone quote
        CHECK(P4vCustomCommandFieldRejected("//depot/a\" -flag b"));
    }

    TEST_CASE("a TRAILING backslash is rejected (#1712 — escapes the template's closing wrap quote)") {
        // A "..."-wrapped placeholder + a value ending in '\' -> `"foo\"` un-terminates the arg.
        CHECK(P4vCustomCommandFieldRejected("C:\\path\\"));
        CHECK(P4vCustomCommandFieldRejected("foo\\"));
        CHECK(P4vCustomCommandFieldRejected("\\")); // lone backslash
        // An INTERIOR backslash (not trailing) must NOT be rejected (no over-blocking).
        CHECK_FALSE(P4vCustomCommandFieldRejected("C:\\path\\file.cpp"));
    }
}
