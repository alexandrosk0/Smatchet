#pragma once

#include <string>
#include <vector>

namespace SmatchetLocalization {

struct LanguageInfo {
    const char* Code;
    const char* DisplayName;
};

const std::vector<LanguageInfo>& AvailableLanguages();
std::string NormalizeLanguageCode(const std::string& code);
void SetLanguage(const std::string& code);
// No GetLanguage() accessor by design: the language string is mutated by SetLanguage under
// LocalizationMutex(), so a `const std::string&` getter would hand callers a reference into
// mutex-protected state and race with the next SetLanguage. The previous one did exactly that and
// had zero callers, so it was deleted rather than fixed (gate-blind-spot-sweep Slice 1b). A future
// caller must return BY VALUE under the lock, like every other accessor in this file.

const char* T(const char* key, const char* englishFallback);
const char* TranslateSource(const char* englishSource);

/// Like `TranslateSource`, but for callers that hand the result straight to a printf-family
/// sink (the `SmatchetLocalizedImGui` `Text*`/`SetTooltip`/`SliderInt` wrappers). A locale
/// override (Locales/<lang>.json) is attacker-influenceable; if its conversion-specifier
/// sequence doesn't exactly match the trusted `englishSource` literal, using it as a format
/// string would read/write varargs that don't exist (OOB read / `%n` write). Returns the
/// override when the specifiers match, `englishSource` itself otherwise — mirrors `Format`'s
/// existing specifier-match guard (CPP_CODE_AUDIT.md #7: `Format`'s guard covers keyed
/// translations but not this `TranslateSource` path, a distinct set of sinks).
const char* TranslateSourceAsFormat(const char* englishSource);

const char* Label(const char* key, const char* englishFallback, const char* stableId);
const char* WindowTitle(const char* key, const char* englishFallback, const char* stableId);
const char* LabelFromSource(const char* label);
const char* WindowTitleFromSource(const char* title);

const char* Format(const char* key, const char* englishFallbackFmt, ...);

} // namespace SmatchetLocalization
