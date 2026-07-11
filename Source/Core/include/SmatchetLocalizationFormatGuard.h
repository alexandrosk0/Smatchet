#ifndef SMATCHET_LOCALIZATION_FORMAT_GUARD_H
#define SMATCHET_LOCALIZATION_FORMAT_GUARD_H

// Pure printf conversion-specifier guard extracted from SmatchetLocalization.cpp
// (SECURITY_AUDIT.md #1 / CPP_CODE_AUDIT.md #7 remediation). A translated string
// loaded from an attacker-influenceable Locales/<lang>.json override must never be
// handed to a vsnprintf-family sink unless its conversion-specifier sequence is
// identical to the trusted English literal the caller's varargs match — otherwise a
// crafted override (e.g. "%s %s %n") reads/writes varargs that were never supplied.
//
// Header-only + dependency-free (std only) so the guard can be unit- and
// fuzz-tested in isolation (tests/fuzz/fuzz_locale_format_guard.cpp) without
// linking SmatchetLocalization.cpp's ConfigManager/Logger closure. nightly-monkey-
// tester Layer 3 (pure security-sensitive string parser over untrusted input).

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace smatchet {
namespace l10n {

// Extract the ordered list of printf conversion-specifier tokens from a format
// string (each token is the run from '%' through its conversion char; `%%` is a
// literal and produces no token). Tolerates NULL and any byte sequence — a
// truncated trailing specifier ("...%") yields the whole tail as one token.
inline std::vector<std::string> ConversionSpecifiers(const char* fmt) {
    std::vector<std::string> specs;
    if (fmt == nullptr) {
        return specs;
    }
    for (const char* p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            continue;
        }
        const char* start = p++;
        if (*p == '%') { // "%%" — literal percent, not a conversion
            continue;
        }
        // flags, width/precision (incl. '*'), length modifiers — consume up to the
        // conversion char (the first alphabetic that terminates the specifier).
        while (*p != '\0' && std::strchr("-+ #0123456789.*hljztLqI", *p) != nullptr) {
            ++p;
        }
        if (*p == '\0') { // truncated specifier — treat the whole tail as one token
            specs.emplace_back(start);
            break;
        }
        specs.emplace_back(start, static_cast<std::size_t>(p - start) + 1); // include conversion char
    }
    return specs;
}

// True iff `translated` carries exactly the same conversion-specifier sequence as
// the trusted `englishLiteral`. A mismatch means the override added/changed/removed
// a specifier, so feeding it to vsnprintf would consume varargs that were never
// supplied (or a `%n` write) — Pillar 3 / arbitrary-write guard.
inline bool FormatSpecifiersMatch(const char* translated, const char* englishLiteral) {
    return ConversionSpecifiers(translated) == ConversionSpecifiers(englishLiteral);
}

} // namespace l10n
} // namespace smatchet

#endif // SMATCHET_LOCALIZATION_FORMAT_GUARD_H
