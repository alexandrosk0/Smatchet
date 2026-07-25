#include <doctest/doctest.h>

#include "StringUtil.h"

// ToLowerAsciiCopy / ToUpperAsciiCopy (inline in StringUtil.h) are the ASCII-case
// helpers that gate-blind-spot-sweep Slice 2 folded NINE re-rolled per-TU copies onto
// (Logger, SmatchetLocalization, IssueTableSerializer, TrackerHttpPure,
// ModelDownloadPolicy, PlaneFieldCatalogPure, plus forwarders in
// AppController_LuaBindings, BuiltinCommands_Helpers and McpJsonRpcPure).
//
// That consolidation is exactly why these need direct coverage: before it, a
// regression in any one copy broke one TU. Now a regression here breaks nine at
// once — including two security-adjacent callers (McpJsonRpcPure's origin/host
// allow-listing and ModelDownloadPolicy's download-domain check), which compare
// LOWERCASED hosts. Header-only, so there is no production .cpp to link.
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
    // Added by Slice 2 for PlaneFieldCatalogPure's property_type normalisation, which
    // compares against upper-case literals ("TEXT", "DATETIME", ...).
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
    // Slice 2 folded nine copies onto one helper. Eight were the same std::tolower
    // body; BuiltinCommands_Helpers' was the odd one out — an explicit 'A'..'Z'
    // branch. The two agree for every byte under the "C" locale, which is the only
    // locale this process ever has (nothing in the tree calls std::setlocale). This
    // asserts that equivalence across the whole byte range rather than trusting it,
    // so the substitution is pinned rather than merely argued.
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
