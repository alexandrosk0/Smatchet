#include <doctest/doctest.h>

#include "StringUtil.h"

// ToLowerAsciiCopy / ToUpperAsciiCopy (inline in StringUtil.h) are the single ASCII-case
// helpers for the whole tree. NINE translation units depend on them: Logger,
// SmatchetLocalization, IssueTableSerializer, TrackerHttpPure, ModelDownloadPolicy and
// PlaneFieldCatalogPure call them directly, and AppController_LuaBindings,
// BuiltinCommands_Helpers and McpJsonRpcPure forward their own published names here.
//
// That fan-in is why these need direct coverage: a regression in this one body breaks
// all nine call sites at once — including two security-adjacent ones that compare
// LOWERCASED hosts (McpJsonRpcPure's origin/host allow-listing and
// ModelDownloadPolicy's download-domain check). Header-only, so there is no
// production .cpp to link.
//
// The `unsigned char` cast inside both helpers is the load-bearing detail: passing a
// negative `char` (any byte >= 0x80 on a signed-char platform) straight to
// std::tolower/std::toupper is UB, and the high-byte cases below are what would catch
// a future "simplification" that drops the cast.

TEST_CASE("ToLowerAsciiCopy — ASCII letters fold, everything else is preserved") {
    SUBCASE("upper-case ASCII folds") {
        CHECK(ToLowerAsciiCopy("HeLLo World") == "hello world");
        CHECK(ToLowerAsciiCopy("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == "abcdefghijklmnopqrstuvwxyz");
    }

    SUBCASE("digits, punctuation and separators pass through") {
        CHECK(ToLowerAsciiCopy("ABC-123_XYZ") == "abc-123_xyz");
        CHECK(ToLowerAsciiCopy("A.B/C:D?E#F") == "a.b/c:d?e#f");
    }

    SUBCASE("already-lower and empty input are identities") {
        CHECK(ToLowerAsciiCopy("already lower") == "already lower");
        CHECK(ToLowerAsciiCopy("") == "");
    }

    SUBCASE("idempotent") {
        const std::string once = ToLowerAsciiCopy("MiXeD CaSe 42");
        CHECK(ToLowerAsciiCopy(once) == once);
    }

    SUBCASE("high bytes are not mangled and do not trip the signed-char UB") {
        // 0xC3 0x9C is UTF-8 'Ü'. ASCII-only folding must leave both bytes intact —
        // this is a byte-wise helper, not a Unicode-aware one, and every call site
        // (host names, field ids, log levels) relies on that.
        const std::string utf8 = "\xC3\x9C-KEY";
        CHECK(ToLowerAsciiCopy(utf8) == "\xC3\x9C-key");
        // Embedded NUL must survive: std::string is not NUL-terminated-by-contract.
        const std::string withNul("A\0B", 3);
        const std::string lowered = ToLowerAsciiCopy(withNul);
        CHECK(lowered.size() == 3);
        CHECK(lowered == std::string("a\0b", 3));
    }
}

TEST_CASE("ToUpperAsciiCopy — the upper-case twin behaves symmetrically") {
    // Consumed by PlaneFieldCatalogPure's property_type normalisation, which compares
    // the folded result against upper-case literals ("TEXT", "DATETIME", ...).
    SUBCASE("lower-case ASCII folds") {
        CHECK(ToUpperAsciiCopy("HeLLo World") == "HELLO WORLD");
        CHECK(ToUpperAsciiCopy("abcdefghijklmnopqrstuvwxyz") == "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    }

    SUBCASE("digits, punctuation, already-upper and empty are preserved") {
        CHECK(ToUpperAsciiCopy("abc-123_xyz") == "ABC-123_XYZ");
        CHECK(ToUpperAsciiCopy("ALREADY UPPER") == "ALREADY UPPER");
        CHECK(ToUpperAsciiCopy("") == "");
    }

    SUBCASE("idempotent") {
        const std::string once = ToUpperAsciiCopy("MiXeD CaSe 42");
        CHECK(ToUpperAsciiCopy(once) == once);
    }

    SUBCASE("high bytes are not mangled and do not trip the signed-char UB") {
        const std::string utf8 = "\xC3\xBC-key";
        CHECK(ToUpperAsciiCopy(utf8) == "\xC3\xBC-KEY");
    }

    SUBCASE("round-trips with ToLowerAsciiCopy over pure ASCII") {
        const std::string s = "Round-Trip_123";
        CHECK(ToLowerAsciiCopy(ToUpperAsciiCopy(s)) == ToLowerAsciiCopy(s));
        CHECK(ToUpperAsciiCopy(ToLowerAsciiCopy(s)) == ToUpperAsciiCopy(s));
    }
}

TEST_CASE("ToLowerAsciiCopy — the per-TU bodies it replaced are behaviour-identical") {
    // Of the nine call sites now routed through this helper, eight always used this
    // std::tolower body; BuiltinCommands_Helpers instead used an explicit 'A'..'Z'
    // branch. The two forms agree for every byte under the "C" locale, which is the
    // only locale this process ever has (nothing in the tree calls std::setlocale) —
    // so routing that one through here is behaviour-preserving. This asserts the
    // equivalence across the whole byte range rather than trusting the argument.
    auto explicitAsciiLower = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
        return s;
    };
    for (int byte = 0; byte < 256; ++byte) {
        const std::string one(1, static_cast<char>(byte));
        CHECK(ToLowerAsciiCopy(one) == explicitAsciiLower(one));
    }
}
