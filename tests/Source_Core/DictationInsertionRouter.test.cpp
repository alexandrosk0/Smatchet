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

#if defined(SMATCHET_WITH_WHISPER)

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

TEST_CASE("DictationInsertionRouter: Insert is non-crashing scaffold") {
    DictationInsertionRouter router;
    char buf[32] = {0};
    int cursor = 0;

    // Insert with no registered target — scaffold logs + drops.
    router.Insert("hello");

    router.RegisterInputText(buf, sizeof(buf), &cursor);
    router.Insert("world");
    // Phase A: scaffold does not yet mutate buf. Full UTF-8 splice lands Phase D.
    // Asserting that buf is still zeroed locks the scaffold contract — any future
    // wire-up that touches buf in Phase A would fail this CHECK.
    CHECK(buf[0] == '\0');
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
}

#endif // SMATCHET_WITH_WHISPER
