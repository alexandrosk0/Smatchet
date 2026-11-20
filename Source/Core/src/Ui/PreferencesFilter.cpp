#include "PreferencesFilter.h"

#include "PreferencesSchema.h"
#include "SmatchetLocalization.h"
#include "TextMatchPure.h"

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

// Search fold: the shared matcher's UTF-8 case fold (TextMatchPure.h). The haystack is built from
// TranslateSource output, i.e. translated text, so an ASCII-only fold would leave "É" and "é" unequal and a
// French user typing "PRÉ" would never match "préférences". The fold covers Latin-1, Latin Extended-A,
// Greek, Cyrillic and the other blocks whose case mapping is regular; anything else compares as written.
std::string PreferencesFilter::ToLowerFold(const std::string& s) { return smatchet::textmatch::FoldCopy(s); }

bool PreferencesFilter::ContainsLower(const std::string& haystack, const std::string& needleLower) {
    return ToLowerFold(haystack).find(needleLower) != std::string::npos;
}

void PreferencesFilter::Update(const char* query, const std::string& uiLanguage) {
    focusFirstMatchHook_ = nullptr; // a request lives for one draw
    const bool languageChanged = !indexBuilt_ || uiLanguage != indexedLanguage_;
    if (languageChanged) {
        indexedLanguage_ = uiLanguage;
        RebuildIndex();
    }
    const std::string incoming = query != nullptr ? query : "";
    queryChanged_ = incoming != queryRaw_;
    matchesRecomputed_ = languageChanged || queryChanged_;
    if (matchesRecomputed_) {
        queryRaw_ = incoming;
        queryLower_ = ToLowerFold(incoming);
        RecomputeMatches();
    }
}

bool PreferencesFilter::ShowSetting(const char* id) {
    if (id == nullptr) {
        return true;
    }
    if (recordObserved_) {
        observed_.insert(id);
    }
    if (queryLower_.empty()) {
        return true;
    }
    if (SmatchetPrefsSchema::FindSetting(id) == nullptr) {
        return true; // fail open — the drift guard owns reporting unknown ids
    }
    const bool shown = matched_.count(id) != 0;
    if (shown && focusFirstMatchHook_ != nullptr) {
        void (*hook)() = focusFirstMatchHook_;
        focusFirstMatchHook_ = nullptr; // once: disarm before calling, so a re-entrant ShowSetting cannot fire it
        hook();
    }
    return shown;
}

void PreferencesFilter::AddDynamicMatch(const char* settingId) {
    if (settingId == nullptr || queryLower_.empty()) {
        return;
    }
    const SmatchetPrefsSchema::PrefsSettingDesc* setting = SmatchetPrefsSchema::FindSetting(settingId);
    if (setting == nullptr || !matched_.insert(settingId).second) {
        return;
    }
    sectionMatches_.insert(setting->SectionId);
    const SmatchetPrefsSchema::PrefsSectionDesc* section = SmatchetPrefsSchema::FindSection(setting->SectionId);
    if (section != nullptr) {
        ++categoryMatches_[section->CategoryId];
    }
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
    const SmatchetPrefsSchema::PrefsSettingDesc* settings = SmatchetPrefsSchema::Settings(settingCount);
    index_.reserve(settingCount);
    for (std::size_t i = 0; i < settingCount; ++i) {
        const SmatchetPrefsSchema::PrefsSettingDesc& setting = settings[i];
        const SmatchetPrefsSchema::PrefsSectionDesc* section = SmatchetPrefsSchema::FindSection(setting.SectionId);
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
        entry.Haystack = ToLowerFold(haystack);
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
    // Tier 1: every word of the query occurs in the pre-folded haystack, in any order.
    const smatchet::textmatch::Query query = smatchet::textmatch::MakeQuery(queryLower_);
    for (const Entry& entry : index_) {
        if (smatchet::textmatch::ContainsAllWordsFolded(entry.Haystack, query)) {
            matched_.insert(entry.Id);
        }
    }
    // Tier 2: letters in order, only when the exact pass found nothing — a typo like "vsnc" still lands,
    // but fuzzy noise never joins exact hits (the same rule every picker list follows).
    if (matched_.empty()) {
        smatchet::textmatch::Scratch scratch;
        for (const Entry& entry : index_) {
            if (smatchet::textmatch::Match(query, entry.Haystack.data(), entry.Haystack.size(),
                                           smatchet::textmatch::Mode::Fuzzy, scratch)
                    .Matched()) {
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
