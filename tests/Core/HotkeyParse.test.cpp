// Doctest coverage for the pure HotkeyParse helper. Always compiled when
// SMATCHET_WITH_WHISPER=ON via the gated `target_sources` block in
// `tests/CMakeLists.txt`. Each case set targets one contract surface so a
// regression names the broken expectation in the failure line.

#include "doctest/doctest.h"

#include "HotkeyParse.h"

#include <string>

using smatchet::whisper::hotkey::Hotkey;
using smatchet::whisper::hotkey::Parse;
using smatchet::whisper::hotkey::Stringify;
namespace mod = smatchet::whisper::hotkey::mod;
namespace vk = smatchet::whisper::hotkey::vk;

TEST_CASE("HotkeyParse: default Ctrl+Alt+Space round-trips") {
    Hotkey hk;
    std::string err;
    CHECK(Parse("Ctrl+Alt+Space", hk, err));
    CHECK(err.empty());
    CHECK(hk.mods == (mod::kControl | mod::kAlt));
    CHECK(hk.vk == vk::kSpace);
    CHECK(Stringify(hk) == "Ctrl+Alt+Space");
}

TEST_CASE("HotkeyParse: every modifier alone with a letter") {
    Hotkey hk;
    std::string err;
    CHECK(Parse("Ctrl+A", hk, err));
    CHECK(hk.mods == mod::kControl);
    CHECK(hk.vk == static_cast<unsigned int>('A'));

    CHECK(Parse("Alt+B", hk, err));
    CHECK(hk.mods == mod::kAlt);
    CHECK(hk.vk == static_cast<unsigned int>('B'));

    CHECK(Parse("Shift+C", hk, err));
    CHECK(hk.mods == mod::kShift);
    CHECK(hk.vk == static_cast<unsigned int>('C'));

    CHECK(Parse("Win+D", hk, err));
    CHECK(hk.mods == mod::kWin);
    CHECK(hk.vk == static_cast<unsigned int>('D'));
}

TEST_CASE("HotkeyParse: alias modifiers (Control/Meta/Super) accepted") {
    Hotkey hk;
    std::string err;
    CHECK(Parse("Control+E", hk, err));
    CHECK(hk.mods == mod::kControl);
    CHECK(Parse("Meta+E", hk, err));
    CHECK(hk.mods == mod::kWin);
    CHECK(Parse("Super+E", hk, err));
    CHECK(hk.mods == mod::kWin);
}

TEST_CASE("HotkeyParse: case-insensitive tokens") {
    Hotkey hk;
    std::string err;
    CHECK(Parse("cTrL+aLt+SpAcE", hk, err));
    CHECK(hk.mods == (mod::kControl | mod::kAlt));
    CHECK(hk.vk == vk::kSpace);

    CHECK(Parse("ctrl+f5", hk, err));
    CHECK(hk.mods == mod::kControl);
    CHECK(hk.vk == vk::kF1 + 4u);
}

TEST_CASE("HotkeyParse: whitespace around plus signs tolerated") {
    Hotkey hk;
    std::string err;
    CHECK(Parse("  Ctrl  +  Alt + Space  ", hk, err));
    CHECK(hk.mods == (mod::kControl | mod::kAlt));
    CHECK(hk.vk == vk::kSpace);
}

TEST_CASE("HotkeyParse: function keys F1..F24") {
    Hotkey hk;
    std::string err;
    CHECK(Parse("F1", hk, err));
    CHECK(hk.vk == vk::kF1);
    CHECK(Parse("F12", hk, err));
    CHECK(hk.vk == vk::kF1 + 11u);
    CHECK(Parse("F24", hk, err));
    CHECK(hk.vk == vk::kF1 + 23u);

    CHECK_FALSE(Parse("F0", hk, err));
    CHECK_FALSE(Parse("F25", hk, err));
    CHECK_FALSE(Parse("F100", hk, err));
}

TEST_CASE("HotkeyParse: named navigation keys") {
    Hotkey hk;
    std::string err;
    CHECK(Parse("Up", hk, err));
    CHECK(hk.vk == vk::kUp);
    CHECK(Parse("PageDown", hk, err));
    CHECK(hk.vk == vk::kPageDown);
    CHECK(Parse("Esc", hk, err));
    CHECK(hk.vk == vk::kEscape);
    CHECK(Parse("Escape", hk, err));
    CHECK(hk.vk == vk::kEscape);
    CHECK(Parse("Enter", hk, err));
    CHECK(hk.vk == vk::kEnter);
    CHECK(Parse("Return", hk, err));
    CHECK(hk.vk == vk::kEnter);
}

TEST_CASE("HotkeyParse: digits 0..9") {
    Hotkey hk;
    std::string err;
    CHECK(Parse("Ctrl+0", hk, err));
    CHECK(hk.vk == static_cast<unsigned int>('0'));
    CHECK(Parse("Ctrl+9", hk, err));
    CHECK(hk.vk == static_cast<unsigned int>('9'));
}

TEST_CASE("HotkeyParse: Stringify canonical order Ctrl/Alt/Shift/Win/Key") {
    Hotkey hk;
    hk.mods = mod::kWin | mod::kAlt | mod::kShift | mod::kControl;
    hk.vk = static_cast<unsigned int>('K');
    CHECK(Stringify(hk) == "Ctrl+Alt+Shift+Win+K");
}

TEST_CASE("HotkeyParse: Stringify -> Parse round-trip preserves struct") {
    Hotkey original;
    original.mods = mod::kControl | mod::kShift;
    original.vk = vk::kF1 + 6u; // F7
    const std::string s = Stringify(original);
    CHECK(s == "Ctrl+Shift+F7");

    Hotkey parsed;
    std::string err;
    CHECK(Parse(s, parsed, err));
    CHECK(parsed.mods == original.mods);
    CHECK(parsed.vk == original.vk);
}

TEST_CASE("HotkeyParse: empty / whitespace input rejected") {
    Hotkey hk;
    std::string err;
    CHECK_FALSE(Parse("", hk, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(Parse("   ", hk, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("HotkeyParse: modifiers-only descriptor rejected") {
    Hotkey hk;
    std::string err;
    CHECK_FALSE(Parse("Ctrl+Alt", hk, err));
    CHECK_FALSE(err.empty());
    CHECK(hk.mods == 0u);
    CHECK(hk.vk == 0u);
}

TEST_CASE("HotkeyParse: unknown token rejected") {
    Hotkey hk;
    std::string err;
    CHECK_FALSE(Parse("Ctrl+Banana", hk, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("HotkeyParse: duplicate modifier rejected") {
    Hotkey hk;
    std::string err;
    CHECK_FALSE(Parse("Ctrl+Ctrl+A", hk, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("HotkeyParse: two non-modifier keys rejected") {
    Hotkey hk;
    std::string err;
    CHECK_FALSE(Parse("Ctrl+A+B", hk, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("HotkeyParse: empty segment from stray plus rejected") {
    Hotkey hk;
    std::string err;
    CHECK_FALSE(Parse("Ctrl++A", hk, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(Parse("+A", hk, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("HotkeyParse: Stringify on default-constructed hotkey is empty") {
    CHECK(Stringify(Hotkey()) == "");
}
