#pragma once

// RemoteVersionPure — the tracker's "last updated" stamp as a comparable version.
//
// A ticket's `updated` field is the version the delta sync diffs against and the stamp a
// late-arriving change probe is judged by: a probe row that is OLDER than the row already held
// must never replace it (docs/plans/active/delta-sync.md § Phase 1). Header-only, SQLite-free and
// platform-free (C++14, no OS headers) so it compiles in the standalone and DX12 targets and in
// the doctest rig.
//
// Accepted shapes (ISO 8601): "2024-05-01T10:00:00.000+0000" (Jira), "2024-05-01T10:00:00.123456Z"
// (Plane), "2024-05-01T10:00:00+01:00", "2024-05-01T10:00:00Z". A fraction is truncated to
// milliseconds. Anything else does not parse; callers then treat the order as Unknown and keep
// today's behaviour (accept the incoming row).

#include <cstdint>
#include <string>

namespace smatchet {
namespace sync {

namespace remote_version_detail {

/// Days from 1970-01-01 to the civil date (proleptic Gregorian), Howard Hinnant's algorithm.
inline std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9u)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// Reads exactly `digits` ASCII digits at `pos` into `out`; false when any is not a digit.
inline bool ReadDigits(const std::string& s, std::size_t& pos, std::size_t digits, int& out) {
    if (pos + digits > s.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    pos += digits;
    out = value;
    return true;
}

inline bool Expect(const std::string& s, std::size_t& pos, char c) {
    if (pos < s.size() && s[pos] == c) {
        ++pos;
        return true;
    }
    return false;
}

/// Optional ".fff" fraction -> milliseconds (extra digits truncated). Leaves `pos` after it.
inline void ReadFractionMs(const std::string& s, std::size_t& pos, int& outMs) {
    outMs = 0;
    if (pos >= s.size() || s[pos] != '.') {
        return;
    }
    ++pos;
    int digitsRead = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        if (digitsRead < 3) {
            outMs = outMs * 10 + (s[pos] - '0');
            ++digitsRead;
        }
        ++pos;
    }
    for (; digitsRead < 3; ++digitsRead) {
        outMs *= 10;
    }
}

/// Zone suffix -> offset in minutes east of UTC. Accepts Z, +hhmm, +hh:mm, +hh.
inline bool ReadZoneOffsetMinutes(const std::string& s, std::size_t& pos, int& outMinutes) {
    outMinutes = 0;
    if (pos >= s.size()) {
        return false; // a stamp without a zone is ambiguous: do not compare it
    }
    if (s[pos] == 'Z' || s[pos] == 'z') {
        ++pos;
        return pos == s.size();
    }
    const char sign = s[pos];
    if (sign != '+' && sign != '-') {
        return false;
    }
    ++pos;
    int hh = 0;
    int mm = 0;
    if (!ReadDigits(s, pos, 2, hh)) {
        return false;
    }
    if (pos < s.size()) {
        (void)Expect(s, pos, ':');
        if (!ReadDigits(s, pos, 2, mm)) {
            return false;
        }
    }
    if (pos != s.size() || hh > 23 || mm > 59) {
        return false;
    }
    outMinutes = (hh * 60 + mm) * (sign == '-' ? -1 : 1);
    return true;
}

} // namespace remote_version_detail

/// Parses an ISO 8601 tracker timestamp to milliseconds since the Unix epoch (UTC). False (and
/// `outMs` untouched) when `text` is not one of the accepted shapes or a field is out of range.
inline bool ParseRemoteUpdatedMs(const std::string& text, std::int64_t& outMs) {
    std::size_t pos = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!remote_version_detail::ReadDigits(text, pos, 4, year) || !remote_version_detail::Expect(text, pos, '-') ||
        !remote_version_detail::ReadDigits(text, pos, 2, month) || !remote_version_detail::Expect(text, pos, '-') ||
        !remote_version_detail::ReadDigits(text, pos, 2, day)) {
        return false;
    }
    if (!(remote_version_detail::Expect(text, pos, 'T') || remote_version_detail::Expect(text, pos, ' ')) ||
        !remote_version_detail::ReadDigits(text, pos, 2, hour) || !remote_version_detail::Expect(text, pos, ':') ||
        !remote_version_detail::ReadDigits(text, pos, 2, minute) || !remote_version_detail::Expect(text, pos, ':') ||
        !remote_version_detail::ReadDigits(text, pos, 2, second)) {
        return false;
    }
    int fractionMs = 0;
    remote_version_detail::ReadFractionMs(text, pos, fractionMs);
    int offsetMinutes = 0;
    if (!remote_version_detail::ReadZoneOffsetMinutes(text, pos, offsetMinutes)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    const std::int64_t days =
        remote_version_detail::DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const std::int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second - offsetMinutes * 60;
    outMs = seconds * 1000 + fractionMs;
    return true;
}

/// How an incoming row's stamp relates to the stamp of the row already held.
enum class RemoteVersionOrder {
    Unknown, ///< either stamp is empty or unparsable - no ordering can be claimed
    Older,   ///< incoming is strictly older than resident
    Same,
    Newer,
};

/// Orders `incoming` against `resident` by parsed instant (so "+0000" and "Z" spellings of one
/// instant compare Same).
inline RemoteVersionOrder CompareRemoteUpdated(const std::string& incoming, const std::string& resident) {
    std::int64_t incomingMs = 0;
    std::int64_t residentMs = 0;
    if (!ParseRemoteUpdatedMs(incoming, incomingMs) || !ParseRemoteUpdatedMs(resident, residentMs)) {
        return RemoteVersionOrder::Unknown;
    }
    if (incomingMs < residentMs) {
        return RemoteVersionOrder::Older;
    }
    return incomingMs == residentMs ? RemoteVersionOrder::Same : RemoteVersionOrder::Newer;
}

/// True when an incoming row must NOT replace the resident one because it is provably older.
/// Unknown order never blocks (the pre-delta behaviour: the incoming row wins).
inline bool IsIncomingRowStale(const std::string& incomingUpdated, const std::string& residentUpdated) {
    return CompareRemoteUpdated(incomingUpdated, residentUpdated) == RemoteVersionOrder::Older;
}

/// A stored stamp later than `nowMs` by more than this is not a real tracker stamp (tracker clock corrected,
/// restore, import): it is treated as unknown and never blocks a write.
constexpr std::int64_t kImplausibleFutureMs = 24LL * 60 * 60 * 1000;

/// True when `residentUpdated` parses to an instant more than a day past `nowMs`.
inline bool IsStampImplausiblyInFuture(const std::string& residentUpdated, std::int64_t nowMs) {
    std::int64_t residentMs = 0;
    return ParseRemoteUpdatedMs(residentUpdated, residentMs) && residentMs > nowMs + kImplausibleFutureMs;
}

/// IsIncomingRowStale, except a resident stamp far in the future (IsStampImplausiblyInFuture) is unknown
/// and so never blocks: a wedged future stamp cannot make every later refetch of the row look stale.
inline bool IsIncomingRowStale(const std::string& incomingUpdated, const std::string& residentUpdated,
                               std::int64_t nowMs) {
    return !IsStampImplausiblyInFuture(residentUpdated, nowMs) && IsIncomingRowStale(incomingUpdated, residentUpdated);
}

} // namespace sync
} // namespace smatchet
