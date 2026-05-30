#pragma once

// HotkeyParse — pure helper that converts a "Ctrl+Alt+Space"-style descriptor
// (the string persisted in `TrackerConfig::WhisperHotkey`) into a (modifiers,
// virtualKey) pair the Win32 RegisterHotKey / SetWindowsHookEx WH_KEYBOARD_LL
// layer can consume. No Win32 includes here so the unit-test rig links the
// helper unconditionally on every host; the bare constants below mirror the
// Win32 `MOD_*` and `VK_*` values byte-for-byte so the consumer can pass the
// struct fields directly to RegisterHotKey without a translation step.
//
// Lives in Source/Plugins/Whisper because the descriptor surface is meaningless when
// SMATCHET_WITH_WHISPER=OFF — the entire subdirectory is CMake-conditional.

#include <string>

namespace smatchet {
namespace whisper {
namespace hotkey {

// Bit-mask values that match the Win32 RegisterHotKey `fsModifiers` contract
// (winuser.h MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN). Mirror the SDK
// constants exactly so the parser output can flow into the Win32 call with
// zero translation.
namespace mod {
constexpr unsigned int kAlt = 0x0001u;
constexpr unsigned int kControl = 0x0002u;
constexpr unsigned int kShift = 0x0004u;
constexpr unsigned int kWin = 0x0008u;
} // namespace mod

// Win32 virtual-key codes for the subset of keys our descriptor language
// accepts. Reproduced here so the unit-test rig does not need <windows.h>;
// the values track the SDK byte-for-byte (winuser.h VK_*). Letter and digit
// keys use the ASCII character itself (VK_A == 'A' == 0x41 etc.).
namespace vk {
constexpr unsigned int kSpace = 0x20u;
constexpr unsigned int kBackspace = 0x08u;
constexpr unsigned int kTab = 0x09u;
constexpr unsigned int kEnter = 0x0Du;
constexpr unsigned int kEscape = 0x1Bu;
constexpr unsigned int kPageUp = 0x21u;
constexpr unsigned int kPageDown = 0x22u;
constexpr unsigned int kEnd = 0x23u;
constexpr unsigned int kHome = 0x24u;
constexpr unsigned int kLeft = 0x25u;
constexpr unsigned int kUp = 0x26u;
constexpr unsigned int kRight = 0x27u;
constexpr unsigned int kDown = 0x28u;
constexpr unsigned int kInsert = 0x2Du;
constexpr unsigned int kDelete = 0x2Eu;
constexpr unsigned int kF1 = 0x70u; // VK_F1; F2..F24 are consecutive (0x71..0x87)
} // namespace vk

struct Hotkey {
    // Bitmask over `mod::*` values. Zero modifiers + non-zero vk is allowed by
    // the parser (e.g. a bare F-key) but the consumer is expected to reject
    // modifiers-only descriptors before calling RegisterHotKey because a global
    // hotkey with no modifier collides with every keypress.
    unsigned int mods = 0;
    unsigned int vk = 0;
};

// Parse a `Ctrl+Alt+Space` style descriptor into a Hotkey. Returns true on
// success; on failure the out struct is zeroed and `outError` carries a short
// human-readable diagnostic (empty input, unknown token, bare-modifier
// descriptor). Whitespace around `+` separators is trimmed. Tokens are
// case-insensitive. Recognised:
//
//   * Modifiers (any combination, order-independent):
//     `Ctrl` / `Control`, `Alt`, `Shift`, `Win` / `Meta` / `Super`.
//   * Single-character keys `A`-`Z` and `0`-`9` (mapped to the ASCII VK).
//   * `F1`-`F24` function keys.
//   * Named keys: `Space`, `Enter` / `Return`, `Escape` / `Esc`, `Tab`,
//     `Backspace`, `Up`, `Down`, `Left`, `Right`, `Home`, `End`, `PageUp`,
//     `PageDown`, `Insert`, `Delete`.
bool Parse(const std::string& descriptor, Hotkey& out, std::string& outError);

// Inverse of Parse — build a canonical descriptor string from a Hotkey. Token
// order: `Ctrl`, `Alt`, `Shift`, `Win`, `<key>`. Single-letter keys are
// upper-cased. Function keys come back as `F<n>`. Named keys round-trip to the
// canonical token from the Parse vocabulary (e.g. `Esc` -> `Esc`, never
// `Escape`). Returns an empty string when `hk.vk == 0`.
std::string Stringify(const Hotkey& hk);

} // namespace hotkey
} // namespace whisper
} // namespace smatchet
