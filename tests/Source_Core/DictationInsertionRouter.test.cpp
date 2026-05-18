// DictationInsertionRouter — Phase A doctest coverage. See
// docs/design/whisper-dictation.md § Phase A verification.
//
// Both gating states (SMATCHET_WITH_WHISPER ON / OFF) must compile this TU.
// When ON, the real registry is exercised (Register/Unregister round-trip,
// multi-buffer independence, IsRecording() default false). When OFF, the
// stubs TU is linked and every Register/Unregister/Insert is a no-op — the
// test pivots to verifying that contract.
//
// Mirror of the Lua-stubs-compile sentinel (tests/Lua/LuaStubsCompile.test.cpp):
// build-failure in either configuration catches drift between the bindings
// and stubs TUs before runtime.

#include <doctest/doctest.h>

#include "DictationInsertionRouter.h"

#include <cstring>
#include <string>

#if SMATCHET_WITH_WHISPER

TEST_CASE("DictationInsertionRouter: default state has no registered buffers") {
    DictationInsertionRouter router;
    CHECK(router.RegisteredCountForTest() == 0u);
    CHECK_FALSE(router.IsRecording());
}

TEST_CASE("DictationInsertionRouter: register / unregister round-trip") {
    DictationInsertionRouter router;
    char buf[64] = {0};
    int cursor = 0;

    router.RegisterInputText(buf, sizeof(buf), &cursor);
    CHECK(router.RegisteredCountForTest() == 1u);

    router.UnregisterInputText(buf);
    CHECK(router.RegisteredCountForTest() == 0u);
}

TEST_CASE("DictationInsertionRouter: re-registering same buffer does not duplicate") {
    DictationInsertionRouter router;
    char buf[64] = {0};
    int cursor = 0;

    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.RegisterInputText(buf, sizeof(buf), &cursor);
    CHECK(router.RegisteredCountForTest() == 1u);
}

TEST_CASE("DictationInsertionRouter: multiple buffers register independently") {
    DictationInsertionRouter router;
    char a[32] = {0};
    char b[32] = {0};
    char c[32] = {0};
    int ca = 0;
    int cb = 0;
    int cc = 0;

    router.RegisterInputText(a, sizeof(a), &ca);
    router.RegisterInputText(b, sizeof(b), &cb);
    router.RegisterInputText(c, sizeof(c), &cc);
    CHECK(router.RegisteredCountForTest() == 3u);

    // Unregistering one leaves the others intact.
    router.UnregisterInputText(b);
    CHECK(router.RegisteredCountForTest() == 2u);

    router.UnregisterInputText(a);
    router.UnregisterInputText(c);
    CHECK(router.RegisteredCountForTest() == 0u);
}

TEST_CASE("DictationInsertionRouter: rejects null buffer / zero capacity") {
    DictationInsertionRouter router;
    int cursor = 0;
    char buf[16] = {0};

    router.RegisterInputText(nullptr, 32, &cursor);
    CHECK(router.RegisteredCountForTest() == 0u);

    router.RegisterInputText(buf, 0, &cursor);
    CHECK(router.RegisteredCountForTest() == 0u);
}

TEST_CASE("DictationInsertionRouter: unregister of unknown buffer is a no-op") {
    DictationInsertionRouter router;
    char known[16] = {0};
    char unknown[16] = {0};
    int cursor = 0;

    router.RegisterInputText(known, sizeof(known), &cursor);
    CHECK(router.RegisteredCountForTest() == 1u);

    router.UnregisterInputText(unknown);
    CHECK(router.RegisteredCountForTest() == 1u);

    router.UnregisterInputText(nullptr);
    CHECK(router.RegisteredCountForTest() == 1u);
}

TEST_CASE("DictationInsertionRouter: Insert with no registered target is a no-op") {
    DictationInsertionRouter router;
    // No registered buffer — Insert logs + drops without crashing.
    router.Insert("hello");
    CHECK(router.RegisteredCountForTest() == 0u);
}

TEST_CASE("DictationInsertionRouter: Insert appends to an empty buffer") {
    DictationInsertionRouter router;
    char buf[32] = {0};
    int cursor = 0;

    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.Insert("hello");
    CHECK(std::string(buf) == "hello");
    CHECK(cursor == 5);
}

TEST_CASE("DictationInsertionRouter: Insert appends after existing content when cursor is null") {
    DictationInsertionRouter router;
    char buf[32] = {0};
    std::strcpy(buf, "abc");

    // Pass nullptr cursor — Insert should land at end-of-content.
    router.RegisterInputText(buf, sizeof(buf), nullptr);
    router.Insert("123");
    CHECK(std::string(buf) == "abc123");
}

TEST_CASE("DictationInsertionRouter: Insert splices at explicit cursor mid-content") {
    DictationInsertionRouter router;
    char buf[32] = {0};
    std::strcpy(buf, "abcdef");
    int cursor = 3; // between "abc" and "def"

    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.Insert("XYZ");
    CHECK(std::string(buf) == "abcXYZdef");
    // Cursor advances by text.size() so successive Inserts chain naturally.
    CHECK(cursor == 6);
}

TEST_CASE("DictationInsertionRouter: Insert chains cursor advances across calls") {
    DictationInsertionRouter router;
    char buf[32] = {0};
    int cursor = 0;

    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.Insert("foo");
    CHECK(cursor == 3);
    router.Insert(" bar");
    CHECK(cursor == 7);
    CHECK(std::string(buf) == "foo bar");
}

TEST_CASE("DictationInsertionRouter: Insert truncates at capacity") {
    DictationInsertionRouter router;
    // Capacity 8 means at most 7 content bytes + terminator.
    char buf[8] = {0};
    int cursor = 0;

    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.Insert("0123456789");
    // Only 7 bytes fit.
    CHECK(std::string(buf) == "0123456");
    CHECK(cursor == 7);
}

TEST_CASE("DictationInsertionRouter: Insert preserves UTF-8 boundary on truncation") {
    DictationInsertionRouter router;
    // 8-byte buffer. "a\xC3\xA9" is 'a' + 'é' (2-byte UTF-8 sequence).
    // Insert text that, naively truncated, would split a multi-byte codepoint.
    // We expect the router to truncate to the last valid codepoint boundary.
    char buf[8] = {0};
    int cursor = 0;
    router.RegisterInputText(buf, sizeof(buf), &cursor);

    // "héllo wörld" — bytes: h(1) é(2) l(1) l(1) o(1) space(1) w(1) ö(2) r(1) l(1) d(1) = 13 bytes.
    // Capacity allows at most 7 content bytes. Naive truncation at 7 would land
    // mid-'ö' (if reached) or just after a single byte; ensure no lead-without-trail
    // pair survives. With buf size 8 the truncated content is "héllo " (6 bytes:
    // h + é(2) + l + l + o + space = 7), but available=7 limits to "héllo w" (8 bytes
    // including the 2-byte é). Wait: 1+2+1+1+1+1+1 = 8 → still ok at 7 if we drop the 'w'.
    // The point of this test is: NEVER end with a half codepoint.
    router.Insert("h\xC3\xA9llo w\xC3\xB6rld");
    const std::size_t len = std::strlen(buf);
    // Last byte must NOT be a UTF-8 continuation byte (0x80-0xBF) unless it is
    // part of a complete sequence — and similarly must NOT be a lead byte for an
    // incomplete sequence. The safest invariant: walking the bytes from the
    // start as UTF-8 must reach exactly `len` with no leftover lead.
    std::size_t i = 0;
    while (i < len) {
        const unsigned char b = static_cast<unsigned char>(buf[i]);
        std::size_t needed = 1;
        if ((b & 0x80u) == 0) {
            needed = 1;
        } else if ((b & 0xE0u) == 0xC0u) {
            needed = 2;
        } else if ((b & 0xF0u) == 0xE0u) {
            needed = 3;
        } else if ((b & 0xF8u) == 0xF0u) {
            needed = 4;
        } else {
            // Unexpected: continuation byte at code-point start.
            FAIL("UTF-8 continuation byte at code-point boundary");
        }
        CHECK(i + needed <= len);
        i += needed;
    }
    CHECK(i == len);
}

TEST_CASE("DictationInsertionRouter: re-register is idempotent for the focused-lookup set") {
    DictationInsertionRouter router;
    char buf[32] = {0};
    int cursor = 0;
    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.RegisterInputText(buf, sizeof(buf), &cursor);
    CHECK(router.RegisteredCountForTest() == 1u);
    router.UnregisterInputText(buf);
    CHECK(router.RegisteredCountForTest() == 0u);
}

TEST_CASE("DictationInsertionRouter: InsertIntoFocusedInputText with no target is a no-op") {
    DictationInsertionRouter router;
    char buf[32] = {0};
    // No buffer registered — call must not crash.
    router.InsertIntoFocusedInputText("hello");
    CHECK(buf[0] == '\0');
}

TEST_CASE("DictationInsertionRouter: InsertIntoFocusedInputText splices into the registered buf") {
    DictationInsertionRouter router;
    char buf[32] = {0};
    std::strcpy(buf, "pre-");
    router.RegisterInputText(buf, sizeof(buf), nullptr);
    router.InsertIntoFocusedInputText("typed");
    CHECK(std::string(buf) == "pre-typed");
}

TEST_CASE("DictationInsertionRouter: g_dictationRouter global is usable") {
    // The global lives in DictationInsertionRouter_Whisper.cpp / _Stubs.cpp.
    // This test ensures the link-time symbol is exported and the default
    // state matches a freshly-constructed instance — guards against accidental
    // omission of the global in either gating TU.
    char buf[16] = {0};
    g_dictationRouter.RegisterInputText(buf, sizeof(buf), nullptr);
    CHECK(g_dictationRouter.RegisteredCountForTest() >= 1u);
    g_dictationRouter.UnregisterInputText(buf);
}

TEST_CASE("DictationInsertionRouter::Insert: unterminated buffer is repaired, no overread") {
    // Regression gate for PR #246 — the BoundedStrlen fix. A registered
    // buffer that lacks a NUL within its capacity must NOT trigger
    // std::strlen overread; instead the router force-terminates at
    // cap-1, clamps `existing` to that, then performs the splice (which
    // here drops because existing+1 >= cap leaves zero room — the
    // important assertion is "no crash + buffer ends terminated").
    DictationInsertionRouter router;
    char buf[8];
    std::memset(buf, 'X', sizeof(buf)); // intentionally NO terminator anywhere.
    router.RegisterInputText(buf, sizeof(buf), nullptr);

    router.Insert("payload");

    // Last byte must be the repair terminator the router wrote in.
    CHECK(buf[sizeof(buf) - 1] == '\0');
    // First 7 bytes must remain untouched (the splice path bails because
    // existing == cap-1 leaves no room for new content).
    for (std::size_t i = 0; i < sizeof(buf) - 1; ++i) {
        CHECK(buf[i] == 'X');
    }

    router.UnregisterInputText(buf);
}

TEST_CASE("DictationInsertionRouter::InsertIntoFocusedInputText: unterminated buffer is repaired") {
    // Same regression gate as Insert above, exercised against the focused-
    // insertion path. Both call sites must use BoundedStrlen.
    DictationInsertionRouter router;
    char buf[8];
    std::memset(buf, 'Z', sizeof(buf)); // no NUL.
    router.RegisterInputText(buf, sizeof(buf), nullptr);

    router.InsertIntoFocusedInputText("payload");

    CHECK(buf[sizeof(buf) - 1] == '\0');
    for (std::size_t i = 0; i < sizeof(buf) - 1; ++i) {
        CHECK(buf[i] == 'Z');
    }

    router.UnregisterInputText(buf);
}

TEST_CASE("DictationInsertionRouter::Insert: normally-terminated buffer behaves identically post-fix") {
    // Sanity-check that the BoundedStrlen fallback does not regress the
    // common path — a buffer with a NUL well within capacity must still
    // splice as before.
    DictationInsertionRouter router;
    char buf[32] = {0};
    std::strcpy(buf, "hi ");
    router.RegisterInputText(buf, sizeof(buf), nullptr);

    router.Insert("there");

    CHECK(std::string(buf) == "hi there");
    router.UnregisterInputText(buf);
}

#else // SMATCHET_WITH_WHISPER not defined — stubs TU is linked.

TEST_CASE("DictationInsertionRouter stubs: every method is a no-op") {
    DictationInsertionRouter router;
    char buf[64] = {0};
    int cursor = 0;

    CHECK(router.RegisteredCountForTest() == 0u);
    CHECK_FALSE(router.IsRecording());

    // Stubs ignore Register/Unregister; RegisteredCountForTest stays 0.
    router.RegisterInputText(buf, sizeof(buf), &cursor);
    CHECK(router.RegisteredCountForTest() == 0u);

    router.UnregisterInputText(buf);
    CHECK(router.RegisteredCountForTest() == 0u);

    // Insert is a no-op; buf must remain untouched.
    router.Insert("ignored");
    CHECK(buf[0] == '\0');

    // InsertIntoFocusedInputText is also a no-op.
    router.InsertIntoFocusedInputText("ignored");
    CHECK(buf[0] == '\0');

    // The g_dictationRouter global must link in stub mode too (sentinel for
    // bindings/stubs drift — the standalone OFF build links this TU and the
    // four UI surfaces call g_dictationRouter unconditionally).
    g_dictationRouter.RegisterInputText(buf, sizeof(buf), &cursor);
    CHECK(g_dictationRouter.RegisteredCountForTest() == 0u);
}

#endif // SMATCHET_WITH_WHISPER
