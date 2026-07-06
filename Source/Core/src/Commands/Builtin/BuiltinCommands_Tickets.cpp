// tickets.* — read-only queries over the active project grid snapshot.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

// fan-in Phase 5: depend on the narrow IAppTicketData facet, not the full AppController.h.
// (CachedTicket — the snapshot element — comes from the facet's rank-0 CachedTicketTypes.h.)
#include "Interfaces/IAppTicketData.h"
#include <nlohmann/json.hpp>   // this TU constructs nlohmann::json directly.
#include "LocalCacheManager.h" // pre-existing direct include (kept; carries CachedTicketTypes.h + cache decls)

#include <algorithm>
#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PaginateJsonArray;
using builtin_detail::PInt;
using builtin_detail::PString;
using builtin_detail::ToLowerAscii;

namespace {

void RegisterTicketsListActiveCommand(CommandRegistry& reg, IAppTicketData& app) {
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
}

void RegisterTicketsSearchActiveCommand(CommandRegistry& reg, IAppTicketData& app) {
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
                                                hit = std::any_of(
                                                    t.fieldValues.begin(), t.fieldValues.end(),
                                                    [&q](const std::pair<const std::string, std::string>& kv) {
                                                        return ToLowerAscii(kv.second).find(q) != std::string::npos;
                                                    });
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
}

void RegisterTicketsGetCommand(CommandRegistry& reg, IAppTicketData& app) {
    {
        Command c =
            MakeCommand("tickets.get", "Full field map for a single active-view ticket.",
                        [&app](const nlohmann::json& args, const CommandContext&) {
                            const std::string id = args.value("id", std::string());
                            auto snapshot = app.GetActiveTicketsSnapshot();
                            if (snapshot) {
                                auto it = std::find_if(snapshot->begin(), snapshot->end(),
                                                       [&id](const CachedTicket& t) { return t.id == id; });
                                if (it != snapshot->end()) {
                                    nlohmann::json one;
                                    one["id"] = it->id;
                                    nlohmann::json fields = nlohmann::json::object();
                                    for (const auto& kv : it->fieldValues) {
                                        fields[kv.first] = kv.second;
                                    }
                                    one["fields"] = std::move(fields);
                                    return CommandResult::Success(std::move(one));
                                }
                            }
                            return CommandResult::Failure(
                                ErrorCode::NotFound, "Ticket '" + id + "' not found in active view.",
                                "Verify the id matches the current view; use tickets.list_active to enumerate.");
                        });
        c.Params = {PString("id", "Ticket id (e.g. 'PROJ-1').", /*required*/ true)};
        reg.Register(std::move(c));
    }
}

void RegisterTicketsExistsCommand(CommandRegistry& reg, IAppTicketData& app) {
    {
        Command c = MakeCommand("tickets.exists", "Whether a ticket id is present in the active view.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string id = args.value("id", std::string());
                                    auto snapshot = app.GetActiveTicketsSnapshot();
                                    bool exists = false;
                                    if (snapshot) {
                                        exists = std::any_of(snapshot->begin(), snapshot->end(),
                                                             [&id](const CachedTicket& t) { return t.id == id; });
                                    }
                                    nlohmann::json out;
                                    out["exists"] = exists;
                                    out["id"] = id;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {PString("id", "Ticket id.", /*required*/ true)};
        reg.Register(std::move(c));
    }
}

} // namespace

void RegisterTicketsCommands(CommandRegistry& reg, IAppTicketData& app) {
    RegisterTicketsListActiveCommand(reg, app);
    RegisterTicketsSearchActiveCommand(reg, app);
    RegisterTicketsGetCommand(reg, app);
    RegisterTicketsExistsCommand(reg, app);
}

} // namespace cmd
} // namespace smatchet
