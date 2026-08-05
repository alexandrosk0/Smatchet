#include <doctest/doctest.h>

#include "PreferencesSchema.h"

#include <cstring>
#include <set>
#include <string>

// Bucket-A structural invariants for the Preferences descriptor table
// (docs/plans/active/preferences-ia-resegmentation-and-search.md, slice 1).
// Assertions are generic — they hold in every feature-guard configuration
// (SMATCHET_WITH_MCP / _AI / _WHISPER on or off), so this TU compiles clean in
// the feature-OFF configure check too. Rendering-side agreement (every
// descriptor draws, labels match) is the bucket-E drift guard's job, not this
// TU's.

using SmatchetPrefsSchema::Categories;
using SmatchetPrefsSchema::FindCategory;
using SmatchetPrefsSchema::FindSection;
using SmatchetPrefsSchema::FindSetting;
using SmatchetPrefsSchema::PrefsCategoryDesc;
using SmatchetPrefsSchema::PrefsSectionDesc;
using SmatchetPrefsSchema::PrefsSettingDesc;
using SmatchetPrefsSchema::Sections;
using SmatchetPrefsSchema::Settings;

namespace {

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

TEST_CASE("PreferencesSchema — categories: present, unique, labelled") {
    std::size_t count = 0;
    const PrefsCategoryDesc* categories = Categories(count);
    REQUIRE(categories != nullptr);
    CHECK(count >= 6); // 8 in a full build; ai_voice is feature-gated

    std::set<std::string> ids;
    for (std::size_t i = 0; i < count; ++i) {
        CAPTURE(categories[i].Id);
        CHECK(categories[i].Id[0] != '\0');
        CHECK(categories[i].TitleEn[0] != '\0');
        CHECK(ids.insert(categories[i].Id).second); // unique
        // Category ids are dotted-id ROOTS — no '.' inside.
        CHECK(std::strchr(categories[i].Id, '.') == nullptr);
    }
}

TEST_CASE("PreferencesSchema — sections: unique, resolve to a category, dotted prefix") {
    std::size_t count = 0;
    const PrefsSectionDesc* sections = Sections(count);
    REQUIRE(sections != nullptr);
    CHECK(count >= 20); // ~27 in a full build; several are feature-gated

    std::set<std::string> ids;
    for (std::size_t i = 0; i < count; ++i) {
        const PrefsSectionDesc& s = sections[i];
        CAPTURE(s.Id);
        CHECK(s.TitleEn[0] != '\0');
        CHECK(ids.insert(s.Id).second);
        CHECK(FindCategory(s.CategoryId) != nullptr);
        // Id is "<CategoryId>.<leaf>" with a non-empty leaf.
        const std::string prefix = std::string(s.CategoryId) + ".";
        CHECK(StartsWith(s.Id, prefix));
        CHECK(std::string(s.Id).size() > prefix.size());
    }
}

TEST_CASE("PreferencesSchema — settings: unique, resolve to a section, dotted prefix, labelled") {
    std::size_t count = 0;
    const PrefsSettingDesc* settings = Settings(count);
    REQUIRE(settings != nullptr);
    CHECK(count >= 70); // ~120 in a full build; large blocks are feature-gated

    std::set<std::string> ids;
    for (std::size_t i = 0; i < count; ++i) {
        const PrefsSettingDesc& s = settings[i];
        CAPTURE(s.Id);
        CHECK(s.LabelEn[0] != '\0');
        CHECK(s.Keywords != nullptr);
        CHECK(ids.insert(s.Id).second);
        CHECK(FindSection(s.SectionId) != nullptr);
        const std::string prefix = std::string(s.SectionId) + ".";
        CHECK(StartsWith(s.Id, prefix));
        CHECK(std::string(s.Id).size() > prefix.size());
    }
}

TEST_CASE("PreferencesSchema — every category hosts at least one section") {
    std::size_t categoryCount = 0;
    const PrefsCategoryDesc* categories = Categories(categoryCount);
    std::size_t sectionCount = 0;
    const PrefsSectionDesc* sections = Sections(sectionCount);

    for (std::size_t c = 0; c < categoryCount; ++c) {
        CAPTURE(categories[c].Id);
        bool hasSection = false;
        for (std::size_t s = 0; s < sectionCount; ++s) {
            if (std::strcmp(sections[s].CategoryId, categories[c].Id) == 0) {
                hasSection = true;
                break;
            }
        }
        CHECK(hasSection);
    }
}

TEST_CASE("PreferencesSchema — every section hosts at least one setting") {
    std::size_t sectionCount = 0;
    const PrefsSectionDesc* sections = Sections(sectionCount);
    std::size_t settingCount = 0;
    const PrefsSettingDesc* settings = Settings(settingCount);

    for (std::size_t s = 0; s < sectionCount; ++s) {
        CAPTURE(sections[s].Id);
        bool hasSetting = false;
        for (std::size_t i = 0; i < settingCount; ++i) {
            if (std::strcmp(settings[i].SectionId, sections[s].Id) == 0) {
                hasSetting = true;
                break;
            }
        }
        CHECK(hasSetting);
    }
}

TEST_CASE("PreferencesSchema — Find* round-trips and unknown-id nullptr") {
    std::size_t count = 0;
    const PrefsSectionDesc* sections = Sections(count);
    REQUIRE(count > 0);
    CHECK(FindSection(sections[0].Id) == &sections[0]);

    const PrefsSettingDesc* settings = Settings(count);
    REQUIRE(count > 0);
    CHECK(FindSetting(settings[0].Id) == &settings[0]);

    CHECK(FindCategory("no.such.category") == nullptr);
    CHECK(FindSection("no.such.section") == nullptr);
    CHECK(FindSetting("no.such.setting") == nullptr);
    CHECK(FindCategory(nullptr) == nullptr);
    CHECK(FindSection(nullptr) == nullptr);
    CHECK(FindSetting(nullptr) == nullptr);
}
