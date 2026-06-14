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

// Map a single non-modifier token to an ImGuiKey. ImGuiKey_A..Z, _0..9, _F1..F12
// are contiguous in imgui, so offset arithmetic is safe.
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
    if (tok == ",") {
        return ImGuiKey_Comma;
    }
    if (tok == "space") {
        return ImGuiKey_Space;
    }
    if (tok == "enter" || tok == "return") {
        return ImGuiKey_Enter;
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
    if (key == ImGuiKey_Comma) {
        return ",";
    }
    if (key == ImGuiKey_Space) {
        return "Space";
    }
    if (key == ImGuiKey_Enter) {
        return "Enter";
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
    if (io.KeyCtrl != hk.ctrl || io.KeyShift != hk.shift || io.KeyAlt != hk.alt ||
        io.KeySuper != hk.super) {
        return false;
    }
    return ImGui::IsKeyPressed(hk.key, /*repeat*/ false);
}

int FindShortcutConflict(const std::vector<ImGuiBugHotkey>& existing,
                         const ImGuiBugHotkey& candidate) {
    for (size_t i = 0; i < existing.size(); ++i) {
        const ImGuiBugHotkey& e = existing[i];
        if (e.key == candidate.key && e.ctrl == candidate.ctrl &&
            e.shift == candidate.shift && e.alt == candidate.alt &&
            e.super == candidate.super) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace ui
} // namespace smatchet
