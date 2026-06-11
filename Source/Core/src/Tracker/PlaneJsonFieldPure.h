#pragma once
// Shared tolerant json-field -> string coercion for the Plane *Pure TUs. Kept
// cpr-free so the doctest rig links them standalone; mirrors the production
// smatchet::plane_detail::JsonFieldToString (PlaneClient.cpp), whose declaration
// lives in PlaneClient_Internal.h and therefore pulls cpr.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace smatchet {
namespace plane_pure {

inline std::string JsonFieldToString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null())
        return std::string();
    const auto& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<std::int64_t>());
    if (v.is_number_float())
        return std::to_string(v.get<double>());
    if (v.is_boolean())
        return v.get<bool>() ? std::string("true") : std::string("false");
    return v.dump();
}

} // namespace plane_pure
} // namespace smatchet
