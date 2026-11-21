#include <doctest/doctest.h>

#include "StringUtil.h"

#include <cctype>
#include <string>

// TrimCopyIsspace (inline in StringUtil.h) replaced seven byte-identical per-TU trims: TrackerHttpPure's
// host trim, JqlChangedSincePure, LinearQueryFromJql, IssueTableSerializer, both CompactDateFormat halves
// and the Whisper hotkey parser. They all trimmed with std::isspace, which also strips \v and \f, so the
// helper keeps exactly that set rather than folding onto TrimCopyAsciiWhitespace (space, tab, CR, LF).
// TrackerHttpPure trims a host for allow-listing, so narrowing the set there would change what it accepts.

TEST_CASE("TrimCopyIsspace trims the std::isspace set from both ends and nothing else") {
    CHECK(TrimCopyIsspace("") == "");
    CHECK(TrimCopyIsspace(" \t\r\n\v\f") == "");
    CHECK(TrimCopyIsspace("\v\f host.example \t") == "host.example");
    CHECK(TrimCopyIsspace("a b") == "a b");
    CHECK(TrimCopyIsspace("  inner  space  ") == "inner  space");
    CHECK(TrimCopyIsspace("x") == "x");
}

TEST_CASE("TrimCopyIsspace differs from TrimCopyAsciiWhitespace only on \\v and \\f") {
    for (int byte = 0; byte < 256; ++byte) {
        const char c = static_cast<char>(byte);
        const std::string padded = std::string(1, c) + "x" + std::string(1, c);
        const bool isspaceTrims = std::isspace(static_cast<unsigned char>(c)) != 0;
        CHECK(TrimCopyIsspace(padded) == (isspaceTrims ? std::string("x") : padded));
        if (c != '\v' && c != '\f') {
            CHECK(TrimCopyIsspace(padded) == TrimCopyAsciiWhitespace(padded));
        }
    }
}

TEST_CASE("TruncateUtf8WithEllipsis cuts on a character boundary") {
    CHECK(TruncateUtf8WithEllipsis("short", 160) == "short");
    CHECK(TruncateUtf8WithEllipsis("abcdef", 3) == "abc...");
    // E-acute is two bytes (C3 A9); a cut at byte 3 would land inside the second one.
    const std::string text = "a"
                             "\xC3\xA9"
                             "\xC3\xA9"
                             "\xC3\xA9";
    CHECK(TruncateUtf8WithEllipsis(text, 3) == "a"
                                               "\xC3\xA9"
                                               "...");
    CHECK(TruncateUtf8WithEllipsis(text, 2) == "a...");
    // A four-byte emoji (F0 9F 98 80) is dropped whole, never left as a stub.
    CHECK(TruncateUtf8WithEllipsis(std::string("ab") + "\xF0\x9F\x98\x80", 4) == "ab...");
    CHECK(TruncateUtf8WithEllipsis("abc", 0) == "...");
}
