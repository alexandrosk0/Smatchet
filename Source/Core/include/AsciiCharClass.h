#ifndef SMATCHET_ASCII_CHAR_CLASS_H
#define SMATCHET_ASCII_CHAR_CLASS_H

// Locale-independent ASCII character classes for hand-written scanners. The <cctype> functions
// follow the global locale and can accept bytes >= 0x80, so a scanner that must give the same
// answer on every machine (and match a pattern written for the classic locale) uses these instead.

namespace smatchet {
namespace ascii {

inline bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsAlnum(char c) { return IsAlpha(c) || IsDigit(c); }
inline bool IsHexDigit(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline bool IsLowerHexDigit(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f'); }

// The classic-locale whitespace set (what `\s` and std::isspace mean in the "C" locale): space, \t,
// \n, \v, \f, \r.
inline bool IsSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

inline char ToLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

} // namespace ascii
} // namespace smatchet

#endif // SMATCHET_ASCII_CHAR_CLASS_H
