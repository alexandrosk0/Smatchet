#pragma once

#include <string>

#include <nlohmann/json.hpp>

inline bool TryParseJsonMaybeDoubleEncoded(const std::string& raw, nlohmann::json& outJson) {
    outJson = nlohmann::json::parse(raw, nullptr, false);
    if (outJson.is_discarded()) {
        outJson = nlohmann::json();
        return false;
    }
    if (outJson.is_string()) {
        nlohmann::json nested = nlohmann::json::parse(outJson.get<std::string>(), nullptr, false);
        if (nested.is_discarded()) {
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
        } catch (...) {
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
        } catch (...) {
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
