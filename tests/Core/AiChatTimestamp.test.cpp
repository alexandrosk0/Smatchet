#include <doctest/doctest.h>

#include "AiChatTimestamp.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

using smatchet::ai::FormatAbsoluteTime;
using smatchet::ai::FormatRelativeTime;

namespace {

// Convenience: build a "now" anchor and offset it by various exact spans so
// every boundary in the plan is pinned by an integer ms count rather than a
// wall-clock-derived value that might wobble between platforms.
constexpr std::int64_t kNow = 1'700'000'000'000LL; // arbitrary mid-2023 anchor (UTC)

constexpr std::int64_t kSecond = 1'000LL;
constexpr std::int64_t kMinute = 60LL * kSecond;
constexpr std::int64_t kHour = 60LL * kMinute;
constexpr std::int64_t kDay = 24LL * kHour;

} // namespace

TEST_CASE("FormatRelativeTime — bucket boundaries (plan-pinned)") {
    SUBCASE("delta == 0 -> just now") { CHECK(FormatRelativeTime(kNow, kNow) == "just now"); }
    SUBCASE("delta == 59s -> just now (just below the 60s boundary)") {
        CHECK(FormatRelativeTime(kNow, kNow - 59 * kSecond) == "just now");
    }
    SUBCASE("delta == 60s -> 1m ago (exactly on the boundary)") {
        CHECK(FormatRelativeTime(kNow, kNow - 60 * kSecond) == "1m ago");
    }
    SUBCASE("delta == 59m59s -> 59m ago (just below the 60m boundary)") {
        const std::int64_t then = kNow - (59 * kMinute + 59 * kSecond);
        CHECK(FormatRelativeTime(kNow, then) == "59m ago");
    }
    SUBCASE("delta == 60m -> 1h ago (exactly on the boundary)") {
        CHECK(FormatRelativeTime(kNow, kNow - kHour) == "1h ago");
    }
    SUBCASE("delta == 23h59m -> 23h ago (just below the 24h boundary)") {
        const std::int64_t then = kNow - (23 * kHour + 59 * kMinute);
        CHECK(FormatRelativeTime(kNow, then) == "23h ago");
    }
    SUBCASE("delta == 24h -> 1d ago (exactly on the boundary)") {
        CHECK(FormatRelativeTime(kNow, kNow - kDay) == "1d ago");
    }
    SUBCASE("delta == 7d -> 7d ago (well past the day boundary)") {
        CHECK(FormatRelativeTime(kNow, kNow - 7 * kDay) == "7d ago");
    }
}

TEST_CASE("FormatRelativeTime — future-time guard clamps to just now") {
    SUBCASE("then 1 second ahead of now (sub-bucket sliver)") {
        CHECK(FormatRelativeTime(kNow, kNow + kSecond) == "just now");
    }
    SUBCASE("then 1 hour ahead of now (gross clock skew)") {
        CHECK(FormatRelativeTime(kNow, kNow + kHour) == "just now");
    }
    SUBCASE("then 1 day ahead of now (server with bad clock)") {
        CHECK(FormatRelativeTime(kNow, kNow + kDay) == "just now");
    }
}

TEST_CASE("FormatRelativeTime — integer-truncation rounding (no rounding-up surprises)") {
    SUBCASE("119 seconds -> 1m ago (not 2m)") { CHECK(FormatRelativeTime(kNow, kNow - 119 * kSecond) == "1m ago"); }
    SUBCASE("1h 59m -> 1h ago (not 2h)") {
        const std::int64_t then = kNow - (1 * kHour + 59 * kMinute);
        CHECK(FormatRelativeTime(kNow, then) == "1h ago");
    }
    SUBCASE("47h 59m -> 1d ago (not 2d)") {
        const std::int64_t then = kNow - (47 * kHour + 59 * kMinute);
        CHECK(FormatRelativeTime(kNow, then) == "1d ago");
    }
}

TEST_CASE("FormatAbsoluteTime — fixed-shape output") {
    // The output is local-TZ dependent, so we can't pin the exact characters
    // without freezing TZ for the test. Instead, format a known instant and
    // assert structural properties that hold in every TZ:
    //   * length is exactly strlen("HH:MM:SS DD-Mon-YYYY") = 20
    //   * positions 2 and 5 are ':'
    //   * position 8 is ' '
    //   * positions 11 and 15 are '-'
    //   * the month slice (positions 12..14) is one of the 12 known abbrevs
    const std::string s = FormatAbsoluteTime(kNow);
    REQUIRE(s.size() == 20);
    CHECK(s[2] == ':');
    CHECK(s[5] == ':');
    CHECK(s[8] == ' ');
    CHECK(s[11] == '-');
    CHECK(s[15] == '-');

    const std::string month = s.substr(12, 3);
    static const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    bool monthMatches = false;
    for (int i = 0; i < 12; ++i) {
        if (month == kMonths[i]) {
            monthMatches = true;
            break;
        }
    }
    CHECK(monthMatches);
}

TEST_CASE("FormatAbsoluteTime — round-trips through localtime so the year is sane") {
    // A second independent assertion that the formatter's year matches what
    // localtime would have produced — pins the "tm_year + 1900" offset so a
    // future refactor can't accidentally drop the +1900.
    const std::time_t seconds = static_cast<std::time_t>(kNow / 1000);
    std::tm reference{};
#if defined(_WIN32)
    REQUIRE(::localtime_s(&reference, &seconds) == 0);
#else
    REQUIRE(::localtime_r(&seconds, &reference) != nullptr);
#endif

    const std::string s = FormatAbsoluteTime(kNow);
    REQUIRE(s.size() == 20);

    char expectedYear[8];
    std::snprintf(expectedYear, sizeof(expectedYear), "%04d", reference.tm_year + 1900);
    CHECK(s.substr(16, 4) == std::string(expectedYear));
}
