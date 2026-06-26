#pragma once

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

inline int ParseJsonIntLoose(const nlohmann::json& v, int fallback = 0) {
    if (v.is_number_integer()) {
        return static_cast<int>(v.get<long long>());
    }
    if (v.is_number_unsigned()) {
        return static_cast<int>(v.get<unsigned long long>());
    }
    if (v.is_number_float()) {
        return static_cast<int>(v.get<double>());
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
    if (v.is_number_integer()) {
        return v.get<long long>();
    }
    if (v.is_number_unsigned()) {
        return static_cast<long long>(v.get<unsigned long long>());
    }
    if (v.is_number_float()) {
        return static_cast<long long>(v.get<double>());
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
