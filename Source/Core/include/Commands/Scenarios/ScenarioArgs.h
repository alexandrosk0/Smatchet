#ifndef SMATCHET_COMMANDS_SCENARIOS_SCENARIO_ARGS_H
#define SMATCHET_COMMANDS_SCENARIOS_SCENARIO_ARGS_H

// Shared scenario-arg coercion helpers, extracted from the per-scenario
// anon-namespace copies (debt 2026-06-28 shared-scenario-onstart-prologue-helper).
// CLI args land as JSON strings (Source/Standalone/CliCommandRunner.cpp § ParseArgs
// stores --key=value as `string`), so scenarios must coerce defensively or risk
// nlohmann::json::type_error.302 when args.value<int>(...) hits a string.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace smatchet {
namespace cmd {

inline int IntArg(const nlohmann::json& args, const char* key, int fallback) {
    if (!args.contains(key))
        return fallback;
    const nlohmann::json& v = args[key];
    if (v.is_number())
        return v.get<int>();
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

inline std::string StringArg(const nlohmann::json& args, const char* key, const std::string& fallback) {
    if (!args.contains(key))
        return fallback;
    const nlohmann::json& v = args[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number())
        return std::to_string(v.get<long long>());
    return fallback;
}

// Booleans show up as JSON bools when the runner forwards a Lua/MCP-side
// boolean directly, but the CLI flow stringifies every `--flag=value` so the
// textual forms "true" / "1" / "yes" (case-insensitive) are also accepted.
// Anything else falls back to the supplied default, matching IntArg.
inline bool BoolArg(const nlohmann::json& args, const char* key, bool fallback) {
    if (!args.contains(key))
        return fallback;
    const nlohmann::json& v = args[key];
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number())
        return v.get<int>() != 0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        if (s == "true" || s == "1" || s == "yes")
            return true;
        if (s == "false" || s == "0" || s == "no")
            return false;
    }
    return fallback;
}

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_SCENARIO_ARGS_H
