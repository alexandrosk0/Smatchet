// commands.* — discovery / introspection commands.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PaginateJsonArray;
using builtin_detail::PaginateString;
using builtin_detail::PInt;
using builtin_detail::PString;

namespace {

void RegisterCommandsListCommand(CommandRegistry& reg) {
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
                                                if (p.Default && !p.Default->is_null())
                                                    pj["default"] = *p.Default;
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
                p.Default = std::make_shared<nlohmann::json>(false);
                p.Description = "Include compact per-param schema per command.";
                return p;
            }()},
        };
        reg.Register(std::move(c));
    }
}

void RegisterCommandsHelpCommand(CommandRegistry& reg) {
    {
        Command c = MakeCommand(
            "commands.help", "Return the full schema (params, flags, examples) for one command.",
            [&reg](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                const std::string name = args.value("name", std::string());
                std::vector<Command> all = reg.All();
                auto it = std::find_if(all.begin(), all.end(), [&name](const Command& cm) { return cm.Name == name; });
                if (it != all.end()) {
                    const Command& cm = *it;
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
                std::vector<std::string> suggestions = reg.FuzzyMatch(name, 3);
                return CommandResult::Failure(ErrorCode::NotFound, "No command named '" + name + "'.",
                                              suggestions.empty() ? std::string()
                                                                  : ("Did you mean '" + suggestions.front() + "'?"),
                                              std::move(suggestions));
            });
        c.Params = {PString("name", "Command name to describe.", /*required*/ true)};
        c.Description = "Returns inputSchema (JSON Schema) + helpText (human readable).";
        reg.Register(std::move(c));
    }
}

void RegisterCommandsSearchCommand(CommandRegistry& reg) {
    {
        Command c = MakeCommand("commands.search", "Fuzzy-match command names by query.",
                                [&reg](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                                    const std::string q = args.value("query", std::string());
                                    const int limit = args.value("limit", 10);
                                    std::vector<std::string> matches =
                                        reg.FuzzyMatch(q, static_cast<size_t>((std::max)(1, limit)));
                                    return CommandResult::Success(PaginateString(matches, limit, 0));
                                });
        c.Params = {
            PString("query", "Fuzzy search string.", /*required*/ true),
            PInt("limit", "Max matches.", 10),
        };
        reg.Register(std::move(c));
    }
}

void RegisterCommandsRecentsCommand(CommandRegistry& reg) {
    {
        Command c = MakeCommand("commands.recents", "Most recently dispatched command names.",
                                [&reg](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                                    const int limit = args.value("limit", 16);
                                    std::vector<std::string> r = reg.Recents(static_cast<size_t>((std::max)(1, limit)));
                                    return CommandResult::Success(PaginateString(r, limit, 0));
                                });
        c.Params = {PInt("limit", "Max items.", 16)};
        reg.Register(std::move(c));
    }
}

} // namespace

void RegisterMetaCommands(CommandRegistry& reg) {
    RegisterCommandsListCommand(reg);
    RegisterCommandsHelpCommand(reg);
    RegisterCommandsSearchCommand(reg);
    RegisterCommandsRecentsCommand(reg);

    // `commands.invoke` is implemented inside the Lua bridge directly (it would
    // be circular here — see AppController_LuaBindings.cpp Phase 2).
}

} // namespace cmd
} // namespace smatchet
