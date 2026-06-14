// Pure-logic tests for the rebindable-shortcut hotkey grammar:
// ParseImGuiHotkey / StringifyImGuiHotkey round-trip + FindShortcutConflict.
// docs/plans/active/keyboard-shortcuts-rebindable.md (PR1 foundation).
// These three functions are ImGui-context-free (they only read the ImGuiKey
// enum); MatchHotkey is excluded here — it needs a live ImGui IO frame and is
// exercised by the bucket-E UI pass, not the doctest rig.

#include <doctest/doctest.h>

#include "Ui/ImGuiHotkey.h"

#include <string>
#include <vector>

using smatchet::ui::FindShortcutConflict;
using smatchet::ui::ImGuiBugHotkey;
using smatchet::ui::ParseImGuiHotkey;
using smatchet::ui::StringifyImGuiHotkey;

TEST_CASE("ParseImGuiHotkey: modifiers + letter key") {
    ImGuiBugHotkey hk;
    REQUIRE(ParseImGuiHotkey("Ctrl+Shift+B", hk));
    CHECK(hk.ctrl);
    CHECK(hk.shift);
    CHECK_FALSE(hk.alt);
    CHECK_FALSE(hk.super);
    CHECK(hk.key == ImGuiKey_B);
}

TEST_CASE("ParseImGuiHotkey: case-insensitive + whitespace tolerant") {
    ImGuiBugHotkey a;
    ImGuiBugHotkey b;
    REQUIRE(ParseImGuiHotkey("ctrl+b", a));
    REQUIRE(ParseImGuiHotkey("  CTRL +  B ", b));
    CHECK(a.ctrl);
    CHECK(a.key == ImGuiKey_B);
    CHECK(b.ctrl);
    CHECK(b.key == ImGuiKey_B);
}

TEST_CASE("ParseImGuiHotkey: control alias + super/win/cmd all set super") {
    ImGuiBugHotkey ctl;
    REQUIRE(ParseImGuiHotkey("Control+A", ctl));
    CHECK(ctl.ctrl);
    CHECK(ctl.key == ImGuiKey_A);

    const char* superSpecs[] = {"Super+S", "Win+S", "Cmd+S"};
    for (const char* spec : superSpecs) {
        ImGuiBugHotkey hk;
        REQUIRE(ParseImGuiHotkey(spec, hk));
        CHECK(hk.super);
        CHECK(hk.key == ImGuiKey_S);
    }
}

TEST_CASE("ParseImGuiHotkey: digit / function / named keys") {
    ImGuiBugHotkey hk;

    REQUIRE(ParseImGuiHotkey("Ctrl+0", hk));
    CHECK(hk.key == ImGuiKey_0);

    REQUIRE(ParseImGuiHotkey("F11", hk));
    CHECK(hk.key == ImGuiKey_F11);
    CHECK_FALSE(hk.ctrl);

    REQUIRE(ParseImGuiHotkey("F1", hk));
    CHECK(hk.key == ImGuiKey_F1);

    REQUIRE(ParseImGuiHotkey("F12", hk));
    CHECK(hk.key == ImGuiKey_F12);

    REQUIRE(ParseImGuiHotkey("Ctrl+,", hk));
    CHECK(hk.key == ImGuiKey_Comma);

    REQUIRE(ParseImGuiHotkey("Space", hk));
    CHECK(hk.key == ImGuiKey_Space);

    REQUIRE(ParseImGuiHotkey("Enter", hk));
    CHECK(hk.key == ImGuiKey_Enter);

    REQUIRE(ParseImGuiHotkey("Return", hk));
    CHECK(hk.key == ImGuiKey_Enter); // "return" is an alias for Enter
}

TEST_CASE("ParseImGuiHotkey: failure cases leave a reset struct") {
    ImGuiBugHotkey hk;
    CHECK_FALSE(ParseImGuiHotkey("", hk));        // empty
    CHECK_FALSE(ParseImGuiHotkey("Ctrl+Shift", hk)); // modifiers only, no main key
    CHECK_FALSE(ParseImGuiHotkey("F13", hk));     // F-key out of 1..12 range
    CHECK_FALSE(ParseImGuiHotkey("F0", hk));
    CHECK(hk.key == ImGuiKey_None); // last attempt reset out
    // ...and every modifier too: an earlier "Ctrl+Shift" parse dirtied ctrl/shift,
    // so these guard that a failed parse fully resets the struct (not just .key).
    CHECK_FALSE(hk.ctrl);
    CHECK_FALSE(hk.shift);
    CHECK_FALSE(hk.alt);
    CHECK_FALSE(hk.super);
}

TEST_CASE("ParseImGuiHotkey: last main key wins when several are present") {
    ImGuiBugHotkey hk;
    REQUIRE(ParseImGuiHotkey("Ctrl+A+B", hk));
    CHECK(hk.ctrl);
    CHECK(hk.key == ImGuiKey_B);
}

TEST_CASE("StringifyImGuiHotkey: fixed modifier order + empty on no key") {
    ImGuiBugHotkey hk;
    hk.key = ImGuiKey_None;
    CHECK(StringifyImGuiHotkey(hk).empty());

    hk = ImGuiBugHotkey{};
    hk.ctrl = true;
    hk.shift = true;
    hk.alt = true;
    hk.super = true;
    hk.key = ImGuiKey_B;
    CHECK(StringifyImGuiHotkey(hk) == "Ctrl+Shift+Alt+Super+B");
}

TEST_CASE("StringifyImGuiHotkey: letters upper-cased, named keys canonical") {
    ImGuiBugHotkey hk;
    hk.key = ImGuiKey_Comma;
    CHECK(StringifyImGuiHotkey(hk) == ",");

    hk = ImGuiBugHotkey{};
    hk.key = ImGuiKey_Space;
    CHECK(StringifyImGuiHotkey(hk) == "Space");

    hk = ImGuiBugHotkey{};
    hk.key = ImGuiKey_Enter;
    CHECK(StringifyImGuiHotkey(hk) == "Enter");

    hk = ImGuiBugHotkey{};
    hk.key = ImGuiKey_F11;
    CHECK(StringifyImGuiHotkey(hk) == "F11");
}

TEST_CASE("Parse <-> Stringify round-trips for the default shortcut set") {
    // Every spec the seeded KeybindingsConfig::Defaults() ships must survive a
    // parse->stringify->parse round-trip unchanged (canonical form is stable).
    const char* specs[] = {
        "Ctrl+B",       "Ctrl+Alt+B",   "Ctrl+J",     "Ctrl+Shift+A", "Ctrl+Shift+F",
        "Ctrl+Shift+D", "Ctrl+Shift+I", "Ctrl+Shift+X", "Ctrl+Shift+K", "Ctrl+Shift+L",
        "Ctrl+,",       "Ctrl+Alt+D",   "F11",        "Ctrl+Shift+P", "Ctrl+Shift+B",
    };
    for (const char* spec : specs) {
        ImGuiBugHotkey first;
        REQUIRE_MESSAGE(ParseImGuiHotkey(spec, first), spec);
        const std::string canonical = StringifyImGuiHotkey(first);
        CHECK_MESSAGE(canonical == std::string(spec), spec); // already canonical

        ImGuiBugHotkey second;
        REQUIRE(ParseImGuiHotkey(canonical, second));
        CHECK(second.ctrl == first.ctrl);
        CHECK(second.shift == first.shift);
        CHECK(second.alt == first.alt);
        CHECK(second.super == first.super);
        CHECK(second.key == first.key);
    }
}

namespace {

ImGuiBugHotkey Hk(bool ctrl, bool shift, bool alt, bool super, ImGuiKey key) {
    ImGuiBugHotkey hk;
    hk.ctrl = ctrl;
    hk.shift = shift;
    hk.alt = alt;
    hk.super = super;
    hk.key = key;
    return hk;
}

} // namespace

TEST_CASE("FindShortcutConflict: returns first matching index or -1") {
    std::vector<ImGuiBugHotkey> existing;
    existing.push_back(Hk(true, false, false, false, ImGuiKey_B));  // Ctrl+B
    existing.push_back(Hk(true, true, false, false, ImGuiKey_F));   // Ctrl+Shift+F
    existing.push_back(Hk(false, false, false, false, ImGuiKey_F11)); // F11

    CHECK(FindShortcutConflict(existing, Hk(true, true, false, false, ImGuiKey_F)) == 1);
    CHECK(FindShortcutConflict(existing, Hk(false, false, false, false, ImGuiKey_F11)) == 2);
    // Same key, different modifiers -> no conflict.
    CHECK(FindShortcutConflict(existing, Hk(true, true, false, false, ImGuiKey_B)) == -1);
    // Super disambiguates -> no conflict against the plain Ctrl+B.
    CHECK(FindShortcutConflict(existing, Hk(true, false, false, true, ImGuiKey_B)) == -1);
    // Unseen combo.
    CHECK(FindShortcutConflict(existing, Hk(false, false, true, false, ImGuiKey_Z)) == -1);
}

TEST_CASE("FindShortcutConflict: empty list never conflicts") {
    std::vector<ImGuiBugHotkey> empty;
    CHECK(FindShortcutConflict(empty, Hk(true, false, false, false, ImGuiKey_B)) == -1);
}
