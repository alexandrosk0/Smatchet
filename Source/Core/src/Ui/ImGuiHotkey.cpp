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

    std::vector<std::string> tokens;
    std::string cur;
    for (char c : lower) {
        if (c == '+') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
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

int FindShortcutConflict(const std::vector<ImGuiBugHotkey>& existing, const ImGuiBugHotkey& candidate) {
    for (size_t i = 0; i < existing.size(); ++i) {
        const ImGuiBugHotkey& e = existing[i];
        if (e.key == candidate.key && e.ctrl == candidate.ctrl && e.shift == candidate.shift &&
            e.alt == candidate.alt && e.super == candidate.super) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace ui
} // namespace smatchet
