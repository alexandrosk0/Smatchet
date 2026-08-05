#include "PreferencesFilter.h"

#include "Commands/FuzzyMatch.h"
#include "PreferencesSchema.h"
#include "SmatchetLocalization.h"

#include <cctype>

namespace {

/// The rendered label up to (not including) the first "##" — ImGui id suffixes
/// ("Label##unique") must never leak into the search haystack.
std::string VisibleLabel(const char* labelEn) {
    const char* translated = SmatchetLocalization::TranslateSource(labelEn);
    std::string label = translated != nullptr ? translated : labelEn;
    const std::size_t idSep = label.find("##");
    if (idSep != std::string::npos) {
        label.resize(idSep);
    }
    return label;
}

} // namespace

std::string PreferencesFilter::ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool PreferencesFilter::ContainsLower(const std::string& haystack, const std::string& needleLower) {
    return ToLowerAscii(haystack).find(needleLower) != std::string::npos;
}

void PreferencesFilter::Update(const char* query, const std::string& uiLanguage) {
    const bool languageChanged = !indexBuilt_ || uiLanguage != indexedLanguage_;
    if (languageChanged) {
        indexedLanguage_ = uiLanguage;
        RebuildIndex();
    }
    const std::string incoming = query != nullptr ? query : "";
    if (languageChanged || incoming != queryRaw_) {
        queryRaw_ = incoming;
        queryLower_ = ToLowerAscii(incoming);
        RecomputeMatches();
    }
}

bool PreferencesFilter::ShowSetting(const char* id) {
    if (id == nullptr) {
        return true;
    }
    observed_.insert(id);
    if (queryLower_.empty()) {
        return true;
    }
    if (SmatchetPrefsSchema::FindSetting(id) == nullptr) {
        return true; // fail open — the drift guard owns reporting unknown ids
    }
    return matched_.count(id) != 0;
}

std::size_t PreferencesFilter::TotalCount() const {
    std::size_t count = 0;
    SmatchetPrefsSchema::Settings(count);
    return count;
}

bool PreferencesFilter::SectionHasMatch(const char* sectionId) const {
    if (queryLower_.empty()) {
        return true;
    }
    return sectionId != nullptr && sectionMatches_.count(sectionId) != 0;
}

bool PreferencesFilter::CategoryHasMatch(const char* categoryId) const {
    if (queryLower_.empty()) {
        return true;
    }
    return categoryId != nullptr && categoryMatches_.count(categoryId) != 0;
}

std::size_t PreferencesFilter::CategoryMatchCount(const char* categoryId) const {
    if (categoryId == nullptr) {
        return 0;
    }
    const auto it = categoryMatches_.find(categoryId);
    return it != categoryMatches_.end() ? it->second : 0;
}

void PreferencesFilter::RebuildIndex() {
    index_.clear();
    std::size_t settingCount = 0;
    const SmatchetPrefsSchema::PrefsSettingDesc* settings =
        SmatchetPrefsSchema::Settings(settingCount);
    index_.reserve(settingCount);
    for (std::size_t i = 0; i < settingCount; ++i) {
        const SmatchetPrefsSchema::PrefsSettingDesc& setting = settings[i];
        const SmatchetPrefsSchema::PrefsSectionDesc* section =
            SmatchetPrefsSchema::FindSection(setting.SectionId);
        Entry entry;
        entry.Id = setting.Id;
        entry.SectionId = setting.SectionId;
        entry.CategoryId = section != nullptr ? section->CategoryId : "";
        std::string haystack = VisibleLabel(setting.LabelEn);
        if (setting.Keywords != nullptr && setting.Keywords[0] != '\0') {
            haystack += ' ';
            haystack += setting.Keywords;
        }
        if (section != nullptr) {
            haystack += ' ';
            haystack += VisibleLabel(section->TitleEn);
            const SmatchetPrefsSchema::PrefsCategoryDesc* category =
                SmatchetPrefsSchema::FindCategory(section->CategoryId);
            if (category != nullptr) {
                haystack += ' ';
                haystack += VisibleLabel(category->TitleEn);
            }
        }
        entry.Haystack = ToLowerAscii(haystack);
        index_.push_back(entry);
    }
    indexBuilt_ = true;
}

void PreferencesFilter::RecomputeMatches() {
    matched_.clear();
    sectionMatches_.clear();
    categoryMatches_.clear();
    if (queryLower_.empty()) {
        return;
    }
    // Tier 1: plain substring over the pre-lowered haystacks.
    for (const Entry& entry : index_) {
        if (entry.Haystack.find(queryLower_) != std::string::npos) {
            matched_.insert(entry.Id);
        }
    }
    // Tier 2: fuzzy subsequence, only when the substring pass found nothing —
    // a typo like "vsnc" still lands, but fuzzy noise never joins exact hits.
    if (matched_.empty()) {
        for (const Entry& entry : index_) {
            if (smatchet::cmd::FuzzyScore(queryLower_, entry.Haystack) > 0) {
                matched_.insert(entry.Id);
            }
        }
    }
    for (const Entry& entry : index_) {
        if (matched_.count(entry.Id) != 0) {
            sectionMatches_.insert(entry.SectionId);
            ++categoryMatches_[entry.CategoryId];
        }
    }
}
