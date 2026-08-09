#include <doctest/doctest.h>

#include "PreferencesFilter.h"
#include "PreferencesSchema.h"

#include <string>

// Bucket-A coverage for the Preferences live-search filter
// (docs/plans/shipped/preferences-ia-resegmentation-and-search.md, slice 1).
// The rig never calls SmatchetLocalization::SetLanguage — the global language
// stays at the English default, so TranslateSource is identity here. The
// language argument to Update() is exercised only as a rebuild trigger.

namespace {

/// A settings id present in every feature-guard configuration.
const char* const kVsyncId = "appearance.display.vsync";
const char* const kUpdatesId = "general.updates.check_enabled";

} // namespace

TEST_CASE("PreferencesFilter — empty query: inactive, everything shows, no lookups") {
    PreferencesFilter filter;
    filter.Update("", "en");
    CHECK_FALSE(filter.Active());
    CHECK(filter.MatchCount() == 0);
    CHECK(filter.TotalCount() >= 70);
    CHECK(filter.ShowSetting(kVsyncId));
    CHECK(filter.ShowSetting(kUpdatesId));
    // Roll-ups report "has matches" while inactive so nothing gets hidden.
    CHECK(filter.SectionHasMatch("appearance.display"));
    CHECK(filter.CategoryHasMatch("appearance"));
    // nullptr query behaves like empty.
    filter.Update(nullptr, "en");
    CHECK_FALSE(filter.Active());
    CHECK(filter.ShowSetting(kVsyncId));
}

TEST_CASE("PreferencesFilter — substring match shows the hit, hides the rest") {
    PreferencesFilter filter;
    filter.Update("vsync", "en");
    CHECK(filter.Active());
    CHECK(filter.ShowSetting(kVsyncId));
    CHECK_FALSE(filter.ShowSetting(kUpdatesId));
    CHECK(filter.MatchCount() >= 1);
    CHECK(filter.MatchCount() < filter.TotalCount());
}

TEST_CASE("PreferencesFilter — matching is case-insensitive") {
    PreferencesFilter filter;
    filter.Update("VSYNC", "en");
    CHECK(filter.ShowSetting(kVsyncId));
    filter.Update("vSyNc", "en");
    CHECK(filter.ShowSetting(kVsyncId));
}

TEST_CASE("PreferencesFilter — ToLowerFold folds accented letters, not just ASCII") {
    // The haystack is built from TranslateSource output, i.e. TRANSLATED text, so
    // an ASCII-only fold left "É" (0xC3 0x89) and "é" (0xC3 0xA9) unequal and a
    // French user typing "PRÉ" never matched "préférences". Byte-level here
    // because the fold is a byte rule over the Latin-1 Supplement block.
    SUBCASE("uppercase accented input folds to the lowercase bytes") {
        CHECK(PreferencesFilter::ToLowerFold("PR\xC3\x89") == "pr\xC3\xA9");
        CHECK(PreferencesFilter::ToLowerFold("\xC3\x80\xC3\x87\xC3\x9C") == "\xC3\xA0\xC3\xA7\xC3\xBC");
    }
    SUBCASE("already-lowercase accented text is unchanged (idempotent)") {
        const std::string lowered = PreferencesFilter::ToLowerFold("pr\xC3\xA9"
                                                                   "f\xC3\xA9rences");
        CHECK(lowered == "pr\xC3\xA9"
                         "f\xC3\xA9rences");
        CHECK(PreferencesFilter::ToLowerFold(lowered) == lowered);
    }
    SUBCASE("ASCII still folds exactly as before") {
        CHECK(PreferencesFilter::ToLowerFold("VSYNC") == "vsync");
        CHECK(PreferencesFilter::ToLowerFold("") == "");
    }
    SUBCASE("U+00D7 multiplication sign is punctuation, not a letter — never shifted") {
        // It sits inside 0xC3 0x80-0x9E but +0x20 would turn it into U+00F7 (÷).
        CHECK(PreferencesFilter::ToLowerFold("\xC3\x97") == "\xC3\x97");
    }
    SUBCASE("a lone 0xC3 at the end is not read past") { CHECK(PreferencesFilter::ToLowerFold("ab\xC3") == "ab\xC3"); }
    SUBCASE("ContainsLower matches through the fold") {
        CHECK(PreferencesFilter::ContainsLower("Pr\xC3\x89"
                                               "f\xC3\x89rences g\xC3\x89n\xC3\x89rales",
                                               PreferencesFilter::ToLowerFold("PR\xC3\x89"
                                                                              "F")));
    }
}

TEST_CASE("PreferencesFilter — keywords keep old-tab muscle memory working") {
    PreferencesFilter filter;
    // "Check for updates automatically" moved Appearance -> General; its
    // keyword bag still carries the old tab name.
    filter.Update("appearance", "en");
    CHECK(filter.ShowSetting(kUpdatesId));
    // "user info" tab dissolved into Connections > Activity sources.
    filter.Update("user info", "en");
    CHECK(filter.ShowSetting("connections.activity.git_repos"));
    CHECK_FALSE(filter.ShowSetting(kVsyncId));
}

TEST_CASE("PreferencesFilter — section and category titles are part of the haystack") {
    PreferencesFilter filter;
    filter.Update("change notifications", "en");
    CHECK(filter.ShowSetting("tracker.notifications.enabled"));
    CHECK(filter.ShowSetting("tracker.notifications.interval"));
}

TEST_CASE("PreferencesFilter — fuzzy fallback fires only when substring finds nothing") {
    PreferencesFilter filter;
    // "vsnc" is a substring of no haystack -> tier 2 subsequence still lands
    // on vsync.
    filter.Update("vsnc", "en");
    CHECK(filter.Active());
    CHECK(filter.ShowSetting(kVsyncId));
    // A query with substring hits must NOT pick up fuzzy-only matches:
    // "port" substring-matches rows like "Max changes per source" ("...per
    // source" contains no "port"? it does not — but "Import existing" does),
    // while a pure-subsequence-only candidate such as "p...o...r...t" spread
    // across an unrelated label stays hidden. Assert via count stability:
    // adding fuzzy would strictly grow the match set.
    filter.Update("port", "en");
    const std::size_t substringOnly = filter.MatchCount();
    CHECK(substringOnly >= 1);
    // Every reported match must actually contain the substring — probe one
    // known non-substring id that a subsequence scan would match ("p","o",
    // "r","t" occur in order in "Production group keyword").
    CHECK_FALSE(filter.ShowSetting("connections.activity.production_keyword"));
}

TEST_CASE("PreferencesFilter — no-match query hides everything, unknown ids fail open") {
    PreferencesFilter filter;
    filter.Update("zzzzqqq", "en");
    CHECK(filter.Active());
    CHECK(filter.MatchCount() == 0);
    CHECK_FALSE(filter.ShowSetting(kVsyncId));
    CHECK_FALSE(filter.SectionHasMatch("appearance.display"));
    CHECK_FALSE(filter.CategoryHasMatch("appearance"));
    CHECK(filter.CategoryMatchCount("appearance") == 0);
    // Unknown id: fail open — the drift guard reports it, the filter must not
    // hide a live widget.
    CHECK(filter.ShowSetting("not.a.real.id"));
}

TEST_CASE("PreferencesFilter — section/category roll-ups follow the matches") {
    PreferencesFilter filter;
    filter.Update("vsync", "en");
    CHECK(filter.SectionHasMatch("appearance.display"));
    CHECK_FALSE(filter.SectionHasMatch("general.updates"));
    CHECK(filter.CategoryHasMatch("appearance"));
    CHECK(filter.CategoryMatchCount("appearance") >= 1);
    CHECK(filter.CategoryMatchCount("annotate") == 0);
    CHECK_FALSE(filter.CategoryHasMatch("annotate"));
}

TEST_CASE("PreferencesFilter — language change triggers a rebuild, not a crash") {
    PreferencesFilter filter;
    filter.Update("vsync", "en");
    CHECK(filter.ShowSetting(kVsyncId));
    // Global localization state is still English; a different language string
    // must rebuild the index and keep matching consistent.
    filter.Update("vsync", "fr");
    CHECK(filter.ShowSetting(kVsyncId));
    filter.Update("vsync", "en");
    CHECK(filter.ShowSetting(kVsyncId));
}

TEST_CASE("PreferencesFilter — observed-id set accumulates and resets") {
    PreferencesFilter filter;
    filter.Update("", "en");
    // Recording is opt-in (off on the production draw path), so the guard's own
    // switch is part of what this covers: nothing accumulates until it is on.
    filter.ShowSetting(kVsyncId);
    CHECK(filter.ObservedSettingIds().empty());
    filter.SetRecordObservedSettingIds(true);
    filter.ResetObservedSettingIds();
    CHECK(filter.ObservedSettingIds().empty());
    filter.ShowSetting(kVsyncId);
    filter.ShowSetting(kUpdatesId);
    filter.ShowSetting(kVsyncId); // dedup
    CHECK(filter.ObservedSettingIds().size() == 2);
    CHECK(filter.ObservedSettingIds().count(kVsyncId) == 1);
    CHECK(filter.ObservedSettingIds().count(kUpdatesId) == 1);
    // Accumulates across query changes (guard walks multiple frames)...
    filter.Update("vsync", "en");
    filter.ShowSetting("appearance.theme_font.font");
    CHECK(filter.ObservedSettingIds().size() == 3);
    // ...until explicitly reset.
    filter.ResetObservedSettingIds();
    CHECK(filter.ObservedSettingIds().empty());
}

TEST_CASE("PreferencesFilter — RecomputeMatchesNow drops a stale dynamic match") {
    // Regression: a dynamic match injected for the keybindings
    // rows persists in the match set until the next query/language recompute —
    // Update() with an unchanged query skips RecomputeMatches(). If the mutation
    // that invalidated the injection (clearing the last matching hotkey) happens
    // without a query edit, the caller must be able to rebuild the base match set
    // so the section does not stay visible with zero matching rows.
    PreferencesFilter filter;
    const char* const kBindingsId = "shortcuts.keyboard.bindings";
    // A query that hits no schema descriptor (substring AND fuzzy tiers), so the
    // dynamic injection is the only thing keeping the section alive.
    filter.Update("zzzzqqqq", "en");
    REQUIRE(filter.Active());
    REQUIRE_FALSE(filter.ShowSetting(kBindingsId));

    filter.AddDynamicMatch(kBindingsId);
    CHECK(filter.ShowSetting(kBindingsId));
    CHECK(filter.CategoryMatchCount("shortcuts") == 1);

    // Same query, same language: Update() must NOT drop the injection (this is
    // the steady-state the draw loop relies on between edits)...
    filter.Update("zzzzqqqq", "en");
    CHECK_FALSE(filter.MatchesRecomputedThisUpdate());
    CHECK(filter.ShowSetting(kBindingsId));

    // ...which is exactly why the mutation edge needs the explicit rebuild.
    filter.RecomputeMatchesNow();
    CHECK_FALSE(filter.ShowSetting(kBindingsId));
    CHECK(filter.CategoryMatchCount("shortcuts") == 0);

    // The caller's follow-up scan re-injects when rows still match.
    filter.AddDynamicMatch(kBindingsId);
    CHECK(filter.ShowSetting(kBindingsId));
}
