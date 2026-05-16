#include <doctest/doctest.h>

#include "TrackerLabelsPure.h"

#include <string>
#include <vector>

using TrackerLabelsPure::ContainsLabelCaseInsensitive;
using TrackerLabelsPure::FilterSuggestionsForDisplay;
using TrackerLabelsPure::LabelMatchesFilter;
using TrackerLabelsPure::LabelsEqualCaseInsensitive;
using TrackerLabelsPure::ParseCsv;
using TrackerLabelsPure::SortAndUniqueLabels;

TEST_CASE("ParseCsv: split + trim + drop-empty") {
    SUBCASE("simple three-element csv") {
        const auto out = ParseCsv("alpha,beta,gamma");
        REQUIRE(out.size() == 3);
        CHECK(out[0] == "alpha");
        CHECK(out[1] == "beta");
        CHECK(out[2] == "gamma");
    }

    SUBCASE("trims whitespace around each entry") {
        const auto out = ParseCsv("  alpha , \tbeta\r ,\n gamma  ");
        REQUIRE(out.size() == 3);
        CHECK(out[0] == "alpha");
        CHECK(out[1] == "beta");
        CHECK(out[2] == "gamma");
    }

    SUBCASE("drops empty / whitespace-only parts (leading, trailing, double-comma)") {
        const auto out = ParseCsv(",alpha,,  ,beta,");
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "alpha");
        CHECK(out[1] == "beta");
    }

    SUBCASE("empty input yields empty vector") {
        const auto out = ParseCsv("");
        CHECK(out.empty());
    }

    SUBCASE("whitespace-only input yields empty vector") {
        const auto out = ParseCsv("   \t\r\n  ");
        CHECK(out.empty());
    }

    SUBCASE("single label without comma") {
        const auto out = ParseCsv("solo");
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "solo");
    }

    SUBCASE("preserves embedded spaces inside a label") {
        const auto out = ParseCsv("hello world,foo bar");
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "hello world");
        CHECK(out[1] == "foo bar");
    }
}

TEST_CASE("LabelsEqualCaseInsensitive: ASCII fold equality") {
    CHECK(LabelsEqualCaseInsensitive("Alpha", "alpha"));
    CHECK(LabelsEqualCaseInsensitive("BUG", "bug"));
    CHECK(LabelsEqualCaseInsensitive("MixedCase", "mIXEDcASE"));
    CHECK_FALSE(LabelsEqualCaseInsensitive("alpha", "beta"));
    CHECK_FALSE(LabelsEqualCaseInsensitive("alpha", "alpha "));
    CHECK(LabelsEqualCaseInsensitive("", ""));
    CHECK_FALSE(LabelsEqualCaseInsensitive("a", ""));
}

TEST_CASE("ContainsLabelCaseInsensitive: any-of under case-insensitive equality") {
    const std::vector<std::string> values = {"Alpha", "Beta", "GAMMA"};

    SUBCASE("matches under fold") {
        CHECK(ContainsLabelCaseInsensitive(values, "alpha"));
        CHECK(ContainsLabelCaseInsensitive(values, "BETA"));
        CHECK(ContainsLabelCaseInsensitive(values, "gamma"));
        CHECK(ContainsLabelCaseInsensitive(values, "GAMMA"));
    }

    SUBCASE("no match returns false") {
        CHECK_FALSE(ContainsLabelCaseInsensitive(values, "delta"));
        CHECK_FALSE(ContainsLabelCaseInsensitive(values, ""));
    }

    SUBCASE("empty input vector never matches") {
        CHECK_FALSE(ContainsLabelCaseInsensitive({}, "alpha"));
        CHECK_FALSE(ContainsLabelCaseInsensitive({}, ""));
    }

    SUBCASE("empty needle matches empty entry") {
        const std::vector<std::string> withEmpty = {"foo", ""};
        CHECK(ContainsLabelCaseInsensitive(withEmpty, ""));
    }
}

TEST_CASE("SortAndUniqueLabels: trim + sort + dedupe (case-insensitive)") {
    SUBCASE("dedupe collapses case-equivalent entries") {
        const auto out = SortAndUniqueLabels({"Bug", "bug", "BUG", "feature"});
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "BUG");
        CHECK(out[1] == "feature");
    }

    SUBCASE("sorts case-insensitively") {
        const auto out = SortAndUniqueLabels({"zebra", "Alpha", "mango"});
        REQUIRE(out.size() == 3);
        CHECK(out[0] == "Alpha");
        CHECK(out[1] == "mango");
        CHECK(out[2] == "zebra");
    }

    SUBCASE("strips whitespace + drops empty/whitespace-only entries") {
        const auto out = SortAndUniqueLabels({"  alpha  ", "\t", "", "beta\n", "  "});
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "alpha");
        CHECK(out[1] == "beta");
    }

    SUBCASE("empty input yields empty output") {
        const auto out = SortAndUniqueLabels({});
        CHECK(out.empty());
    }

    SUBCASE("single-element input passes through") {
        const auto out = SortAndUniqueLabels({"solo"});
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "solo");
    }

    SUBCASE("idempotent on already-normalised input") {
        const std::vector<std::string> normalised = {"alpha", "beta", "gamma"};
        const auto out = SortAndUniqueLabels(normalised);
        REQUIRE(out.size() == 3);
        CHECK(out[0] == "alpha");
        CHECK(out[1] == "beta");
        CHECK(out[2] == "gamma");
    }
}

TEST_CASE("ParseCsv + SortAndUniqueLabels round-trip") {
    SUBCASE("dup-detect across csv splits") {
        const auto out = SortAndUniqueLabels(ParseCsv("Bug, feature , BUG , bug"));
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "BUG");
        CHECK(out[1] == "feature");
    }

    SUBCASE("trailing-comma + whitespace-only chunks round-trip cleanly") {
        const auto out = SortAndUniqueLabels(ParseCsv(" alpha,  ,beta,, "));
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "alpha");
        CHECK(out[1] == "beta");
    }
}

TEST_CASE("LabelMatchesFilter: empty filter matches all; substring otherwise") {
    SUBCASE("empty filter passes any label, including empty") {
        CHECK(LabelMatchesFilter("anything", ""));
        CHECK(LabelMatchesFilter("", ""));
    }

    SUBCASE("substring match is case-insensitive on label side") {
        CHECK(LabelMatchesFilter("BugFix", "bug"));
        CHECK(LabelMatchesFilter("feature-flag", "flag"));
        CHECK(LabelMatchesFilter("Alpha", "alpha"));
    }

    SUBCASE("non-match returns false") {
        CHECK_FALSE(LabelMatchesFilter("alpha", "beta"));
        CHECK_FALSE(LabelMatchesFilter("", "x"));
    }
}

TEST_CASE("FilterSuggestionsForDisplay: union of selected-set + substring match") {
    const std::vector<std::string> suggestions = {"Alpha", "Beta", "Gamma", "Delta"};

    SUBCASE("empty filter returns every suggestion") {
        const auto out = FilterSuggestionsForDisplay(suggestions, {}, "");
        REQUIRE(out.size() == 4);
        CHECK(out[0] == "Alpha");
        CHECK(out[3] == "Delta");
    }

    SUBCASE("filter narrows to substring match (case-insensitive)") {
        const auto out = FilterSuggestionsForDisplay(suggestions, {}, "a");
        REQUIRE(out.size() == 4);
    }

    SUBCASE("filter narrows correctly with no selected") {
        const auto out = FilterSuggestionsForDisplay(suggestions, {}, "gam");
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "Gamma");
    }

    SUBCASE("selected suggestion stays visible even when filter excludes it") {
        const std::vector<std::string> selected = {"Beta"};
        const auto out = FilterSuggestionsForDisplay(suggestions, selected, "gam");
        REQUIRE(out.size() == 2);
        CHECK(out[0] == "Beta");
        CHECK(out[1] == "Gamma");
    }

    SUBCASE("selected-side fold is case-insensitive") {
        const std::vector<std::string> selected = {"BETA"};
        const auto out = FilterSuggestionsForDisplay(suggestions, selected, "zzz");
        REQUIRE(out.size() == 1);
        CHECK(out[0] == "Beta");
    }

    SUBCASE("empty suggestions yields empty output") {
        const auto out = FilterSuggestionsForDisplay({}, {"Beta"}, "");
        CHECK(out.empty());
    }
}
