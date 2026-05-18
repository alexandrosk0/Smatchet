// HotkeyParse implementation. Pure ASCII token-walker, no Win32, no allocator
// surprises — produces a Hotkey usable directly by RegisterHotKey on the
// caller side. The full vocabulary list lives in the header docstring; this
// TU is the canonical source for the case-insensitive string -> VK mapping.

#include "HotkeyParse.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace smatchet {
namespace whisper {
namespace hotkey {

namespace {

// Lower-case ASCII in place. Locale-independent — descriptor tokens are pure
// ASCII (English mnemonic key names).
std::string AsciiLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z') {
            out.push_back(static_cast<char>(uc + ('a' - 'A')));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Trim ASCII whitespace from both ends. Spaces around `+` are tolerated.
std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// Split `s` on `+`, trimming each segment. Empty input -> empty result; an
// empty segment (e.g. "Ctrl++A") is preserved so the caller surfaces the
// malformed input as an "empty token" error.
std::vector<std::string> SplitOnPlus(const std::string& s) {
    std::vector<std::string> out;
    std::string current;
    for (char c : s) {
        if (c == '+') {
            out.push_back(Trim(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(Trim(current));
    return out;
}

// Map a lower-cased token to a modifier bit. Returns 0 when the token is not
// a recognised modifier; the caller then tries the key-name table.
unsigned int TokenToModifier(const std::string& lower) {
    if (lower == "ctrl" || lower == "control") return mod::kControl;
    if (lower == "alt") return mod::kAlt;
    if (lower == "shift") return mod::kShift;
    if (lower == "win" || lower == "meta" || lower == "super") return mod::kWin;
    return 0;
}

// Map a lower-cased token to a virtual-key code. Returns 0 when the token is
// not a recognised key.
unsigned int TokenToVirtualKey(const std::string& lower) {
    // Single ASCII letter or digit — map to its uppercase ASCII byte (VK_A == 'A').
    if (lower.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(lower[0]);
        if (ch >= 'a' && ch <= 'z') {
            return static_cast<unsigned int>(ch - ('a' - 'A'));
        }
        if (ch >= '0' && ch <= '9') {
            return static_cast<unsigned int>(ch);
        }
    }

    // Function keys F1..F24 — VK_F1 is 0x70 and the range is contiguous.
    if (lower.size() >= 2 && lower[0] == 'f') {
        const std::string digits = lower.substr(1);
        bool numeric = !digits.empty();
        for (char c : digits) {
            if (c < '0' || c > '9') {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            int n = 0;
            for (char c : digits) {
                n = n * 10 + (c - '0');
                if (n > 24) {
                    return 0; // out-of-range function key
                }
            }
            if (n >= 1 && n <= 24) {
                return vk::kF1 + static_cast<unsigned int>(n - 1);
            }
        }
    }

    if (lower == "space") return vk::kSpace;
    if (lower == "enter" || lower == "return") return vk::kEnter;
    if (lower == "escape" || lower == "esc") return vk::kEscape;
    if (lower == "tab") return vk::kTab;
    if (lower == "backspace") return vk::kBackspace;
    if (lower == "up") return vk::kUp;
    if (lower == "down") return vk::kDown;
    if (lower == "left") return vk::kLeft;
    if (lower == "right") return vk::kRight;
    if (lower == "home") return vk::kHome;
    if (lower == "end") return vk::kEnd;
    if (lower == "pageup") return vk::kPageUp;
    if (lower == "pagedown") return vk::kPageDown;
    if (lower == "insert") return vk::kInsert;
    if (lower == "delete") return vk::kDelete;

    return 0;
}

// Inverse — map a VK back to the canonical token used in Stringify output.
// Returns an empty string when the VK is not one of the supported keys.
std::string VirtualKeyToToken(unsigned int v) {
    if (v >= 'A' && v <= 'Z') {
        return std::string(1, static_cast<char>(v));
    }
    if (v >= '0' && v <= '9') {
        return std::string(1, static_cast<char>(v));
    }
    if (v >= vk::kF1 && v <= vk::kF1 + 23u) {
        const unsigned int n = v - vk::kF1 + 1u;
        char buf[8];
        std::snprintf(buf, sizeof(buf), "F%u", n);
        return buf;
    }
    switch (v) {
    case vk::kSpace:
        return "Space";
    case vk::kEnter:
        return "Enter";
    case vk::kEscape:
        return "Esc";
    case vk::kTab:
        return "Tab";
    case vk::kBackspace:
        return "Backspace";
    case vk::kUp:
        return "Up";
    case vk::kDown:
        return "Down";
    case vk::kLeft:
        return "Left";
    case vk::kRight:
        return "Right";
    case vk::kHome:
        return "Home";
    case vk::kEnd:
        return "End";
    case vk::kPageUp:
        return "PageUp";
    case vk::kPageDown:
        return "PageDown";
    case vk::kInsert:
        return "Insert";
    case vk::kDelete:
        return "Delete";
    default:
        return std::string();
    }
}

} // namespace

bool Parse(const std::string& descriptor, Hotkey& out, std::string& outError) {
    out = Hotkey();
    outError.clear();

    const std::string trimmed = Trim(descriptor);
    if (trimmed.empty()) {
        outError = "hotkey descriptor is empty";
        return false;
    }

    const std::vector<std::string> tokens = SplitOnPlus(trimmed);
    unsigned int mods = 0;
    unsigned int vkOut = 0;
    bool keySeen = false;

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& raw = tokens[i];
        if (raw.empty()) {
            outError = "empty hotkey token (check for stray '+')";
            out = Hotkey();
            return false;
        }
        const std::string lower = AsciiLower(raw);
        const unsigned int m = TokenToModifier(lower);
        if (m != 0) {
            if ((mods & m) != 0) {
                outError = "duplicate modifier: " + raw;
                out = Hotkey();
                return false;
            }
            mods |= m;
            continue;
        }
        const unsigned int v = TokenToVirtualKey(lower);
        if (v == 0) {
            outError = "unknown hotkey token: " + raw;
            out = Hotkey();
            return false;
        }
        if (keySeen) {
            outError = "more than one non-modifier key in descriptor";
            out = Hotkey();
            return false;
        }
        vkOut = v;
        keySeen = true;
    }

    if (!keySeen) {
        outError = "hotkey requires a non-modifier key";
        out = Hotkey();
        return false;
    }

    out.mods = mods;
    out.vk = vkOut;
    return true;
}

std::string Stringify(const Hotkey& hk) {
    if (hk.vk == 0) {
        return std::string();
    }
    std::string out;
    out.reserve(32);
    if ((hk.mods & mod::kControl) != 0) {
        if (!out.empty()) out.push_back('+');
        out += "Ctrl";
    }
    if ((hk.mods & mod::kAlt) != 0) {
        if (!out.empty()) out.push_back('+');
        out += "Alt";
    }
    if ((hk.mods & mod::kShift) != 0) {
        if (!out.empty()) out.push_back('+');
        out += "Shift";
    }
    if ((hk.mods & mod::kWin) != 0) {
        if (!out.empty()) out.push_back('+');
        out += "Win";
    }
    const std::string key = VirtualKeyToToken(hk.vk);
    if (key.empty()) {
        return std::string();
    }
    if (!out.empty()) out.push_back('+');
    out += key;
    return out;
}

} // namespace hotkey
} // namespace whisper
} // namespace smatchet
