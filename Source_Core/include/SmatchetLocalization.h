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
const std::string& GetLanguage();

const char* T(const char* key, const char* englishFallback);
const char* TranslateSource(const char* englishSource);

const char* Label(const char* key, const char* englishFallback, const char* stableId);
const char* WindowTitle(const char* key, const char* englishFallback, const char* stableId);
const char* LabelFromSource(const char* label);
const char* WindowTitleFromSource(const char* title);

const char* Format(const char* key, const char* englishFallbackFmt, ...);

} // namespace SmatchetLocalization

