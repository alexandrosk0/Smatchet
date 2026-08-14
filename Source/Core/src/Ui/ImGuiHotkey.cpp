#include "ImGuiHotkey.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace smatchet {
namespace ui {

namespace {

std::string LowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Punctuation + named navigation/edit keys — single source of truth for both
// directions. `token` is stored display-cased (what KeyToToken emits); parsing
// compares case-insensitively, so "Esc"/"esc" both resolve. Multiple rows may
// share a key (aliases like Return/Enter); `canonical` marks the one row
// KeyToToken renders. Letters / digits / F-keys are contiguous in imgui and use
// offset arithmetic instead of a table row.
struct NamedKey {
    const char* token;
    ImGuiKey key;
    bool canonical;
};
const NamedKey kNamedKeys[] = {
    {"=", ImGuiKey_Equal, true},           {"-", ImGuiKey_Minus, true},
    {",", ImGuiKey_Comma, true},           {".", ImGuiKey_Period, true},
    {"/", ImGuiKey_Slash, true},           {";", ImGuiKey_Semicolon, true},
    {"'", ImGuiKey_Apostrophe, true},      {"`", ImGuiKey_GraveAccent, true},
    {"[", ImGuiKey_LeftBracket, true},     {"]", ImGuiKey_RightBracket, true},
    {"\\", ImGuiKey_Backslash, true},      {"Space", ImGuiKey_Space, true},
    {"Enter", ImGuiKey_Enter, true},       {"Return", ImGuiKey_Enter, false},
    {"Tab", ImGuiKey_Tab, true},           {"Backspace", ImGuiKey_Backspace, true},
    {"Delete", ImGuiKey_Delete, true},     {"Del", ImGuiKey_Delete, false},
    {"Escape", ImGuiKey_Escape, true},     {"Esc", ImGuiKey_Escape, false},
    {"Insert", ImGuiKey_Insert, true},     {"Ins", ImGuiKey_Insert, false},
    {"Home", ImGuiKey_Home, true},         {"End", ImGuiKey_End, true},
    {"PageUp", ImGuiKey_PageUp, true},     {"PgUp", ImGuiKey_PageUp, false},
    {"PageDown", ImGuiKey_PageDown, true}, {"PgDn", ImGuiKey_PageDown, false},
    {"Up", ImGuiKey_UpArrow, true},        {"Down", ImGuiKey_DownArrow, true},
    {"Left", ImGuiKey_LeftArrow, true},    {"Right", ImGuiKey_RightArrow, true},
    // Numeric keypad. Tokens are punctuation-FREE by necessity: '+' is the spec
    // separator, so a "Num+" token would split into ["num", "+"] and resolve to the
    // main-row Shift+'=' instead — the wrong physical key, silently. The keypad keys
    // are layout-independent, which is why the zoom defaults bind them alongside the
    // main-row combos.
    {"Num0", ImGuiKey_Keypad0, true},               {"Num1", ImGuiKey_Keypad1, true},
    {"Num2", ImGuiKey_Keypad2, true},               {"Num3", ImGuiKey_Keypad3, true},
    {"Num4", ImGuiKey_Keypad4, true},               {"Num5", ImGuiKey_Keypad5, true},
    {"Num6", ImGuiKey_Keypad6, true},               {"Num7", ImGuiKey_Keypad7, true},
    {"Num8", ImGuiKey_Keypad8, true},               {"Num9", ImGuiKey_Keypad9, true},
    {"NumAdd", ImGuiKey_KeypadAdd, true},           {"KeypadAdd", ImGuiKey_KeypadAdd, false},
    {"NumSubtract", ImGuiKey_KeypadSubtract, true}, {"KeypadSubtract", ImGuiKey_KeypadSubtract, false},
    {"NumMultiply", ImGuiKey_KeypadMultiply, true}, {"KeypadMultiply", ImGuiKey_KeypadMultiply, false},
    {"NumDivide", ImGuiKey_KeypadDivide, true},     {"KeypadDivide", ImGuiKey_KeypadDivide, false},
    {"NumDecimal", ImGuiKey_KeypadDecimal, true},   {"NumEnter", ImGuiKey_KeypadEnter, true},
    {"NumEqual", ImGuiKey_KeypadEqual, true},
};

// Tokens naming the SHIFTED character of a main-row key on a US/ANSI layout: '+' IS
// Shift+'=' and '_' IS Shift+'-'. ImGui reports ImGuiKey_Equal / ImGuiKey_Minus with
// io.KeyShift set, so a spec written with the shifted character must normalize to
// {shift, base key} or MatchHotkey's exact-modifier rule would never fire for it.
// These are INPUT spellings only — StringifyImGuiHotkey renders the canonical
// "Ctrl+Shift+=" form, so "Ctrl++" round-trips to "Ctrl+Shift+=" by design and the
// two specs correctly collide in a conflict check (they are one keystroke).
struct ShiftedKey {
    const char* token;
    ImGuiKey key;
};
const ShiftedKey kShiftedKeys[] = {
    {"+", ImGuiKey_Equal},
    {"plus", ImGuiKey_Equal},
    {"_", ImGuiKey_Minus},
    {"underscore", ImGuiKey_Minus},
};

// Map a single non-modifier token to an ImGuiKey. ImGuiKey_A..Z, _0..9, _F1..F12
// are contiguous in imgui, so offset arithmetic is safe; everything else is a
// case-insensitive lookup in kNamedKeys. Tokens arrive lower-cased.
ImGuiKey KeyFromToken(const std::string& tok) {
    if (tok.size() == 1) {
        const char c = tok[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'a'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
        }
    }
    if (tok.size() >= 2 && tok[0] == 'f') {
        const std::string num = tok.substr(1);
        bool allDigits = !num.empty();
        for (char c : num) {
            if (c < '0' || c > '9') {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            const int n = std::atoi(num.c_str());
            if (n >= 1 && n <= 12) {
                return static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
            }
        }
    }
    for (const NamedKey& nk : kNamedKeys) {
        if (tok == LowerAscii(nk.token)) {
            return nk.key;
        }
    }
    return ImGuiKey_None;
}

// Map a token spelling the SHIFTED character of a main-row key to that key's base
// ImGuiKey; the caller raises the shift flag. Returns ImGuiKey_None for anything else
// so the caller falls through to the regular KeyFromToken lookup. Tokens arrive
// lower-cased.
ImGuiKey ShiftedKeyFromToken(const std::string& tok) {
    for (const ShiftedKey& sk : kShiftedKeys) {
        if (tok == LowerAscii(sk.token)) {
            return sk.key;
        }
    }
    return ImGuiKey_None;
}

// Inverse of KeyFromToken: render an ImGuiKey as a canonical display token.
// Letters upper-cased so the round-tripped string is human-presentable.
std::string KeyToToken(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
        const char c = static_cast<char>('A' + (key - ImGuiKey_A));
        return std::string(1, c);
    }
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
        const char c = static_cast<char>('0' + (key - ImGuiKey_0));
        return std::string(1, c);
    }
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
        const int n = 1 + (key - ImGuiKey_F1);
        return std::string("F") + std::to_string(n);
    }
    for (const NamedKey& nk : kNamedKeys) {
        if (nk.canonical && nk.key == key) {
            return std::string(nk.token);
        }
    }
    return std::string();
}

} // namespace

bool ParseImGuiHotkey(const std::string& spec, ImGuiBugHotkey& out) {
    out = ImGuiBugHotkey{};
    if (spec.empty()) {
        return false;
    }
    const std::string lower = LowerAscii(spec);

    // Right-trim so "is this '+' the final character?" is an exact question (the loop
    // below already drops interior spaces/tabs).
    std::string trimmed = lower;
    while (!trimmed.empty() && (trimmed[trimmed.size() - 1U] == ' ' || trimmed[trimmed.size() - 1U] == '\t')) {
        trimmed.erase(trimmed.size() - 1U);
    }

    // A '+' that would yield an EMPTY token and sits at the very end of the spec is a
    // literal plus main key ("Ctrl++", "Ctrl+ +", bare "+"). Every other '+' stays the
    // separator it has always been, which keeps tokenization byte-identical for every
    // previously-parseable spec — notably "Ctrl++B", which still reads as Ctrl+B rather
    // than silently gaining a Shift.
    std::vector<std::string> tokens;
    std::string cur;
    for (std::size_t i = 0; i < trimmed.size(); ++i) {
        const char c = trimmed[i];
        if (c == '+') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            } else if (i + 1U == trimmed.size()) {
                cur.push_back('+');
            }
        } else if (c != ' ' && c != '\t') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        tokens.push_back(cur);
    }

    bool gotKey = false;
    for (const std::string& tok : tokens) {
        if (tok == "ctrl" || tok == "control") {
            out.ctrl = true;
        } else if (tok == "shift") {
            out.shift = true;
        } else if (tok == "alt") {
            out.alt = true;
        } else if (tok == "super" || tok == "win" || tok == "cmd") {
            out.super = true;
        } else {
            const ImGuiKey shifted = ShiftedKeyFromToken(tok);
            if (shifted != ImGuiKey_None) {
                out.key = shifted; // last main key wins
                out.shift = true;  // the shifted spelling IS Shift + the base key
                gotKey = true;
                continue;
            }
            const ImGuiKey k = KeyFromToken(tok);
            if (k != ImGuiKey_None) {
                out.key = k; // last main key wins
                gotKey = true;
            }
        }
    }
    return gotKey && out.key != ImGuiKey_None;
}

std::string StringifyImGuiHotkey(const ImGuiBugHotkey& hk) {
    const std::string main = KeyToToken(hk.key);
    if (main.empty()) {
        return std::string();
    }
    std::string out;
    if (hk.ctrl) {
        out += "Ctrl+";
    }
    if (hk.shift) {
        out += "Shift+";
    }
    if (hk.alt) {
        out += "Alt+";
    }
    if (hk.super) {
        out += "Super+";
    }
    out += main;
    return out;
}

bool MatchHotkey(const ImGuiIO& io, const ImGuiBugHotkey& hk) {
    if (hk.key == ImGuiKey_None) {
        return false;
    }
    if (io.KeyCtrl != hk.ctrl || io.KeyShift != hk.shift || io.KeyAlt != hk.alt || io.KeySuper != hk.super) {
        return false;
    }
    return ImGui::IsKeyPressed(hk.key, /*repeat*/ false);
}

bool SameCombo(const ImGuiBugHotkey& a, const ImGuiBugHotkey& b) {
    return a.key == b.key && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt && a.super == b.super;
}

int FindShortcutConflict(const std::vector<ImGuiBugHotkey>& existing, const ImGuiBugHotkey& candidate) {
    for (size_t i = 0; i < existing.size(); ++i) {
        if (SameCombo(existing[i], candidate)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const std::vector<ImGuiKey>& BindableImGuiKeys() {
    // Built once from the same sources KeyToToken renders from, so the capture widget
    // can never offer a key the stringifier would drop (a captured combo must survive
    // Stringify -> Parse). Function-local static: no static-init-order concern, and the
    // build cost is paid on the first capture frame, never per frame.
    static std::vector<ImGuiKey> keys;
    if (!keys.empty()) {
        return keys;
    }
    keys.reserve(26U + 10U + 12U + (sizeof(kNamedKeys) / sizeof(kNamedKeys[0])));
    for (ImGuiKey k = ImGuiKey_A; k <= ImGuiKey_Z; k = static_cast<ImGuiKey>(k + 1)) {
        keys.push_back(k);
    }
    for (ImGuiKey k = ImGuiKey_0; k <= ImGuiKey_9; k = static_cast<ImGuiKey>(k + 1)) {
        keys.push_back(k);
    }
    for (ImGuiKey k = ImGuiKey_F1; k <= ImGuiKey_F12; k = static_cast<ImGuiKey>(k + 1)) {
        keys.push_back(k);
    }
    for (const NamedKey& nk : kNamedKeys) {
        if (!nk.canonical) {
            continue; // alias row — the canonical row already contributed this key
        }
        if (nk.key == ImGuiKey_Escape) {
            continue; // Esc cancels a capture; binding it would make capture unexitable
        }
        keys.push_back(nk.key);
    }
    return keys;
}

} // namespace ui
} // namespace smatchet
