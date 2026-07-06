// libFuzzer driver for the localization format-specifier guard —
// smatchet::l10n::ConversionSpecifiers / FormatSpecifiersMatch
// (SmatchetLocalizationFormatGuard.h). This guard is the sole thing standing
// between an attacker-influenceable Locales/<lang>.json override string and a
// vsnprintf-family sink in SmatchetLocalization::Format / TranslateSourceAsFormat
// (SECURITY_AUDIT.md #1 / CPP_CODE_AUDIT.md #7): only when the override's
// conversion-specifier sequence exactly matches the trusted English literal is the
// override used as the format string. This driver fuzzes the specifier parser +
// comparator directly on untrusted bytes. nightly-monkey-tester Layer 3.
//
// The parser is a char-by-char '%'-state machine (flags/width/precision/length →
// conversion char), so pointer-walk OOB / UB are the bug classes — surfaced by
// ASan+UBSan under the fuzzer preset. The input is split at a data-derived offset
// into (attacker override, trusted literal) to exercise the comparator the way
// Format() calls it; the whole input is also parsed standalone, and the NULL path
// is hit for null-safety. Header-only closure: no SmatchetLocalization.cpp
// (ConfigManager/Logger) needed.
#include "SmatchetLocalizationFormatGuard.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* p = reinterpret_cast<const char*>(data);
    const std::size_t split = size ? (data[0] % (size + 1)) : 0;
    const std::string translated(p, split);           // attacker-controlled override
    const std::string english(p + split, size - split); // trusted English literal

    (void)smatchet::l10n::FormatSpecifiersMatch(translated.c_str(), english.c_str());
    (void)smatchet::l10n::ConversionSpecifiers(translated.c_str());
    (void)smatchet::l10n::FormatSpecifiersMatch(nullptr, english.c_str());
    return 0;
}
