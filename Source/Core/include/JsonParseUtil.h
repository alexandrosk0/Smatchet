#pragma once

#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "Json/BoundedJsonParse.h"

// Both parses route through ParseBounded: `raw` (and the double-encoded inner
// string) is tracker-sourced field data, so a deeply-nested payload would
// stack-overflow the recursive ~json DOM teardown under a bare parse — the 3-arg
// non-throwing form still builds the full DOM. ParseBounded caps depth/nodes/bytes
// and signals failure via a non-empty errOut (it never throws / discards).
inline bool TryParseJsonMaybeDoubleEncoded(const std::string& raw, nlohmann::json& outJson) {
    std::string err;
    outJson = smatchet::json_safe::ParseBounded(raw, err);
    if (!err.empty()) {
        outJson = nlohmann::json();
        return false;
    }
    if (outJson.is_string()) {
        std::string nestedErr;
        nlohmann::json nested = smatchet::json_safe::ParseBounded(outJson.get<std::string>(), nestedErr);
        if (!nestedErr.empty()) {
            outJson = nlohmann::json();
            return false;
        }
        outJson = std::move(nested);
    }
    return true;
}

namespace json_parse_util_detail {

// True when truncating `d` toward zero lands inside int's range. Casting any
// other double to int (out of range, or NaN) is undefined behaviour.
inline bool TruncatesIntoInt(double d) {
    return d > static_cast<double>((std::numeric_limits<int>::min)()) - 1.0 &&
           d < static_cast<double>((std::numeric_limits<int>::max)()) + 1.0;
}

// The same for long long: [-2^63, 2^63), both bounds exact doubles.
inline bool TruncatesIntoInt64(double d) {
    return d >= static_cast<double>((std::numeric_limits<long long>::min)()) &&
           d < -static_cast<double>((std::numeric_limits<long long>::min)());
}

} // namespace json_parse_util_detail

// A number outside the target range takes `fallback`, whatever its JSON encoding,
// the same as an out-of-range numeric string, for which std::stoi and std::stoll
// throw out_of_range. Wrapping it would name a different record, and casting an
// out-of-range float is undefined behaviour. Unsigned is tested before integer
// because nlohmann's is_number_integer() is also true for an unsigned value.
inline int ParseJsonIntLoose(const nlohmann::json& v, int fallback = 0) {
    if (v.is_number_unsigned()) {
        const unsigned long long u = v.get<unsigned long long>();
        return u <= static_cast<unsigned long long>((std::numeric_limits<int>::max)()) ? static_cast<int>(u) : fallback;
    }
    if (v.is_number_integer()) {
        const long long n = v.get<long long>();
        return n >= (std::numeric_limits<int>::min)() && n <= (std::numeric_limits<int>::max)() ? static_cast<int>(n)
                                                                                                : fallback;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return json_parse_util_detail::TruncatesIntoInt(d) ? static_cast<int>(d) : fallback;
    }
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) { // catch-all-ok: stoi/stoll on untrusted JSON string
            return fallback;
        }
    }
    return fallback;
}

inline long long ParseJsonInt64Loose(const nlohmann::json& v, long long fallback = 0) {
    if (v.is_number_unsigned()) {
        const unsigned long long u = v.get<unsigned long long>();
        return u <= static_cast<unsigned long long>((std::numeric_limits<long long>::max)()) ? static_cast<long long>(u)
                                                                                             : fallback;
    }
    if (v.is_number_integer()) {
        return v.get<long long>();
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        return json_parse_util_detail::TruncatesIntoInt64(d) ? static_cast<long long>(d) : fallback;
    }
    if (v.is_string()) {
        try {
            return std::stoll(v.get<std::string>());
        } catch (...) { // catch-all-ok: stoi/stoll on untrusted JSON string
            return fallback;
        }
    }
    return fallback;
}

inline int ParseJsonIntFieldLoose(const nlohmann::json& j, const char* key, int fallback = 0) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return fallback;
    }
    return ParseJsonIntLoose(*it, fallback);
}

inline long long ParseJsonInt64FieldLoose(const nlohmann::json& j, const char* key, long long fallback = 0) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return fallback;
    }
    return ParseJsonInt64Loose(*it, fallback);
}
