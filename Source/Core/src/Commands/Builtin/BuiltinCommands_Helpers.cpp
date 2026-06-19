// Shared helpers for the per-category Builtin TUs. Declarations in
// BuiltinCommands_Internal.h. No registration logic here.

#include "BuiltinCommands_Internal.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace smatchet {
namespace cmd {
namespace builtin_detail {

nlohmann::json PaginateString(const std::vector<std::string>& items, int limit, int offset) {
    if (limit <= 0)
        limit = 50;
    if (limit > 500)
        limit = 500;
    if (offset < 0)
        offset = 0;
    const int total = static_cast<int>(items.size());
    nlohmann::json arr = nlohmann::json::array();
    for (int i = offset; i < total && static_cast<int>(arr.size()) < limit; ++i) {
        arr.push_back(items[i]);
    }
    nlohmann::json out;
    out["items"] = std::move(arr);
    out["total"] = total;
    out["limit"] = limit;
    out["offset"] = offset;
    out["hasMore"] = (offset + static_cast<int>(out["items"].size())) < total;
    return out;
}

nlohmann::json PaginateJsonArray(const nlohmann::json& items, int limit, int offset) {
    if (limit <= 0)
        limit = 50;
    if (limit > 500)
        limit = 500;
    if (offset < 0)
        offset = 0;
    const int total = static_cast<int>(items.size());
    nlohmann::json arr = nlohmann::json::array();
    for (int i = offset; i < total && static_cast<int>(arr.size()) < limit; ++i) {
        arr.push_back(items[i]);
    }
    nlohmann::json out;
    out["items"] = std::move(arr);
    out["total"] = total;
    out["limit"] = limit;
    out["offset"] = offset;
    out["hasMore"] = (offset + static_cast<int>(out["items"].size())) < total;
    return out;
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) -> char { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; });
    return s;
}

std::string CategoryFromName(const std::string& name) {
    const auto dot = name.find('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

// Light-touch redaction for `config.path` env dump — token-bearing names lose
// their value. Add to this list as new secrets are introduced.
bool IsSensitiveEnvName(const std::string& name) {
    const std::string n = ToLowerAscii(name);
    return n.find("token") != std::string::npos || n.find("secret") != std::string::npos ||
           n.find("password") != std::string::npos || n.find("api_key") != std::string::npos;
}

// Iterate known SMATCHET_* env vars (stable contract from the plan's
// "Environment contract" table). Returns observed names + redacted values.
nlohmann::json ObservedSmatchetEnv() {
    static const char* const kEnvNames[] = {
        "SMATCHET_MCP_PORT",
        "SMATCHET_MCP_HOST",
        "SMATCHET_MCP_ALLOW_REMOTE",
        "SMATCHET_USER_DATA",
        "SMATCHET_LOG_LEVEL",
        "SMATCHET_NO_COLOR",
        "NO_COLOR",
        "SMATCHET_DEFAULT_FORMAT",
        "SMATCHET_TRACKER_TOKEN",    // → cfg.ApiToken (Jira) or cfg.PlaneApiKey (Plane)
        "SMATCHET_TRACKER_BASE_URL", // → cfg.Domain (Jira) or cfg.PlaneUrl (Plane)
        "SMATCHET_BACKEND_TYPE",
        "SMATCHET_TRACKER_TYPE",
        "SMATCHET_DB_PATH",
        "SMATCHET_SPAWN_TIMEOUT_MS",
    };
    nlohmann::json out = nlohmann::json::array();
    for (const char* n : kEnvNames) {
        const char* v = std::getenv(n);
        if (!v || !*v)
            continue;
        nlohmann::json one;
        one["name"] = n;
        one["value"] = IsSensitiveEnvName(n) ? std::string("***") : std::string(v);
        out.push_back(std::move(one));
    }
    return out;
}

Command MakeCommand(std::string name, std::string summary,
                    std::function<CommandResult(const nlohmann::json&, const CommandContext&)> handler) {
    Command c;
    c.Name = std::move(name);
    c.Category = CategoryFromName(c.Name);
    c.Summary = std::move(summary);
    c.Handler = std::move(handler);
    return c;
}

ParamSpec PInt(std::string name, std::string desc, long long defaultVal) {
    ParamSpec p;
    p.Name = std::move(name);
    p.Description = std::move(desc);
    p.Type = ParamType::Int;
    p.Default = std::make_shared<nlohmann::json>(defaultVal);
    return p;
}

ParamSpec PString(std::string name, std::string desc, bool required) {
    ParamSpec p;
    p.Name = std::move(name);
    p.Description = std::move(desc);
    p.Type = ParamType::String;
    p.Required = required;
    return p;
}

} // namespace builtin_detail
} // namespace cmd
} // namespace smatchet
