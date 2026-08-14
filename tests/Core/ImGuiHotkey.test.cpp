// Pure-logic tests for the rebindable-shortcut hotkey grammar:
// ParseImGuiHotkey / StringifyImGuiHotkey round-trip + FindShortcutConflict.
// docs/plans/shipped/keyboard-shortcuts-rebindable.md.
// These three functions are ImGui-context-free (they only read the ImGuiKey
// enum); MatchHotkey is excluded here — it needs a live ImGui IO frame and is
// exercised by the bucket-E UI pass, not the doctest rig.

#include <doctest/doctest.h>

#include "Ui/ImGuiHotkey.h"

#include <string>
#include <vector>

using smatchet::ui::BindableImGuiKeys;
using smatchet::ui::FindShortcutConflict;
using smatchet::ui::ImGuiBugHotkey;
using smatchet::ui::ParseImGuiHotkey;
using smatchet::ui::SameCombo;
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

TEST_CASE("ParseImGuiHotkey: punctuation keys (zoom in/out depend on = and -)") {
    ImGuiBugHotkey hk;

    REQUIRE(ParseImGuiHotkey("Ctrl+=", hk)); // Zoom In default
    CHECK(hk.ctrl);
    CHECK(hk.key == ImGuiKey_Equal);

    REQUIRE(ParseImGuiHotkey("Ctrl+-", hk)); // Zoom Out default
    CHECK(hk.ctrl);
    CHECK(hk.key == ImGuiKey_Minus);

    const struct {
        const char* spec;
        ImGuiKey key;
    } punct[] = {
        {".", ImGuiKey_Period},       {"/", ImGuiKey_Slash},       {";", ImGuiKey_Semicolon},
        {"'", ImGuiKey_Apostrophe},   {"`", ImGuiKey_GraveAccent}, {"[", ImGuiKey_LeftBracket},
        {"]", ImGuiKey_RightBracket}, {"\\", ImGuiKey_Backslash},
    };
    for (const auto& p : punct) {
        ImGuiBugHotkey k;
        REQUIRE_MESSAGE(ParseImGuiHotkey(p.spec, k), p.spec);
        CHECK(k.key == p.key);
    }
}

TEST_CASE("ParseImGuiHotkey: navigation / editing keys + aliases") {
    const struct {
        const char* spec;
        ImGuiKey key;
    } nav[] = {
        {"Tab", ImGuiKey_Tab},           {"Backspace", ImGuiKey_Backspace}, {"Delete", ImGuiKey_Delete},
        {"Del", ImGuiKey_Delete},        {"Escape", ImGuiKey_Escape},       {"Esc", ImGuiKey_Escape},
        {"Insert", ImGuiKey_Insert},     {"Ins", ImGuiKey_Insert},          {"Home", ImGuiKey_Home},
        {"End", ImGuiKey_End},           {"PageUp", ImGuiKey_PageUp},       {"PgUp", ImGuiKey_PageUp},
        {"PageDown", ImGuiKey_PageDown}, {"PgDn", ImGuiKey_PageDown},       {"Up", ImGuiKey_UpArrow},
        {"Down", ImGuiKey_DownArrow},    {"Left", ImGuiKey_LeftArrow},      {"Right", ImGuiKey_RightArrow},
    };
    for (const auto& n : nav) {
        ImGuiBugHotkey k;
        REQUIRE_MESSAGE(ParseImGuiHotkey(n.spec, k), n.spec);
        CHECK(k.key == n.key);
    }
}

TEST_CASE("ParseImGuiHotkey: failure cases leave a reset struct") {
    ImGuiBugHotkey hk;
    CHECK_FALSE(ParseImGuiHotkey("", hk));           // empty
    CHECK_FALSE(ParseImGuiHotkey("Ctrl+Shift", hk)); // modifiers only, no main key
    CHECK_FALSE(ParseImGuiHotkey("F13", hk));        // F-key out of 1..12 range
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

// --- multi-combo keybindings: the literal-'+' + keypad grammar extension --------
// docs/plans/keybindings-multi-combo.md. The tokenizer learned that a trailing '+'
// is a literal key; these cases pin BOTH that it works and that nothing else moved.

TEST_CASE("ParseImGuiHotkey: the literal-plus rule changes no pre-existing spec") {
    // Every spec below parsed identically before the trailing-'+' rule landed. This
    // is the regression wall for that tokenizer change — if any of these shifts, the
    // rule leaked past the final-character position it is confined to.
    const struct {
        const char* spec;
        bool ctrl;
        bool shift;
        bool alt;
        ImGuiKey key;
    } unchanged[] = {
        {"Ctrl+=", true, false, false, ImGuiKey_Equal},
        {"Ctrl+-", true, false, false, ImGuiKey_Minus},
        {"Ctrl+,", true, false, false, ImGuiKey_Comma},
        {"Ctrl+Shift+B", true, true, false, ImGuiKey_B},
        {"  CTRL +  B ", true, false, false, ImGuiKey_B},
        {"Ctrl+A+B", true, false, false, ImGuiKey_B},
        {"Ctrl+Alt+D", true, false, true, ImGuiKey_D},
        // An INTERIOR '+' stays the separator, so this is still plain Ctrl+B — it does
        // NOT silently gain a Shift by reading the second '+' as a plus key.
        {"Ctrl++B", true, false, false, ImGuiKey_B},
    };
    for (const auto& u : unchanged) {
        ImGuiBugHotkey hk;
        REQUIRE_MESSAGE(ParseImGuiHotkey(u.spec, hk), u.spec);
        CHECK_MESSAGE(hk.ctrl == u.ctrl, u.spec);
        CHECK_MESSAGE(hk.shift == u.shift, u.spec);
        CHECK_MESSAGE(hk.alt == u.alt, u.spec);
        CHECK_MESSAGE(hk.key == u.key, u.spec);
    }

    // Still-failing specs stay failing: a dangling separator is not a key.
    ImGuiBugHotkey bad;
    CHECK_FALSE(ParseImGuiHotkey("Ctrl+", bad));
    CHECK_FALSE(ParseImGuiHotkey("Ctrl+Shift", bad));
}

TEST_CASE("ParseImGuiHotkey: '+' and '_' normalise to Shift + their base key") {
    // On a US/ANSI layout '+' IS Shift+'=' — ImGui reports ImGuiKey_Equal with
    // io.KeyShift set — so the spec must carry the shift flag or MatchHotkey's
    // exact-modifier rule could never fire for it.
    const char* plusSpellings[] = {"Ctrl++", "Ctrl+Plus", "Ctrl+ +", "ctrl+plus"};
    for (const char* spec : plusSpellings) {
        ImGuiBugHotkey hk;
        REQUIRE_MESSAGE(ParseImGuiHotkey(spec, hk), spec);
        CHECK_MESSAGE(hk.ctrl, spec);
        CHECK_MESSAGE(hk.shift, spec);
        CHECK_MESSAGE(hk.key == ImGuiKey_Equal, spec);
    }

    ImGuiBugHotkey bare;
    REQUIRE(ParseImGuiHotkey("+", bare));
    CHECK_FALSE(bare.ctrl);
    CHECK(bare.shift);
    CHECK(bare.key == ImGuiKey_Equal);

    ImGuiBugHotkey under;
    REQUIRE(ParseImGuiHotkey("Ctrl+_", under));
    CHECK(under.ctrl);
    CHECK(under.shift);
    CHECK(under.key == ImGuiKey_Minus);
    REQUIRE(ParseImGuiHotkey("Ctrl+Underscore", under));
    CHECK(under.shift);
    CHECK(under.key == ImGuiKey_Minus);

    // Idempotent when the spec already spells the Shift out.
    ImGuiBugHotkey both;
    REQUIRE(ParseImGuiHotkey("Ctrl+Shift++", both));
    CHECK(both.ctrl);
    CHECK(both.shift);
    CHECK(both.key == ImGuiKey_Equal);
}

TEST_CASE("StringifyImGuiHotkey: shifted spellings canonicalise, they do not echo") {
    // Deliberate asymmetry: "Ctrl++" is an accepted INPUT spelling whose canonical
    // rendering is "Ctrl+Shift+=". Config stores canonical strings, so the stored
    // form is a round-trip fixed point even though the input form is not.
    ImGuiBugHotkey hk;
    REQUIRE(ParseImGuiHotkey("Ctrl++", hk));
    CHECK(StringifyImGuiHotkey(hk) == "Ctrl+Shift+=");

    ImGuiBugHotkey again;
    REQUIRE(ParseImGuiHotkey(StringifyImGuiHotkey(hk), again));
    CHECK(StringifyImGuiHotkey(again) == "Ctrl+Shift+="); // fixed point from here on
}

TEST_CASE("FindShortcutConflict: 'Ctrl++' collides with 'Ctrl+Shift+='") {
    // The whole point of normalising at parse time: the two spellings are ONE
    // keystroke, so the editor must warn. Conflict checks compare parsed structs,
    // never spec strings, which is what makes this fall out for free.
    ImGuiBugHotkey explicitShift;
    ImGuiBugHotkey plusForm;
    REQUIRE(ParseImGuiHotkey("Ctrl+Shift+=", explicitShift));
    REQUIRE(ParseImGuiHotkey("Ctrl++", plusForm));
    CHECK(SameCombo(explicitShift, plusForm));

    std::vector<ImGuiBugHotkey> existing;
    existing.push_back(explicitShift);
    CHECK(FindShortcutConflict(existing, plusForm) == 0);

    // ...and Ctrl+= (no shift) is a DIFFERENT keystroke, so it must not collide.
    ImGuiBugHotkey noShift;
    REQUIRE(ParseImGuiHotkey("Ctrl+=", noShift));
    CHECK_FALSE(SameCombo(explicitShift, noShift));
    CHECK(FindShortcutConflict(existing, noShift) == -1);
}

TEST_CASE("ParseImGuiHotkey: keypad tokens (layout-independent zoom aliases)") {
    const struct {
        const char* spec;
        ImGuiKey key;
    } pad[] = {
        {"Num0", ImGuiKey_Keypad0},
        {"Num5", ImGuiKey_Keypad5},
        {"Num9", ImGuiKey_Keypad9},
        {"NumAdd", ImGuiKey_KeypadAdd},
        {"KeypadAdd", ImGuiKey_KeypadAdd},
        {"NumSubtract", ImGuiKey_KeypadSubtract},
        {"KeypadSubtract", ImGuiKey_KeypadSubtract},
        {"NumMultiply", ImGuiKey_KeypadMultiply},
        {"NumDivide", ImGuiKey_KeypadDivide},
        {"NumDecimal", ImGuiKey_KeypadDecimal},
        {"NumEnter", ImGuiKey_KeypadEnter},
        {"NumEqual", ImGuiKey_KeypadEqual},
    };
    for (const auto& p : pad) {
        ImGuiBugHotkey hk;
        REQUIRE_MESSAGE(ParseImGuiHotkey(p.spec, hk), p.spec);
        CHECK_MESSAGE(hk.key == p.key, p.spec);
    }

    // The keypad key is distinct from its main-row namesake — that is exactly why it
    // earns a second binding rather than sharing one.
    ImGuiBugHotkey padAdd;
    ImGuiBugHotkey rowEqual;
    REQUIRE(ParseImGuiHotkey("Ctrl+NumAdd", padAdd));
    REQUIRE(ParseImGuiHotkey("Ctrl+=", rowEqual));
    CHECK_FALSE(SameCombo(padAdd, rowEqual));

    // Aliases render as their canonical spelling.
    CHECK(StringifyImGuiHotkey(padAdd) == "Ctrl+NumAdd");
}

TEST_CASE("BindableImGuiKeys: every capturable key survives Stringify -> Parse") {
    const std::vector<ImGuiKey>& keys = BindableImGuiKeys();
    REQUIRE_FALSE(keys.empty());

    for (std::size_t i = 0; i < keys.size(); ++i) {
        ImGuiBugHotkey hk;
        hk.ctrl = true; // a modifier is required for a bindable combo anyway
        hk.key = keys[i];
        const std::string spec = StringifyImGuiHotkey(hk);
        REQUIRE_FALSE(spec.empty()); // the stringifier must know every offered key

        ImGuiBugHotkey back;
        REQUIRE_MESSAGE(ParseImGuiHotkey(spec, back), spec.c_str());
        CHECK_MESSAGE(SameCombo(hk, back), spec.c_str());
    }

    // Esc is the capture widget's cancel key: offering it would make capture
    // unexitable, so it is deliberately absent even though the grammar parses it.
    bool sawEscape = false;
    bool sawKeypadAdd = false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        sawEscape = sawEscape || keys[i] == ImGuiKey_Escape;
        sawKeypadAdd = sawKeypadAdd || keys[i] == ImGuiKey_KeypadAdd;
    }
    CHECK_FALSE(sawEscape);
    CHECK(sawKeypadAdd); // the widened set is what makes Ctrl+numpad-plus capturable

    // No duplicates — alias rows must not contribute a second entry for one key.
    for (std::size_t i = 0; i < keys.size(); ++i) {
        for (std::size_t j = i + 1U; j < keys.size(); ++j) {
            CHECK(keys[i] != keys[j]);
        }
    }
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
        "Ctrl+B",
        "Ctrl+Alt+B",
        "Ctrl+J",
        "Ctrl+Shift+A",
        "Ctrl+Shift+F",
        "Ctrl+Shift+D",
        "Ctrl+Shift+I",
        "Ctrl+Shift+X",
        "Ctrl+Shift+K",
        "Ctrl+Shift+L",
        "Ctrl+,",
        "Ctrl+Alt+D",
        "F11",
        "Ctrl+Shift+P",
        "Ctrl+Shift+B",
        // Menu-shortcut additions (KeybindingsConfig::Defaults()):
        "Ctrl+=",
        "Ctrl+-",
        "Ctrl+0",
        "Ctrl+O",
        "Ctrl+Shift+V",
        "Ctrl+Shift+G",
        "Ctrl+Shift+E",
        "Ctrl+Shift+U",
        "Ctrl+Shift+M",
        "Ctrl+Shift+N",
        // Multi-combo zoom aliases (docs/plans/keybindings-multi-combo.md). These are
        // the CANONICAL spellings Defaults() ships; the "Ctrl++" input spelling is
        // deliberately absent because it canonicalises to "Ctrl+Shift+=" and so is not
        // a fixed point (see the canonicalisation case above).
        "Ctrl+Shift+=",
        "Ctrl+NumAdd",
        "Ctrl+NumSubtract",
        "Ctrl+Num0",
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
    existing.push_back(Hk(true, false, false, false, ImGuiKey_B));    // Ctrl+B
    existing.push_back(Hk(true, true, false, false, ImGuiKey_F));     // Ctrl+Shift+F
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
