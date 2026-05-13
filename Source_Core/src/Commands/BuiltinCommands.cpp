// Built-in commands registered by AppController::Initialize.
// See backlog/COMMAND_SYSTEM_PLAN.md for the full catalogue spec.
//
// All handlers are AsyncSafe=true unless explicitly marked otherwise. Every
// handler must be reentrant — handlers may call back into the registry via
// `Lua commands.invoke` or `commands.invoke` MCP tool.

#include "Commands/BuiltinCommands.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/ViewToggleCommands.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetUI.h"
#include "SmatchetUiSession.h"
#include "TrackerFieldSchema.h"
#include "UiPerfMonitor.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#else
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

// Pulled from SmatchetUI.cpp via extern — same singleton pattern used by ViewToggleCommands.cpp.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

// --- Helpers ---------------------------------------------------------------

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
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
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

// --- Command builders ------------------------------------------------------

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
    p.Default = defaultVal;
    return p;
}

ParamSpec PString(std::string name, std::string desc, bool required = false) {
    ParamSpec p;
    p.Name = std::move(name);
    p.Description = std::move(desc);
    p.Type = ParamType::String;
    p.Required = required;
    return p;
}

} // namespace

void RegisterBuiltinCommands(CommandRegistry& reg, AppController& app) {
    // === meta — discovery / introspection =================================

    {
        Command c = MakeCommand("commands.list", "List registered commands, optionally filtered by category.",
                                [&reg](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                                    const std::string category = args.value("category", std::string());
                                    // Default 500 (not 50) so agents don't silently miss commands when the
                                    // catalog grows past the previous default. Use --limit=N to narrow.
                                    const int limit = args.value("limit", 500);
                                    const int offset = args.value("offset", 0);
                                    const bool full = args.value("full", false);
                                    std::vector<Command> all = category.empty() ? reg.All() : reg.ByCategory(category);
                                    nlohmann::json items = nlohmann::json::array();
                                    for (const Command& cm : all) {
                                        nlohmann::json one;
                                        one["name"] = cm.Name;
                                        one["category"] = cm.Category;
                                        one["summary"] = cm.Summary;
                                        one["destructive"] = cm.Destructive;
                                        one["idempotent"] = cm.Idempotent;
                                        one["dryRunSupported"] = cm.DryRunSupported;
                                        one["asyncCompletes"] = !cm.AsyncSafe;
                                        if (full) {
                                            // Compact per-param view (lighter than full inputSchema).
                                            // Agents that need full JSON Schema should call commands.help.
                                            nlohmann::json params = nlohmann::json::array();
                                            for (const ParamSpec& p : cm.Params) {
                                                nlohmann::json pj;
                                                pj["name"] = p.Name;
                                                const char* tname = "string";
                                                switch (p.Type) {
                                                case ParamType::String:
                                                    tname = "string";
                                                    break;
                                                case ParamType::Int:
                                                    tname = "int";
                                                    break;
                                                case ParamType::Bool:
                                                    tname = "bool";
                                                    break;
                                                case ParamType::Number:
                                                    tname = "number";
                                                    break;
                                                case ParamType::Json:
                                                    tname = "json";
                                                    break;
                                                }
                                                pj["type"] = tname;
                                                pj["required"] = p.Required;
                                                if (!p.Description.empty())
                                                    pj["description"] = p.Description;
                                                if (!p.Default.is_null())
                                                    pj["default"] = p.Default;
                                                if (!p.Enum.empty())
                                                    pj["enum"] = p.Enum;
                                                params.push_back(std::move(pj));
                                            }
                                            one["params"] = std::move(params);
                                            if (!cm.Aliases.empty())
                                                one["aliases"] = cm.Aliases;
                                        }
                                        items.push_back(std::move(one));
                                    }
                                    return CommandResult::Success(PaginateJsonArray(items, limit, offset));
                                });
        c.Description =
            "Discovery entry point for agents. Pass --full to include params per command in one call "
            "(avoids N round-trips to commands.help). For complete JSON Schema use commands.help --name=<n>.";
        c.Params = {
            PString("category", "Restrict to one category (e.g. 'tickets')."),
            PInt("limit", "Max items (default 500, max 500).", 500),
            PInt("offset", "Pagination offset.", 0),
            {[] {
                ParamSpec p;
                p.Name = "full";
                p.Type = ParamType::Bool;
                p.Default = false;
                p.Description = "Include compact per-param schema per command.";
                return p;
            }()},
        };
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("commands.help", "Return the full schema (params, flags, examples) for one command.",
                        [&reg](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                            const std::string name = args.value("name", std::string());
                            std::vector<Command> all = reg.All();
                            for (const Command& cm : all) {
                                if (cm.Name == name) {
                                    nlohmann::json out;
                                    out["name"] = cm.Name;
                                    out["category"] = cm.Category;
                                    out["summary"] = cm.Summary;
                                    out["description"] = cm.Description;
                                    out["destructive"] = cm.Destructive;
                                    out["idempotent"] = cm.Idempotent;
                                    out["dryRunSupported"] = cm.DryRunSupported;
                                    out["asyncCompletes"] = !cm.AsyncSafe;
                                    out["aliases"] = cm.Aliases;
                                    out["inputSchema"] = cm.BuildJsonSchema();
                                    out["helpText"] = cm.BuildHelpText();
                                    return CommandResult::Success(std::move(out));
                                }
                            }
                            std::vector<std::string> suggestions = reg.FuzzyMatch(name, 3);
                            return CommandResult::Failure(
                                ErrorCode::NotFound, "No command named '" + name + "'.",
                                suggestions.empty() ? std::string() : ("Did you mean '" + suggestions.front() + "'?"),
                                std::move(suggestions));
                        });
        c.Params = {PString("name", "Command name to describe.", /*required*/ true)};
        c.Description = "Returns inputSchema (JSON Schema) + helpText (human readable).";
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("commands.search", "Fuzzy-match command names by query.",
                                [&reg](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                                    const std::string q = args.value("query", std::string());
                                    const int limit = args.value("limit", 10);
                                    std::vector<std::string> matches =
                                        reg.FuzzyMatch(q, static_cast<size_t>(std::max(1, limit)));
                                    return CommandResult::Success(PaginateString(matches, limit, 0));
                                });
        c.Params = {
            PString("query", "Fuzzy search string.", /*required*/ true),
            PInt("limit", "Max matches.", 10),
        };
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("commands.recents", "Most recently dispatched command names.",
                                [&reg](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                                    const int limit = args.value("limit", 16);
                                    std::vector<std::string> r = reg.Recents(static_cast<size_t>(std::max(1, limit)));
                                    return CommandResult::Success(PaginateString(r, limit, 0));
                                });
        c.Params = {PInt("limit", "Max items.", 16)};
        reg.Register(std::move(c));
    }

    // `commands.invoke` is implemented inside the Lua bridge directly (it would
    // be circular here — see AppController_LuaBindings.cpp Phase 2).

    // === app =============================================================

    {
        Command c = MakeCommand("app.version", "Application version + build metadata.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    nlohmann::json out;
                                    out["version"] = app.GetAppVersion();
                                    out["releaseRepo"] = app.GetGitHubReleaseRepo();
                                    return CommandResult::Success(std::move(out));
                                });
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("app.quit", "Request graceful shutdown of the running instance.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    app.RequestAppQuit();
                                    nlohmann::json out;
                                    out["requested"] = true;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Destructive = true; // takes the window down — requires --yes / __confirm
        c.Idempotent = true;
        reg.Register(std::move(c));
    }

    // === config =========================================================

    {
        Command c = MakeCommand("config.path", "Paths + observed SMATCHET_* env vars (tokens redacted).",
                                [](const nlohmann::json&, const CommandContext&) {
                                    nlohmann::json out;
                                    out["userData"] = ConfigManager::GetUserDataDirectory();
                                    out["runtimeAssets"] = ConfigManager::GetRuntimeAssetDirectory();
                                    out["imGuiSettings"] = ConfigManager::GetImGuiSettingsPath();
                                    nlohmann::json envObj;
                                    envObj["observed"] = ObservedSmatchetEnv();
                                    out["env"] = std::move(envObj);
                                    return CommandResult::Success(std::move(out));
                                });
        reg.Register(std::move(c));
    }

    // === perf ===========================================================

    {
        Command c = MakeCommand("perf.snapshot", "Per-scope UI perf rows from the last drawn frame.",
                                [](const nlohmann::json&, const CommandContext&) {
                                    std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
                                    nlohmann::json arr = nlohmann::json::array();
                                    for (const UiPerfRow& r : rows) {
                                        nlohmann::json one;
                                        one["name"] = r.name;
                                        one["lastTotalMs"] = r.lastTotalMs;
                                        one["avgPerCallMs"] = r.avgPerCallMs;
                                        one["maxMs"] = r.maxMs;
                                        one["calls"] = r.calls;
                                        one["lifetimeHits"] = r.lifetimeHits;
                                        one["emaAvgMs"] = r.emaAvgMs;
                                        arr.push_back(std::move(one));
                                    }
                                    nlohmann::json out;
                                    out["rows"] = std::move(arr);
                                    return CommandResult::Success(std::move(out));
                                });
        c.Description =
            "Returns array of UI perf rows ({name, lastTotalMs, avgPerCallMs, maxMs, calls, lifetimeHits, emaAvgMs}).";
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("perf.frame_count", "Total count of profiled scope entries in the last frame.",
                                [](const nlohmann::json&, const CommandContext&) {
                                    std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
                                    nlohmann::json out;
                                    out["scopeCount"] = rows.size();
                                    std::uint64_t totalCalls = 0;
                                    for (const UiPerfRow& r : rows)
                                        totalCalls += r.calls;
                                    out["totalCalls"] = totalCalls;
                                    return CommandResult::Success(std::move(out));
                                });
        reg.Register(std::move(c));
    }

    // === tickets =========================================================

    {
        Command c = MakeCommand("tickets.list_active", "Tickets currently loaded in the active project grid.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const int limit = args.value("limit", 50);
                                    const int offset = args.value("offset", 0);
                                    auto snapshot = app.GetActiveTicketsSnapshot();
                                    nlohmann::json items = nlohmann::json::array();
                                    if (snapshot) {
                                        for (const CachedTicket& t : *snapshot) {
                                            nlohmann::json one;
                                            one["id"] = t.id;
                                            // Light projection — full payload would blow context for large views.
                                            const auto& fv = t.fieldValues;
                                            auto itSummary = fv.find("summary");
                                            if (itSummary != fv.end())
                                                one["summary"] = itSummary->second;
                                            auto itStatus = fv.find("status");
                                            if (itStatus != fv.end())
                                                one["status"] = itStatus->second;
                                            items.push_back(std::move(one));
                                        }
                                    }
                                    return CommandResult::Success(PaginateJsonArray(items, limit, offset));
                                });
        c.Description = "Returns {items:[{id, summary?, status?}], total, limit, offset, hasMore}.";
        c.Params = {
            PInt("limit", "Max items (default 50, max 500).", 50),
            PInt("offset", "Pagination offset.", 0),
        };
        c.Aliases = {"list_active_tickets"}; // back-compat with legacy MCP tool name.
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("tickets.search_active",
                                "Case-insensitive substring search across active-view tickets (id + field values).",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string query = args.value("query", std::string());
                                    const int limit = args.value("limit", 50);
                                    const int offset = args.value("offset", 0);
                                    auto snapshot = app.GetActiveTicketsSnapshot();
                                    const std::string q = ToLowerAscii(query);
                                    nlohmann::json items = nlohmann::json::array();
                                    if (snapshot && !q.empty()) {
                                        for (const CachedTicket& t : *snapshot) {
                                            bool hit = (ToLowerAscii(t.id).find(q) != std::string::npos);
                                            if (!hit) {
                                                for (const auto& kv : t.fieldValues) {
                                                    if (ToLowerAscii(kv.second).find(q) != std::string::npos) {
                                                        hit = true;
                                                        break;
                                                    }
                                                }
                                            }
                                            if (hit) {
                                                nlohmann::json one;
                                                one["id"] = t.id;
                                                auto itS = t.fieldValues.find("summary");
                                                if (itS != t.fieldValues.end())
                                                    one["summary"] = itS->second;
                                                items.push_back(std::move(one));
                                            }
                                        }
                                    }
                                    return CommandResult::Success(PaginateJsonArray(items, limit, offset));
                                });
        c.Params = {
            PString("query", "Case-insensitive substring.", /*required*/ true),
            PInt("limit", "Max items.", 50),
            PInt("offset", "Pagination offset.", 0),
        };
        c.Aliases = {"search_active_tickets"};
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("tickets.get", "Full field map for a single active-view ticket.",
                        [&app](const nlohmann::json& args, const CommandContext&) {
                            const std::string id = args.value("id", std::string());
                            auto snapshot = app.GetActiveTicketsSnapshot();
                            if (snapshot) {
                                for (const CachedTicket& t : *snapshot) {
                                    if (t.id == id) {
                                        nlohmann::json one;
                                        one["id"] = t.id;
                                        nlohmann::json fields = nlohmann::json::object();
                                        for (const auto& kv : t.fieldValues) {
                                            fields[kv.first] = kv.second;
                                        }
                                        one["fields"] = std::move(fields);
                                        return CommandResult::Success(std::move(one));
                                    }
                                }
                            }
                            return CommandResult::Failure(
                                ErrorCode::NotFound, "Ticket '" + id + "' not found in active view.",
                                "Verify the id matches the current view; use tickets.list_active to enumerate.");
                        });
        c.Params = {PString("id", "Ticket id (e.g. 'PROJ-1').", /*required*/ true)};
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("tickets.exists", "Whether a ticket id is present in the active view.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string id = args.value("id", std::string());
                                    auto snapshot = app.GetActiveTicketsSnapshot();
                                    bool exists = false;
                                    if (snapshot) {
                                        for (const CachedTicket& t : *snapshot) {
                                            if (t.id == id) {
                                                exists = true;
                                                break;
                                            }
                                        }
                                    }
                                    nlohmann::json out;
                                    out["exists"] = exists;
                                    out["id"] = id;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {PString("id", "Ticket id.", /*required*/ true)};
        reg.Register(std::move(c));
    }

    // === debug ===========================================================

    {
        Command c = MakeCommand("debug.log", "Emit a one-shot Logger entry (info/warn/error). For agent breadcrumbs.",
                                [](const nlohmann::json& args, const CommandContext&) {
                                    const std::string level = ToLowerAscii(args.value("level", std::string("info")));
                                    const std::string msg = args.value("message", std::string());
                                    if (level == "trace")
                                        LOG_TRACE("[cmd] %s", msg.c_str());
                                    else if (level == "debug")
                                        LOG_DEBUG("[cmd] %s", msg.c_str());
                                    else if (level == "warn")
                                        LOG_WARN("[cmd] %s", msg.c_str());
                                    else if (level == "error")
                                        LOG_ERROR("[cmd] %s", msg.c_str());
                                    else
                                        LOG_INFO("[cmd] %s", msg.c_str());
                                    nlohmann::json out;
                                    out["written"] = true;
                                    return CommandResult::Success(std::move(out));
                                });
        ParamSpec level = PString("level", "trace|debug|info|warn|error");
        level.Default = "info";
        level.Enum = {"trace", "debug", "info", "warn", "error"};
        c.Params = {
            std::move(level),
            PString("message", "Log message text.", /*required*/ true),
        };
        reg.Register(std::move(c));
    }

    // === sync ============================================================

    {
        Command c = MakeCommand("sync.incremental", "Delta sync from tracker (last-fetched timestamp onward).",
                                [&app](const nlohmann::json&, const CommandContext& ctx) {
                                    if (ctx.DryRun) {
                                        nlohmann::json out;
                                        out["wouldDo"] = "incremental sync from last-fetched cursor";
                                        return CommandResult::Success(
                                            {{"wouldDo", "incremental sync from last-fetched cursor"}});
                                    }
                                    app.SyncWithBackend();
                                    nlohmann::json out;
                                    out["triggered"] = true;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Destructive = false;
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("sync.full", "Full sync: wipe local cache and re-fetch all tickets from tracker.",
                        [&app](const nlohmann::json&, const CommandContext& ctx) {
                            if (ctx.DryRun) {
                                return CommandResult::Success({{"wouldDo", "wipe local cache + full re-fetch"}});
                            }
                            std::string err;
                            app.RecreateLocalCacheDatabase(err);
                            app.SyncWithBackend();
                            nlohmann::json out;
                            out["triggered"] = true;
                            if (!err.empty())
                                out["warning"] = err;
                            return CommandResult::Success(std::move(out));
                        });
        c.Destructive = true;
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("sync.refresh_local", "Rebuild in-memory ticket list from the local SQLite cache (no network).",
                        [&app](const nlohmann::json&, const CommandContext&) {
                            app.RefreshLocalData();
                            return CommandResult::Success({{"triggered", true}});
                        });
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("sync.tracker_status",
                                "Last connectivity state and diagnostic from the tracker reachability probe.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    using S = AppController::TrackerConnectivityState;
                                    const S s = app.GetLastTrackerConnectivityState();
                                    const char* stateStr = "unknown";
                                    switch (s) {
                                    case S::Unknown:
                                        stateStr = "unknown";
                                        break;
                                    case S::AuthenticatedReachable:
                                        stateStr = "authenticated-reachable";
                                        break;
                                    case S::ReachableAuthOrConfigError:
                                        stateStr = "reachable-auth-or-config-error";
                                        break;
                                    case S::TransportDown:
                                        stateStr = "transport-down";
                                        break;
                                    case S::ServiceUnavailable:
                                        stateStr = "service-unavailable";
                                        break;
                                    }
                                    nlohmann::json out;
                                    out["state"] = stateStr;
                                    out["syncWarning"] = app.GetLastTicketSyncWarning();
                                    out["fieldCatalogError"] = app.GetFieldCatalogError();
                                    return CommandResult::Success(std::move(out));
                                });
        reg.Register(std::move(c));
    }

    // === ticket (mutations) =============================================

    {
        Command c = MakeCommand(
            "ticket.set_field", "Update a single field on a ticket.",
            [&app](const nlohmann::json& args, const CommandContext& ctx) {
                const std::string id = args.value("id", std::string());
                const std::string field = args.value("field", std::string());
                const std::string value = args.value("value", std::string());
                if (ctx.DryRun) {
                    // Read current value for the diff preview.
                    auto snap = app.GetActiveTicketsSnapshot();
                    std::string from;
                    if (snap) {
                        for (const auto& t : *snap) {
                            if (t.id == id) {
                                auto it = t.fieldValues.find(field);
                                if (it != t.fieldValues.end())
                                    from = it->second;
                                break;
                            }
                        }
                    }
                    return CommandResult::Success(
                        {{"wouldDo", {{"ticket", id}, {"field", field}, {"from", from}, {"to", value}}}});
                }
                const TrackerField* fieldMeta = app.FindFieldById(field);
                if (!fieldMeta) {
                    return CommandResult::Failure(ErrorCode::NotFound, "Field '" + field + "' not found in catalog.",
                                                  "Run fields.refresh_catalog first.");
                }
                std::string err;
                const bool ok = app.SubmitFieldEdit(id, *fieldMeta, {value}, err);
                if (!ok) {
                    return CommandResult::Failure(ErrorCode::BackendError, "Field edit failed: " + err,
                                                  "Check tracker connectivity.");
                }
                return CommandResult::Success({{"ok", true}});
            });
        c.Destructive = true;
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Params = {
            PString("id", "Ticket id (e.g. 'PROJ-1').", true),
            PString("field", "Field id (e.g. 'status', 'priority').", true),
            PString("value", "New field value (raw string).", true),
        };
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("ticket.add_comment", "Post a plain-text comment on a ticket.",
                        [&app](const nlohmann::json& args, const CommandContext&) {
                            const std::string id = args.value("id", std::string());
                            const std::string body = args.value("body", std::string());
                            std::string err;
                            const bool ok = app.AddIssueCommentPlain(id, body, err);
                            if (!ok) {
                                return CommandResult::Failure(ErrorCode::BackendError, "Comment failed: " + err);
                            }
                            return CommandResult::Success({{"ok", true}});
                        });
        c.Destructive = true;
        c.Idempotent = false;
        c.Params = {
            PString("id", "Ticket id.", true),
            PString("body", "Comment body (plain text).", true),
        };
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("ticket.add_worklog", "Log time worked on a ticket.",
                        [&app](const nlohmann::json& args, const CommandContext&) {
                            const int seconds = args.value("seconds", 0);
                            const std::string id = args.value("id", std::string());
                            const std::string comment = args.value("comment", std::string());
                            const std::string started = args.value("started", std::string());
                            // timeSpent format: "1h 30m" — build from seconds.
                            const int h = seconds / 3600;
                            const int m = (seconds % 3600) / 60;
                            const int s = seconds % 60;
                            char timeSpent[64] = {};
                            if (h > 0 && m > 0)
                                std::snprintf(timeSpent, sizeof(timeSpent), "%dh %dm", h, m);
                            else if (h > 0)
                                std::snprintf(timeSpent, sizeof(timeSpent), "%dh", h);
                            else if (m > 0)
                                std::snprintf(timeSpent, sizeof(timeSpent), "%dm", m);
                            else
                                std::snprintf(timeSpent, sizeof(timeSpent), "%ds", s);
                            std::string err;
                            const bool ok = app.SubmitWorklog(id, timeSpent, "", "auto", comment, started, err);
                            if (!ok) {
                                return CommandResult::Failure(ErrorCode::BackendError, "Worklog failed: " + err);
                            }
                            return CommandResult::Success({{"ok", true}, {"timeSpent", std::string(timeSpent)}});
                        });
        c.Destructive = true;
        c.Idempotent = false;
        {
            ParamSpec ps;
            ps.Name = "seconds";
            ps.Type = ParamType::Int;
            ps.Required = true;
            ps.Description = "Time worked in seconds (e.g. 3600 = 1 hour).";
            c.Params.push_back(std::move(ps));
        }
        c.Params.push_back(PString("id", "Ticket id.", true));
        c.Params.push_back(PString("started", "ISO 8601 start timestamp (optional; uses now if empty)."));
        c.Params.push_back(PString("comment", "Worklog description."));
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("ticket.transition", "Transition a ticket to a new status.",
                        [&app](const nlohmann::json& args, const CommandContext& ctx) {
                            const std::string id = args.value("id", std::string());
                            const std::string toStatus = args.value("toStatus", std::string());
                            if (ctx.DryRun) {
                                return CommandResult::Success({{"wouldDo", {{"ticket", id}, {"toStatus", toStatus}}}});
                            }
                            const TrackerField* statusField = app.FindFieldById("status");
                            if (!statusField) {
                                return CommandResult::Failure(ErrorCode::NotFound, "Status field not found in catalog.",
                                                              "Run fields.refresh_catalog first.");
                            }
                            std::string err;
                            const bool ok = app.SubmitFieldEdit(id, *statusField, {toStatus}, err);
                            if (!ok) {
                                return CommandResult::Failure(ErrorCode::BackendError, "Transition failed: " + err);
                            }
                            return CommandResult::Success({{"ok", true}});
                        });
        c.Destructive = true;
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Params = {
            PString("id", "Ticket id.", true),
            PString("toStatus", "Target status value.", true),
        };
        reg.Register(std::move(c));
    }

    // === fields ==========================================================

    {
        Command c = MakeCommand("fields.list_available", "List tracker fields from the local catalog.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const int limit = args.value("limit", 50);
                                    const int offset = args.value("offset", 0);
                                    const std::vector<TrackerField>& fields = app.GetAvailableFields();
                                    nlohmann::json items = nlohmann::json::array();
                                    for (const TrackerField& f : fields) {
                                        nlohmann::json one;
                                        one["id"] = f.Id;
                                        one["name"] = f.Name;
                                        one["type"] = f.Type;
                                        items.push_back(std::move(one));
                                    }
                                    return CommandResult::Success(PaginateJsonArray(items, limit, offset));
                                });
        c.Params = {
            PInt("limit", "Max items.", 50),
            PInt("offset", "Pagination offset.", 0),
        };
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("fields.get", "Get metadata for a single field by id.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string id = args.value("id", std::string());
                                    const TrackerField* f = app.FindFieldById(id);
                                    if (!f) {
                                        return CommandResult::Failure(ErrorCode::NotFound,
                                                                      "Field '" + id + "' not found in catalog.",
                                                                      "Run fields.refresh_catalog to update.");
                                    }
                                    nlohmann::json out;
                                    out["id"] = f->Id;
                                    out["name"] = f->Name;
                                    out["type"] = f->Type;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {PString("id", "Field id.", true)};
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("fields.refresh_catalog", "Re-fetch the tracker field catalog from the backend.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    const TrackerConfig cfg = ConfigManager::Load();
                                    const bool ok = app.RefreshFieldCatalog(cfg);
                                    nlohmann::json out;
                                    out["ok"] = ok;
                                    out["fieldCount"] = static_cast<int>(app.GetAvailableFields().size());
                                    if (!ok)
                                        out["error"] = app.GetFieldCatalogError();
                                    return CommandResult::Success(std::move(out));
                                });
        c.Idempotent = false;
        reg.Register(std::move(c));
    }

    // === users ===========================================================

    {
        Command c = MakeCommand("users.search", "Search tracker users by display-name substring.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string query = args.value("query", std::string());
                                    const int limit = args.value("limit", 20);
                                    std::vector<TrackerUser> users;
                                    std::string err;
                                    app.SearchUsersByQuery(query, users, err);
                                    nlohmann::json items = nlohmann::json::array();
                                    for (const TrackerUser& u : users) {
                                        nlohmann::json one;
                                        one["id"] = u.AccountId;
                                        one["displayName"] = u.DisplayName;
                                        one["email"] = u.EmailAddress;
                                        items.push_back(std::move(one));
                                    }
                                    return CommandResult::Success(PaginateJsonArray(items, limit, 0));
                                });
        c.Params = {
            PString("query", "Display-name substring.", true),
            PInt("limit", "Max results.", 20),
        };
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand(
            "users.watchers", "List watchers for a ticket.", [&app](const nlohmann::json& args, const CommandContext&) {
                const std::string ticketId = args.value("ticketId", std::string());
                std::vector<TrackerUser> watchers;
                std::string err;
                const bool ok = app.FetchIssueWatchers(ticketId, watchers, err);
                if (!ok) {
                    return CommandResult::Failure(ErrorCode::BackendError, "Watchers fetch failed: " + err);
                }
                nlohmann::json items = nlohmann::json::array();
                for (const TrackerUser& u : watchers) {
                    nlohmann::json one;
                    one["id"] = u.AccountId;
                    one["displayName"] = u.DisplayName;
                    items.push_back(std::move(one));
                }
                return CommandResult::Success(PaginateJsonArray(items, 500, 0));
            });
        c.Params = {PString("ticketId", "Ticket id.", true)};
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("users.votes", "List voters (and vote count) for a ticket.",
                        [&app](const nlohmann::json& args, const CommandContext&) {
                            const std::string ticketId = args.value("ticketId", std::string());
                            std::vector<TrackerUser> voters;
                            std::string err;
                            int voteCount = 0;
                            const bool ok = app.FetchIssueVotes(ticketId, voters, err, &voteCount);
                            if (!ok) {
                                return CommandResult::Failure(ErrorCode::BackendError, "Votes fetch failed: " + err);
                            }
                            nlohmann::json items = nlohmann::json::array();
                            for (const TrackerUser& u : voters) {
                                nlohmann::json one;
                                one["id"] = u.AccountId;
                                one["displayName"] = u.DisplayName;
                                items.push_back(std::move(one));
                            }
                            nlohmann::json out;
                            out["voteCount"] = voteCount;
                            out["voters"] = std::move(items);
                            return CommandResult::Success(std::move(out));
                        });
        c.Params = {PString("ticketId", "Ticket id.", true)};
        reg.Register(std::move(c));
    }

    // === offline ==========================================================

    {
        Command c = MakeCommand("offline.list_pending", "List pending (queued) offline creates and field edits.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const int limit = args.value("limit", 50);
                                    const int offset = args.value("offset", 0);
                                    nlohmann::json creates = nlohmann::json::array();
                                    for (const PendingCreate& pc : app.GetPendingCreates()) {
                                        nlohmann::json one;
                                        one["id"] = static_cast<long long>(pc.Id);
                                        one["attempts"] = pc.Attempts;
                                        one["payload"] = pc.Payload;
                                        creates.push_back(std::move(one));
                                    }
                                    nlohmann::json edits = nlohmann::json::array();
                                    for (const PendingFieldEditRecord& fe : app.GetPendingFieldEdits()) {
                                        nlohmann::json one;
                                        one["id"] = static_cast<long long>(fe.Id);
                                        one["ticketId"] = fe.IssueKey;
                                        one["fieldId"] = fe.FieldId;
                                        one["attempts"] = fe.Attempts;
                                        edits.push_back(std::move(one));
                                    }
                                    nlohmann::json out;
                                    out["creates"] = PaginateJsonArray(creates, limit, offset);
                                    out["fieldEdits"] = PaginateJsonArray(edits, limit, offset);
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {
            PInt("limit", "Max items per category.", 50),
            PInt("offset", "Pagination offset.", 0),
        };
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand(
            "offline.replay_now", "Immediately attempt to replay all queued offline creates and field edits.",
            [&app](const nlohmann::json&, const CommandContext& ctx) {
                if (ctx.DryRun) {
                    const size_t createCount = app.GetPendingCreateCount();
                    const size_t editCount = app.GetPendingFieldEdits().size();
                    return CommandResult::Success({{"wouldDo",
                                                    {{"pendingCreates", static_cast<int>(createCount)},
                                                     {"pendingFieldEdits", static_cast<int>(editCount)}}}});
                }
                app.TickOfflineCreates();
                app.TickOfflineFieldEdits();
                return CommandResult::Success({{"triggered", true}});
            });
        c.Destructive = true;
        c.Idempotent = false;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand(
            "offline.prune_dead", "Permanently delete all dead-letter offline creates and field edits.",
            [&app](const nlohmann::json&, const CommandContext& ctx) {
                const auto deadCreates = app.GetDeadPendingCreates();
                const auto deadEdits = app.GetDeadPendingFieldEdits();
                if (ctx.DryRun) {
                    return CommandResult::Success({{"wouldDo",
                                                    {{"deadCreates", static_cast<int>(deadCreates.size())},
                                                     {"deadFieldEdits", static_cast<int>(deadEdits.size())}}}});
                }
                std::vector<std::int64_t> cIds, eIds;
                for (const auto& c2 : deadCreates)
                    cIds.push_back(c2.DeadId);
                for (const auto& e : deadEdits)
                    eIds.push_back(e.DeadId);
                auto cs = app.DeleteDeadPendingCreates(cIds);
                auto es = app.DeleteDeadPendingFieldEdits(eIds);
                nlohmann::json out;
                out["deletedCreates"] = cs.Deleted;
                out["deletedFieldEdits"] = es.Deleted;
                return CommandResult::Success(std::move(out));
            });
        c.Destructive = true;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }

    // === config ==========================================================

    {
        Command c = MakeCommand("config.get", "Get one or all keys from the loaded config.",
                                [](const nlohmann::json& args, const CommandContext&) {
                                    const TrackerConfig cfg = ConfigManager::Load();
                                    const std::string key = args.value("key", std::string());
                                    // Build a full projection of commonly-queried fields.
                                    nlohmann::json all;
                                    all["trackerType"] = cfg.TrackerType;
                                    all["domain"] = cfg.Domain;
                                    all["email"] = cfg.Email;
                                    all["jqlQuery"] = cfg.JqlQuery;
                                    all["mcpEnabled"] = cfg.McpEnabled;
                                    all["mcpPort"] = cfg.McpPort;
                                    all["mcpAllowRemote"] = cfg.McpAllowRemote;
                                    all["mcpAllowLuaExecution"] = cfg.McpAllowLuaExecution;
                                    all["readOnlyMode"] = cfg.ReadOnlyMode;
                                    all["logMinLevel"] = cfg.LogMinLevel;
                                    all["logTrackerHttpBodies"] = cfg.LogTrackerHttpBodies;
                                    all["dbPath"] = cfg.DbPath;
                                    all["planeUrl"] = cfg.PlaneUrl;
                                    all["planeWorkspaceSlug"] = cfg.PlaneWorkspaceSlug;
                                    // Never expose token/password in output.
                                    if (!key.empty()) {
                                        if (all.contains(key)) {
                                            return CommandResult::Success({{key, all[key]}});
                                        }
                                        return CommandResult::Failure(
                                            ErrorCode::NotFound, "Config key '" + key + "' not found or not exposed.");
                                    }
                                    return CommandResult::Success(std::move(all));
                                });
        c.Params = {PString("key", "Config key name (omit for all safe keys).")};
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("config.reload", "Reload config from disk (picks up manual edits to smatchet_config.json).",
                        [](const nlohmann::json&, const CommandContext&) {
                            ConfigManager::Load(); // no-op cache flush in current impl; triggers disk read
                            return CommandResult::Success({{"triggered", true}});
                        });
        reg.Register(std::move(c));
    }

    // === perf (additional) ===============================================

    {
        Command c = MakeCommand("perf.dump", "Write perf snapshot to a JSON file and return {file, count}.",
                                [](const nlohmann::json& args, const CommandContext&) {
                                    const std::string& userDataDir = ConfigManager::GetUserDataDirectory();
                                    std::string outPath = args.value("outPath", std::string());
                                    if (outPath.empty()) {
                                        std::time_t t = std::time(nullptr);
                                        char ts[64] = {};
                                        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&t));
                                        outPath = userDataDir + "perf-snapshot-" + ts + ".json";
                                    }
                                    std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
                                    nlohmann::json doc = nlohmann::json::array();
                                    for (const UiPerfRow& r : rows) {
                                        doc.push_back({{"name", r.name},
                                                       {"lastTotalMs", r.lastTotalMs},
                                                       {"avgPerCallMs", r.avgPerCallMs},
                                                       {"maxMs", r.maxMs},
                                                       {"calls", r.calls},
                                                       {"emaAvgMs", r.emaAvgMs}});
                                    }
                                    // Write file.
                                    std::error_code ec;
                                    fs::path outFs(outPath);
                                    fs::create_directories(outFs.parent_path(), ec);
                                    std::FILE* f = std::fopen(outPath.c_str(), "wb");
                                    if (!f) {
                                        return CommandResult::Failure(
                                            ErrorCode::HandlerError, "Could not write perf dump to '" + outPath + "'.");
                                    }
                                    const std::string s = doc.dump(2);
                                    std::fwrite(s.data(), 1, s.size(), f);
                                    std::fclose(f);
                                    nlohmann::json out;
                                    out["file"] = outPath;
                                    out["count"] = static_cast<int>(rows.size());
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {PString("outPath", "Output file path (default: <userData>/perf-snapshot-<ts>.json).")};
        reg.Register(std::move(c));
    }

    // === app (additional) ================================================

    {
        Command c = MakeCommand("app.check_updates", "Check GitHub for a newer Smatchet release.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    const AppUpdateInfo info = app.CheckForAppUpdate(false);
                                    nlohmann::json out;
                                    out["ok"] = info.Ok;
                                    out["updateAvailable"] = info.UpdateAvailable;
                                    out["currentVersion"] = info.CurrentVersion;
                                    out["latestVersion"] = info.LatestVersion;
                                    out["releaseUrl"] = info.ReleaseUrl;
                                    if (!info.Error.empty())
                                        out["error"] = info.Error;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Idempotent = false;
        reg.Register(std::move(c));
    }

    // === debug (additional) ==============================================

    {
        Command c = MakeCommand("debug.mcp_status", "MCP server reachability and last-client-activity timestamp.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    nlohmann::json out;
#if defined(SMATCHET_WITH_MCP)
                                    out["mcpEnabled"] = true;
                                    std::chrono::steady_clock::time_point lastActivity;
                                    const bool hasActivity = app.TryGetMcpLastClientHttpActivity(&lastActivity);
                                    out["hasClientActivity"] = hasActivity;
                                    if (hasActivity) {
                                        const auto elapsed = std::chrono::steady_clock::now() - lastActivity;
                                        out["lastActivityMsAgo"] = static_cast<long long>(
                                            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                                    }
                                    out["activityLog"] = app.CopyMcpActivityLog();
#else
                out["mcpEnabled"] = false;
#endif
                                    return CommandResult::Success(std::move(out));
                                });
        reg.Register(std::move(c));
    }

    // === ticket (remaining) =============================================

    {
        Command c =
            MakeCommand("ticket.set_fields", "Update multiple fields on a ticket in one call.",
                        [&app](const nlohmann::json& args, const CommandContext& ctx) {
                            const std::string id = args.value("id", std::string());
                            const nlohmann::json fieldsMap = args.value("fields", nlohmann::json::object());
                            if (!fieldsMap.is_object()) {
                                return CommandResult::Failure(ErrorCode::ValidationError,
                                                              "'fields' must be a JSON object of {fieldId: value}.");
                            }
                            if (ctx.DryRun) {
                                return CommandResult::Success({{"wouldDo", {{"ticket", id}, {"fields", fieldsMap}}}});
                            }
                            nlohmann::json results = nlohmann::json::object();
                            for (const auto& kv : fieldsMap.items()) {
                                const TrackerField* f = app.FindFieldById(kv.key());
                                if (!f) {
                                    results[kv.key()] = {{"ok", false}, {"error", "field not found"}};
                                    continue;
                                }
                                std::string val =
                                    kv.value().is_string() ? kv.value().get<std::string>() : kv.value().dump();
                                std::string err;
                                const bool ok = app.SubmitFieldEdit(id, *f, {val}, err);
                                results[kv.key()] = {{"ok", ok}, {"error", err}};
                            }
                            return CommandResult::Success({{"results", results}});
                        });
        c.Destructive = true;
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Params = {
            PString("id", "Ticket id.", true),
            {[] {
                ParamSpec p;
                p.Name = "fields";
                p.Type = ParamType::Json;
                p.Required = true;
                p.Description = "JSON object: {fieldId: value, ...}";
                return p;
            }()},
        };
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand(
            "ticket.create", "Create a new ticket (live or queued offline).",
            [&app](const nlohmann::json& args, const CommandContext& ctx) {
                const bool offline = args.value("offline", false);
                const std::string projectKey = args.value("projectKey", std::string());
                const std::string issueTypeName = args.value("issueType", std::string("Task"));
                const std::string summary = args.value("summary", std::string());
                if (ctx.DryRun) {
                    return CommandResult::Success({{"wouldDo",
                                                    {{"projectKey", projectKey},
                                                     {"issueType", issueTypeName},
                                                     {"summary", summary},
                                                     {"offline", offline}}}});
                }
                IssueDraft draft;
                draft.ProjectKey = projectKey;
                draft.IssueTypeName = issueTypeName;
                draft.FieldValues["summary"] = summary;
                if (offline) {
                    const std::int64_t qid = app.QueueCreateOffline(draft);
                    if (qid <= 0) {
                        return CommandResult::Failure(ErrorCode::HandlerError, "Failed to queue offline create.");
                    }
                    return CommandResult::Success({{"queued", true}, {"offlineId", static_cast<long long>(qid)}});
                }
                auto fut = app.CreateIssueAsync(draft);
                const auto result = fut.get();
                if (!result.Ok) {
                    return CommandResult::Failure(ErrorCode::BackendError, "Create failed: " + result.Error);
                }
                return CommandResult::Success({{"ok", true}, {"issueKey", result.IssueKey}});
            });
        c.Destructive = true;
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.AsyncSafe = false;
        c.Params = {
            PString("projectKey", "Tracker project key (e.g. 'PROJ').", true),
            PString("summary", "Issue summary.", true),
            PString("issueType", "Issue type display name (default: Task)."),
            {[] {
                ParamSpec p;
                p.Name = "offline";
                p.Type = ParamType::Bool;
                p.Default = false;
                p.Description = "Queue offline rather than creating live.";
                return p;
            }()},
        };
        reg.Register(std::move(c));
    }

    // === sync (remaining) ================================================

    {
        Command c =
            MakeCommand("sync.fetch_active_view",
                        "Fetch tickets for the active view from the tracker without caching (returns inline JSON).",
                        [&app](const nlohmann::json&, const CommandContext&) {
                            TrackerIssueFetchPack pack = app.FetchIssuesForActiveView();
                            nlohmann::json items = nlohmann::json::array();
                            for (const CachedTicket& t : pack.Tickets) {
                                nlohmann::json one;
                                one["id"] = t.id;
                                auto itS = t.fieldValues.find("summary");
                                if (itS != t.fieldValues.end())
                                    one["summary"] = itS->second;
                                items.push_back(std::move(one));
                            }
                            nlohmann::json out;
                            out["tickets"] = std::move(items);
                            out["fullSyncCompleted"] = pack.FullSyncCompleted;
                            if (!pack.FetchError.empty())
                                out["error"] = pack.FetchError;
                            if (!pack.Warning.empty())
                                out["warning"] = pack.Warning;
                            return CommandResult::Success(std::move(out));
                        });
        c.Idempotent = false;
        c.AsyncSafe = false;
        reg.Register(std::move(c));
    }

    // === attach ==========================================================

    {
        Command c = MakeCommand("attach.open", "Open a ticket attachment (downloads then launches viewer).",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string url = args.value("url", std::string());
                                    const std::string filename = args.value("filename", std::string());
                                    const std::string mime = args.value("mimeType", std::string());
                                    app.OpenAttachment(url, filename, mime);
                                    return CommandResult::Success({{"opened", true}});
                                });
        c.Destructive = true;
        c.Params = {
            PString("url", "Attachment URL.", true),
            PString("filename", "Filename for download.", true),
            PString("mimeType", "MIME type (e.g. 'image/png')."),
        };
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("attach.download_preview", "Download an attachment to a local temp file for preview.",
                        [&app](const nlohmann::json& args, const CommandContext&) {
                            const std::string url = args.value("url", std::string());
                            const std::string filename = args.value("filename", std::string());
                            const std::string mime = args.value("mimeType", std::string());
                            std::string err;
                            const bool ok = app.DownloadAttachmentForPreview(url, filename, mime, &err);
                            if (!ok) {
                                return CommandResult::Failure(ErrorCode::HandlerError, "Download failed: " + err);
                            }
                            return CommandResult::Success({{"ok", true}});
                        });
        c.Idempotent = false;
        c.Params = {
            PString("url", "Attachment URL.", true),
            PString("filename", "Local filename.", true),
            PString("mimeType", "MIME type."),
        };
        reg.Register(std::move(c));
    }

    // === fields (remaining) ==============================================

    {
        Command c = MakeCommand("fields.icon_for", "Resolve the icon asset path/URL for a field+value pair.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string fieldId = args.value("field", std::string());
                                    const std::string rawValue = args.value("value", std::string());
                                    const TrackerField* f = app.FindFieldById(fieldId);
                                    std::string outPathOrUrl;
                                    const bool found = app.TryGetFieldIconMapTarget(fieldId, f, rawValue, outPathOrUrl);
                                    nlohmann::json out;
                                    out["found"] = found;
                                    out["pathOrUrl"] = found ? outPathOrUrl : std::string();
                                    out["field"] = fieldId;
                                    out["value"] = rawValue;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {
            PString("field", "Field id (e.g. 'priority').", true),
            PString("value", "Raw field value.", true),
        };
        reg.Register(std::move(c));
    }

    // === config (remaining) ==============================================

    {
        // config.set — bidirectional key table: cmd key name → JSON file key name.
        // Precedence contract: env vars beat the JSON file. config.set writes to the JSON
        // file, so it is overridden by SMATCHET_* env vars if both are set simultaneously.
        // Keys marked (restart) require the app or MCP plugin to restart to take effect;
        // all others are picked up on the next ConfigManager::Load() call (next sync, etc.).
        //
        // Credentials (ApiToken, PlaneApiKey, McpAuthToken) are NOT in this table — use
        // SMATCHET_TRACKER_TOKEN / SMATCHET_MCP_AUTH_TOKEN env vars for secrets.
        struct CfgKey {
            const char* cmd;
            const char* json;
            const char* hint;
        };
        static const CfgKey kKeys[] = {
            {"readOnlyMode", "read_only_mode", ""},
            {"logMinLevel", "log_min_level", ""},
            {"logTrackerHttpBodies", "log_tracker_http_bodies", ""},
            {"showPerformance", "show_performance_window", ""},
            {"showLogWindow", "show_log_window", ""},
            {"enableFieldOverflowTooltips", "field_overflow_tooltips", ""},
            {"jqlQuery", "jql", "takes effect on next sync"},
            {"domain", "domain", "restart required to reconnect"},
            {"email", "email", "restart required to reconnect"},
            // PR 6: `projectKey` / `planeProjectId` writable keys removed. Project is now
            // per-operation; pass it on `ticket.create` (required) or pick via the in-app picker.
            {"trackerType", "tracker_type", "restart required"},
            {"planeUrl", "plane_url", "restart required to reconnect"},
            {"planeWorkspaceSlug", "plane_workspace_slug", "restart required to reconnect"},
            {"mcpEnabled", "mcp_enabled", "MCP plugin restart required"},
            {"mcpPort", "mcp_port", "MCP plugin restart required"},
            {"mcpAllowRemote", "mcp_allow_remote", "MCP plugin restart required"},
            {"mcpAllowLuaExecution", "mcp_allow_lua_execution", "MCP plugin restart required"},
            {nullptr, nullptr, nullptr},
        };

        Command c = MakeCommand(
            "config.set", "Persist one config key to smatchet_config.json (allowlisted; env vars override).",
            [](const nlohmann::json& args, const CommandContext& ctx) {
                const std::string key = args.value("key", std::string());
                // Parse the string value as JSON first (handles true/false/integers).
                // Fall back to a plain JSON string so bare values like `debug` work.
                const std::string rawVal = args.value("value", std::string());
                nlohmann::json val;
                try {
                    val = nlohmann::json::parse(rawVal);
                } catch (...) {
                    val = rawVal; // treat as plain string
                }
                const CfgKey* found = nullptr;
                for (int i = 0; kKeys[i].cmd; ++i) {
                    if (key == kKeys[i].cmd) {
                        found = &kKeys[i];
                        break;
                    }
                }
                if (!found) {
                    std::string allowed;
                    for (int i = 0; kKeys[i].cmd; ++i) {
                        if (i)
                            allowed += ", ";
                        allowed += kKeys[i].cmd;
                    }
                    return CommandResult::Failure(ErrorCode::ValidationError,
                                                  "Key '" + key + "' is not in the config.set allowlist.",
                                                  "Allowed: " + allowed);
                }
                if (ctx.DryRun) {
                    return CommandResult::Success(
                        {{"wouldDo", {{"cmdKey", key}, {"jsonKey", found->json}, {"value", val}}}});
                }
                nlohmann::json cfgJson = ConfigManager::LoadMergedConfigJson();
                cfgJson[found->json] = val;
                ConfigManager::WriteConfigJson(cfgJson);
                // Invalidate the Load() cache so next call picks up the new value.
                ConfigManager::InvalidateCache();
                nlohmann::json out;
                out["key"] = key;
                out["jsonKey"] = found->json;
                out["value"] = val;
                out["written"] = true;
                if (found->hint && found->hint[0]) {
                    out["hint"] = std::string(found->hint);
                }
                return CommandResult::Success(std::move(out));
            });
        c.Destructive = false; // Config edits are easily undone; --yes would be friction.
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Description = "cmd-key → JSON-key mapping is bidirectional with config.get. "
                        "Credentials (token, apiKey) are not settable via this command; "
                        "use SMATCHET_TRACKER_TOKEN env var instead.";
        c.Params = {
            PString("key", "Config key name (camelCase, from config.get).", true),
            // String type: bare values like `debug` and `true` both work.
            // The handler parses JSON (for bools/ints) then falls back to treating
            // the input as a plain string so users don't need to escape quotes.
            PString("value", "New value — plain string, bool (true/false), or integer.", true),
        };
        reg.Register(std::move(c));
    }

    // === perf (remaining) ================================================

    {
        Command c =
            MakeCommand("perf.reset", "Clear all accumulated UI perf measurements (use before a benchmark run).",
                        [](const nlohmann::json&, const CommandContext&) {
                            UiPerfMonitor::Instance().Reset();
                            return CommandResult::Success({{"reset", true}});
                        });
        reg.Register(std::move(c));
    }

    // === app (remaining) =================================================

    {
        Command c = MakeCommand("app.set_readonly", "Enable or disable read-only mode (persists to config).",
                                [](const nlohmann::json& args, const CommandContext& ctx) {
                                    const bool on = args.value("on", false);
                                    if (ctx.DryRun) {
                                        return CommandResult::Success({{"wouldDo", {{"readOnlyMode", on}}}});
                                    }
                                    TrackerConfig cfg = ConfigManager::Load();
                                    cfg.ReadOnlyMode = on;
                                    ConfigManager::Save(cfg);
                                    ConfigManager::InvalidateCache();
                                    return CommandResult::Success({{"readOnlyMode", on}});
                                });
        c.Destructive = true;
        c.DryRunSupported = true;
        c.Params = {
            {[] {
                ParamSpec p;
                p.Name = "on";
                p.Type = ParamType::Bool;
                p.Required = true;
                p.Description = "true = enable read-only, false = disable.";
                return p;
            }()},
        };
        reg.Register(std::move(c));
    }

    // === perf.toggle_panel + debug.thread_dump ==========================

    {
        Command c = MakeCommand("perf.toggle_panel", "Show or hide the Performance Monitor panel (persists to config).",
                                [](const nlohmann::json& args, const CommandContext& ctx) {
                                    // Read current value to compute toggle if `open` not supplied.
                                    TrackerConfig cfg = ConfigManager::Load();
                                    const bool currentlyOpen = cfg.ShowPerformanceWindow;
                                    const bool newOpen =
                                        args.contains("open") ? args.value("open", false) : !currentlyOpen;
                                    if (ctx.DryRun) {
                                        return CommandResult::Success({{"wouldDo", {{"showPerformance", newOpen}}}});
                                    }
                                    nlohmann::json cfgJson = ConfigManager::LoadMergedConfigJson();
                                    cfgJson["show_performance_window"] = newOpen;
                                    ConfigManager::WriteConfigJson(cfgJson);
                                    ConfigManager::InvalidateCache();
                                    return CommandResult::Success({{"showPerformance", newOpen}});
                                });
        c.DryRunSupported = true;
        c.Params = {
            {[] {
                ParamSpec p;
                p.Name = "open";
                p.Type = ParamType::Bool;
                p.Description = "true=show, false=hide, omit=toggle current state.";
                return p;
            }()},
        };
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("debug.thread_dump", "Return basic thread count and state information.",
                        [](const nlohmann::json&, const CommandContext&) {
                            // C++14 doesn't expose thread IDs without platform headers.
                            // Return what we can without OS-specific code in Source_Core.
                            nlohmann::json out;
                            out["note"] =
                                "Thread dump requires OS-specific APIs; counts are unavailable via this command.";
                            out["hardwareConcurrency"] = static_cast<int>(std::thread::hardware_concurrency());
                            return CommandResult::Success(std::move(out));
                        });
        // Include <thread> already in CommandRegistry.h; add locally:
        c.Description = "Returns hardware_concurrency. Full OS thread dump not available cross-platform.";
        reg.Register(std::move(c));
    }

    // === debug (remaining) ===============================================

    {
        Command c =
            MakeCommand("debug.lua_eval", "Evaluate a Lua code snippet and return the result summary.",
                        [&app](const nlohmann::json& args, const CommandContext& /*ctx*/) {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
                            const std::string code = args.value("code", std::string());
                            std::string outError;
                            std::string outResult;
                            const bool ok = app.ExecuteLuaConsoleSnippet(code, outError, outResult);
                            if (!ok) {
                                return CommandResult::Failure(ErrorCode::HandlerError, "Lua eval failed: " + outError);
                            }
                            return CommandResult::Success({{"result", outResult}});
#else
                (void)app;
                (void)args;
                return CommandResult::Failure(ErrorCode::HandlerError,
                    "Lua automation is not enabled in this build.");
#endif
                        });
        c.Destructive = true;
        c.Idempotent = false;
        c.Params = {PString("code", "Lua code to evaluate.", true)};
        reg.Register(std::move(c));
    }

    // === scenario ========================================================

    // Register scenario factories on the runner owned by AppController.
    // The PriorityGridScrollScenario is the first built-in; others can be
    // added by appending more RegisterFactory calls here.
    {
        // PriorityGridScrollScenario factory — defined in ScenarioRunner.cpp but
        // the concrete type is in PriorityGridScrollScenario.cpp. Forward-declare
        // the factory maker via a helper.
        // (Registration delegated to AppController::Scenarios().RegisterFactory so
        // the runner lives on AppController, not in the registry.)
        //
        // We register scenario.* CLI commands through the registry as normal;
        // actual execution is delegated to the ScenarioRunner.
    }

    {
        // scenario.list reads factories_ which is set up once in Initialize and never
        // mutated afterwards (built-in factories are registered before any worker is
        // spawned). Safe to read from any thread — no dispatch hop needed.
        Command c = MakeCommand("scenario.list", "List registered scenario names.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    const std::vector<std::string> names = app.Scenarios().ListNames();
                                    return CommandResult::Success(PaginateString(names, 500, 0));
                                });
        reg.Register(std::move(c));
    }

    {
        // scenario.run writes ScenarioRunner::active_ (a unique_ptr) which the UI thread
        // dereferences every frame inside Tick(). Mutating active_ from an MCP worker
        // races with Tick. Hop to UI thread so Start() runs while Tick() is paused
        // between frames.
        Command c =
            MakeCommand("scenario.run", "Run a named automation scenario (perf measurement, scroll driver, etc.).",
                        [&app](const nlohmann::json& args, const CommandContext& ctx) {
                            // Copy ctx into the closure so the UI-thread lambda has the dry-run /
                            // destructive flags (it cannot capture the const reference safely across
                            // a thread boundary — the original ctx may live on the worker stack).
                            CommandContext ctxCopy = ctx;
                            return RunOnUiThreadAsCommandResult(app, [&app, args, ctxCopy]() {
                                const std::string name = args.value("name", std::string());
                                return app.Scenarios().Start(name, args, ctxCopy);
                            });
                        });
        c.Destructive = true;
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        c.Params = {
            PString("name", "Scenario name (from scenario.list).", true),
            PInt("frames", "Frame count to run.", 600),
            PString("outPath", "Output JSON file path (default: auto-generated in <userData>/perf/)."),
        };
        reg.Register(std::move(c));
    }

    {
        // scenario.cancel resets ScenarioRunner::active_ while Tick() may be reading it.
        // Same race as scenario.run — must run on UI thread.
        Command c = MakeCommand("scenario.cancel", "Abort the active running scenario.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    return RunOnUiThreadAsCommandResult(app, [&app]() {
                                        const bool was = app.Scenarios().Active();
                                        app.Scenarios().Cancel();
                                        return CommandResult::Success({{"wasCancelled", was}});
                                    });
                                });
        reg.Register(std::move(c));
    }

    // === debug — dock diagnostics ============================================

    {
        // debug.dock.dump logs the state of key dock nodes to the runtime log.
        // Handler runs on the UI thread because ImGui::DockBuilderGetNode reads
        // the imgui dock-node table which is owned by the UI thread.
        Command c = MakeCommand("debug.dock.dump", "Log all dock nodes to the runtime log.",
                                [&app](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                                    return RunOnUiThreadAsCommandResult(app, []() {
                                        static const ImGuiID kNodes[] = {
                                            SmatchetDockNodeIds::kPrimarySideBar,
                                            SmatchetDockNodeIds::kBottomPanel,
                                            SmatchetDockNodeIds::kSecondarySideBar,
                                        };
                                        static const char* const kNames[] = {
                                            "PrimarySideBar(0x4)",
                                            "BottomPanel(0xA)",
                                            "SecondarySideBar(0x10)",
                                        };
                                        for (int i = 0; i < 3; ++i) {
                                            ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(kNodes[i]);
                                            if (!node) {
                                                LOG_INFO("debug.dock.dump: %s NOT FOUND", kNames[i]);
                                            } else {
                                                LOG_INFO(
                                                    "debug.dock.dump: %s id=0x%08X size=(%.0f,%.0f) "
                                                    "flags=0x%X tabs=%d empty=%d",
                                                    kNames[i],
                                                    static_cast<unsigned int>(node->ID),
                                                    node->Size.x, node->Size.y,
                                                    static_cast<int>(node->LocalFlags),
                                                    node->Windows.Size,
                                                    static_cast<int>(node->IsEmpty()));
                                            }
                                        }
                                        // Walk the full dockspace tree from the root node.
                                        const ImGuiID kDockSpaceId = 0x08BD597Du;
                                        ImGuiDockNode* root = ::ImGui::DockBuilderGetNode(kDockSpaceId);
                                        if (!root) {
                                            LOG_INFO("debug.dock.dump: dockspace root (0x%08X) NOT FOUND",
                                                     static_cast<unsigned int>(kDockSpaceId));
                                        } else {
                                            // Iterative depth-first walk (no recursion; C++14 safe).
                                            struct Frame {
                                                ImGuiDockNode* node;
                                                int depth;
                                            };
                                            std::vector<Frame> stack;
                                            stack.push_back({root, 0});
                                            while (!stack.empty()) {
                                                Frame frame = stack.back();
                                                stack.pop_back();
                                                ImGuiDockNode* n = frame.node;
                                                if (!n) { continue; }
                                                // Build depth-indent prefix.
                                                std::string indent(
                                                    static_cast<size_t>(frame.depth * 2), ' ');
                                                LOG_INFO(
                                                    "debug.dock.dump: %sid=0x%08X size=(%.0f,%.0f) "
                                                    "flags=0x%X tabs=%d empty=%d",
                                                    indent.c_str(),
                                                    static_cast<unsigned int>(n->ID),
                                                    n->Size.x, n->Size.y,
                                                    static_cast<int>(n->LocalFlags),
                                                    n->Windows.Size,
                                                    static_cast<int>(n->IsEmpty()));
                                                // Push children (right first so left is processed first).
                                                if (n->ChildNodes[1]) {
                                                    stack.push_back({n->ChildNodes[1], frame.depth + 1});
                                                }
                                                if (n->ChildNodes[0]) {
                                                    stack.push_back({n->ChildNodes[0], frame.depth + 1});
                                                }
                                            }
                                        }
                                        nlohmann::json out;
                                        out["logged"] = true;
                                        return CommandResult::Success(std::move(out));
                                    });
                                });
        reg.Register(std::move(c));
    }

    {
        // debug.dock.reset forces a full dock layout reset to the VS-shell default.
        // Must run on the UI thread — SmatchetUI_ResetLayoutToDefault touches ImGui + g_ui.
        Command c = MakeCommand("debug.dock.reset", "Force reset dock layout to VS shell default.",
                                [&app](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                                    return RunOnUiThreadAsCommandResult(app, []() {
                                        SmatchetUI_ResetLayoutToDefault(g_ui);
                                        nlohmann::json out;
                                        out["reset"] = true;
                                        return CommandResult::Success(std::move(out));
                                    });
                                });
        c.Destructive = true;
        c.Idempotent = true;
        reg.Register(std::move(c));
    }

    RegisterViewToggleCommands(reg, app);

    LOG_INFO("CommandRegistry: registered %zu built-in commands", reg.All().size());
}

} // namespace cmd
} // namespace smatchet
