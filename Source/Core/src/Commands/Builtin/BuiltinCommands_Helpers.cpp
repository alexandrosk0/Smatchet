// Shared helpers for the per-category Builtin TUs. Declarations in
// BuiltinCommands_Internal.h. No registration logic here.

#include "BuiltinCommands_Internal.h"

#include "StringUtil.h"

#include <algorithm>
#include <cstdio>
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

// Forwards to the shared Core helper (gate-blind-spot-sweep Slice 2). The NAME stays: it is
// declared in BuiltinCommands_Internal.h and pulled in by BuiltinCommands_{Debug,Tickets}.cpp
// via `using builtin_detail::ToLowerAscii;`.
//
// Behaviour note — this body was the odd one out: an explicit `'A'..'Z'` branch rather than
// std::tolower. The two agree exactly under the "C" locale, which is the only locale this
// process ever has (nothing in the tree calls std::setlocale), so the substitution is
// behaviour-identical for every input. Should that ever change, this forwarder is the single
// place to reinstate the locale-independent branch.
std::string ToLowerAscii(std::string s) { return ToLowerAsciiCopy(std::move(s)); }

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

// See BuiltinCommands_Internal.h for why every non-zero component is emitted (#2054).
std::string FormatWorklogTimeSpent(int seconds) {
    if (seconds <= 0) {
        return std::string("0s");
    }
    const int h = seconds / 3600;
    const int m = (seconds % 3600) / 60;
    const int s = seconds % 60;
    std::string out;
    char part[24] = {};
    if (h > 0) {
        std::snprintf(part, sizeof(part), "%dh", h);
        out += part;
    }
    if (m > 0) {
        if (!out.empty())
            out += ' ';
        std::snprintf(part, sizeof(part), "%dm", m);
        out += part;
    }
    if (s > 0) {
        if (!out.empty())
            out += ' ';
        std::snprintf(part, sizeof(part), "%ds", s);
        out += part;
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
