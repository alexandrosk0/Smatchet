// LuaJsonConvert.h
// Shared JSON <-> Lua marshalling leaf, lifted out of the two Lua binding TUs
// (AppController_LuaBindings.cpp + AppController_LuaBindingsCore.cpp) per debt
// line 129 / DRY Pillar 5 — the converter was a byte-identical copy in both.
// Header-only inline: the converter is reached ONLY through the Lua-gated
// binding TUs and AppController_LuaBindings_detail.h, so the whole surface sits
// behind SMATCHET_WITH_LUA_AUTOMATION exactly like ILuaBindingHost.h. In a
// Lua-OFF build the header stays includable and empty (sol2 headers absent),
// which keeps the dual-target / Lua-disabled build green (no #863-class skew).

#pragma once

#if defined(SMATCHET_WITH_LUA_AUTOMATION)

// Order matches ILuaBindingHost.h / AppController.h: limits + cstdint BEFORE
// sol/sol.hpp so GCC 13+ std::numeric_limits picks the right specialisation
// under -mcmodel=large.
#include <limits>
#include <cstdint>

#include <sol/sol.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace smatchet {
namespace lua_json_detail {

constexpr int kJsonToLuaMaxDepth = 64;

// Node budget for the converter itself (defense-in-depth). decode_json already
// bounds depth + nodes at parse time; this guards the OTHER caller — commands.invoke
// results (cr.Data), which the bindings produce internally — from fanning out an
// unbounded number of sol tables. `nodes` is decremented past zero to signal the
// cap was hit; on overflow we stop converting (return nil for the remaining
// subtree) rather than throw (Pillar 3 graceful degradation).
constexpr std::size_t kJsonToLuaMaxNodes = 200000u;

inline sol::object JsonToLuaImpl(sol::state_view luaView, const nlohmann::json& j, int depth, std::size_t& nodes) {
    if (depth > kJsonToLuaMaxDepth || nodes == 0) {
        return sol::make_object(luaView, sol::nil);
    }
    --nodes;
    switch (j.type()) {
    case nlohmann::json::value_t::null:
        return sol::make_object(luaView, sol::nil);
    case nlohmann::json::value_t::boolean:
        return sol::make_object(luaView, j.get<bool>());
    case nlohmann::json::value_t::number_integer:
        return sol::make_object(luaView, static_cast<double>(j.get<std::int64_t>()));
    case nlohmann::json::value_t::number_unsigned:
        return sol::make_object(luaView, static_cast<double>(j.get<std::uint64_t>()));
    case nlohmann::json::value_t::number_float:
        return sol::make_object(luaView, j.get<double>());
    case nlohmann::json::value_t::string:
        return sol::make_object(luaView, j.get<std::string>());
    case nlohmann::json::value_t::array: {
        sol::table arr = luaView.create_table();
        std::size_t idx = 1;
        for (const auto& el : j) {
            arr[idx++] = JsonToLuaImpl(luaView, el, depth + 1, nodes);
        }
        return arr;
    }
    case nlohmann::json::value_t::object: {
        sol::table tbl = luaView.create_table();
        for (auto it = j.begin(); it != j.end(); ++it) {
            tbl[it.key()] = JsonToLuaImpl(luaView, it.value(), depth + 1, nodes);
        }
        return tbl;
    }
    default:
        return sol::make_object(luaView, sol::nil);
    }
}

// `nodes` is a shared element budget (defense-in-depth, mirrors JsonToLuaImpl):
// a Lua table with a single sparse integer key (e.g. {[2000000000]=1}) makes
// max_idx astronomically large, so densifying [1..max_idx] would build a
// billions-element json array and exhaust the heap. The budget caps total
// materialised elements; on exhaustion we stop densifying (Pillar 3 graceful
// degradation) instead of OOM-crashing.
inline nlohmann::json LuaToJsonImpl(sol::object obj, int depth, std::size_t& nodes) {
    if (depth > 64 || nodes == 0)
        return nullptr;
    --nodes;
    if (obj.get_type() == sol::type::lua_nil)
        return nullptr;
    if (obj.is<bool>())
        return obj.as<bool>();
    if (obj.is<double>())
        return obj.as<double>();
    if (obj.is<std::string>())
        return obj.as<std::string>();
    if (obj.is<sol::table>()) {
        sol::table t = obj.as<sol::table>();
        bool is_array = true;
        size_t max_idx = 0;
        t.for_each([&](sol::object k, sol::object /*value*/) {
            if (k.is<size_t>()) {
                max_idx = (std::max)(max_idx, k.as<size_t>());
            } else {
                is_array = false;
            }
        });
        if (is_array && max_idx > 0) {
            nlohmann::json j = nlohmann::json::array();
            for (size_t i = 1; i <= max_idx; ++i) {
                if (nodes == 0)
                    break; // budget exhausted — stop densifying (caps a sparse-key blow-up)
                j.push_back(LuaToJsonImpl(t[i], depth + 1, nodes));
            }
            return j;
        } else {
            nlohmann::json j = nlohmann::json::object();
            t.for_each([&](sol::object k, sol::object v) {
                if (nodes == 0)
                    return; // budget exhausted — stop materializing keys (matches the array branch)
                if (k.is<std::string>()) {
                    j[k.as<std::string>()] = LuaToJsonImpl(v, depth + 1, nodes);
                }
            });
            return j;
        }
    }
    return nullptr;
}

} // namespace lua_json_detail
} // namespace smatchet

// Public converters — GLOBAL scope + inline so the binding TUs' unqualified
// JsonToLua / LuaToJson call sites resolve here with zero churn (they used to
// reach file-scope / namespace-static copies). Both delegate into the detail
// namespace above.
inline sol::object JsonToLua(sol::state_view luaView, const nlohmann::json& j) {
    std::size_t nodes = smatchet::lua_json_detail::kJsonToLuaMaxNodes;
    return smatchet::lua_json_detail::JsonToLuaImpl(luaView, j, 0, nodes);
}

inline nlohmann::json LuaToJson(sol::object obj) {
    std::size_t nodes = smatchet::lua_json_detail::kJsonToLuaMaxNodes;
    return smatchet::lua_json_detail::LuaToJsonImpl(obj, 0, nodes);
}

#endif // SMATCHET_WITH_LUA_AUTOMATION
