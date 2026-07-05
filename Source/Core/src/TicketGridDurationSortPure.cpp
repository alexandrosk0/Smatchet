#include "TicketGridDurationSortPure.h"

#include <cctype>
#include <limits>

namespace {

constexpr long long kSecondsPerHour = 3600;
constexpr long long kSecondsPerDay = 8 * kSecondsPerHour;
constexpr long long kSecondsPerWeek = 5 * kSecondsPerDay;
// Caps a hostile/garbled field value (e.g. "99999999999999w") from overflowing `long long`
// (UB) in the multiply/add below — this is a sort comparator, so an out-of-range value just
// needs to saturate to "very large", not be exact. CPP_CODE_AUDIT.md #19.
constexpr long long kMaxDurationSeconds = (std::numeric_limits<long long>::max)() / 2;

void SaturatingAccumulateDuration(long long& total, long long num, long long perUnit) {
    if (num > 0 && perUnit > 0 && num > kMaxDurationSeconds / perUnit) {
        total = kMaxDurationSeconds;
        return;
    }
    const long long added = num * perUnit;
    if (total > kMaxDurationSeconds - added) {
        total = kMaxDurationSeconds;
    } else {
        total += added;
    }
}

std::string TrimSpacesTabs(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        --b;
    }
    return s.substr(a, b - a);
}

// Whole-string integer fast path ("3600" == plain seconds). Returns true + sets outValue when
// the entire (trimmed) string parses as one integer; false to fall through to the manual
// unit-by-unit parse below (e.g. "3h 30m").
bool TryParseWholeDurationSeconds(const std::string& s, long long& outValue) {
    size_t pos = 0;
    try {
        long long v = std::stoll(s, &pos, 10);
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
            ++pos;
        }
        if (pos >= s.size()) {
            outValue = v;
            return true;
        }
    } catch (...) {
        // catch-all-ok: std::stoll throws on non-numeric input — intentional fall-through to the
        // manual unit-by-unit duration parse below.
    }
    return false;
}

// Manual unit-by-unit duration parse ("3h 30m", "2.5h", ...) for values the whole-string fast
// path rejected.
long long ParseDurationUnitsSum(const std::string& s) {
    size_t pos = 0;
    long long total = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
            ++pos;
        }
        if (pos >= s.size()) {
            break;
        }
        long long num = 1;
        if (std::isdigit(static_cast<unsigned char>(s[pos]))) {
            size_t next = 0;
            try {
                num = std::stoll(s.substr(pos), &next, 10);
                pos += next;
            } catch (...) {
                break;
            }
        }
        if (pos >= s.size()) {
            SaturatingAccumulateDuration(total, num, 1);
            break;
        }
        const char u = s[pos];
        if (u == 'w' || u == 'W') {
            SaturatingAccumulateDuration(total, num, kSecondsPerWeek);
            ++pos;
        } else if (u == 'd' || u == 'D') {
            SaturatingAccumulateDuration(total, num, kSecondsPerDay);
            ++pos;
        } else if (u == 'h' || u == 'H') {
            SaturatingAccumulateDuration(total, num, kSecondsPerHour);
            ++pos;
        } else if (u == 'm' || u == 'M') {
            SaturatingAccumulateDuration(total, num, 60LL);
            ++pos;
        } else {
            // Non-unit char (e.g. the '.' in "2.5h") — not a recognized unit suffix. Advance
            // past it so the loop always makes progress; without this `pos` never moves and
            // `num` gets re-added forever (infinite loop / permanent UI freeze on sort —
            // CPP_CODE_AUDIT.md #3).
            SaturatingAccumulateDuration(total, num, 1);
            ++pos;
        }
    }
    return total;
}

} // namespace

namespace TicketGridDurationSortPure {

long long MaxDurationSeconds() {
    return kMaxDurationSeconds;
}

long long ParseDurationToSecondsForSort(const std::string& input) {
    const std::string s = TrimSpacesTabs(input);
    if (s.empty()) {
        return 0;
    }
    long long wholeValue = 0;
    if (TryParseWholeDurationSeconds(s, wholeValue)) {
        return wholeValue;
    }
    return ParseDurationUnitsSum(s);
}

int CompareTimeTrackingValues(const std::string& aVal, const std::string& bVal) {
    const long long sa = ParseDurationToSecondsForSort(aVal);
    const long long sb = ParseDurationToSecondsForSort(bVal);
    if (sa != sb) {
        return (sa < sb) ? -1 : 1;
    }
    return 0;
}

} // namespace TicketGridDurationSortPure
