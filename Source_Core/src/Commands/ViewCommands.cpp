// view.* command group — registered from SmatchetUI once ViewState is ready.
// See backlog/COMMAND_SYSTEM_PLAN.md §"Initial command catalogue – view".

#include "Commands/ViewCommands.h"

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "Views.h"

#include <string>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

nlohmann::json ViewDefToJson(const ViewDefinition& v) {
    nlohmann::json j;
    j["id"]     = v.Id;
    j["name"]   = v.Name;
    j["jql"]    = v.Jql;
    j["fields"] = v.Fields;
    return j;
}

nlohmann::json PaginateViewDefs(const std::vector<ViewDefinition>& views,
                                 int limit, int offset) {
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    if (offset < 0) offset = 0;
    const int total = static_cast<int>(views.size());
    nlohmann::json arr = nlohmann::json::array();
    for (int i = offset; i < total && static_cast<int>(arr.size()) < limit; ++i) {
        arr.push_back(ViewDefToJson(views[i]));
    }
    nlohmann::json out;
    out["items"]   = std::move(arr);
    out["total"]   = total;
    out["limit"]   = limit;
    out["offset"]  = offset;
    out["hasMore"] = (offset + static_cast<int>(out["items"].size())) < total;
    return out;
}

}  // namespace

void RegisterViewCommands(AppController& app, Views& views) {
    CommandRegistry& reg = app.Commands();
    // Idempotent guard — don't re-register on second call.
    if (reg.HasExact("view.list")) return;

    {
        Command c;
        c.Name = "view.list"; c.Category = "view";
        c.Summary = "List all configured ticket-grid views.";
        c.Params = {[]{ ParamSpec p; p.Name="limit"; p.Type=ParamType::Int; p.Default=50; return p; }(),
                    []{ ParamSpec p; p.Name="offset"; p.Type=ParamType::Int; p.Default=0; return p; }()};
        c.Handler = [&views](const nlohmann::json& args, CommandContext&) {
            const ViewsStore& store = views.GetStore();
            return CommandResult::Success(
                PaginateViewDefs(store.Views,
                                 args.value("limit", 50),
                                 args.value("offset", 0)));
        };
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "view.get"; c.Category = "view";
        c.Summary = "Get definition of a single view by id.";
        c.Params = {[]{ ParamSpec p; p.Name="id"; p.Type=ParamType::String;
                        p.Required=true; p.Description="View id."; return p; }()};
        c.Handler = [&views](const nlohmann::json& args, CommandContext&) {
            const std::string id = args.value("id", std::string());
            const ViewsStore& store = views.GetStore();
            for (const ViewDefinition& v : store.Views) {
                if (v.Id == id) return CommandResult::Success(ViewDefToJson(v));
            }
            return CommandResult::Failure(ErrorCode::NotFound,
                "View '" + id + "' not found.");
        };
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "view.current"; c.Category = "view";
        c.Summary = "Get the currently active view.";
        c.Handler = [&views](const nlohmann::json&, CommandContext&) {
            const ViewDefinition* active = views.GetActiveView();
            if (!active) {
                return CommandResult::Failure(ErrorCode::NotFound,
                    "No active view configured.");
            }
            return CommandResult::Success(ViewDefToJson(*active));
        };
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "view.activate"; c.Category = "view";
        c.Summary = "Switch the active view by id.";
        c.Params = {[]{ ParamSpec p; p.Name="id"; p.Type=ParamType::String;
                        p.Required=true; p.Description="View id from view.list."; return p; }()};
        c.Handler = [&views, &app](const nlohmann::json& args, CommandContext&) {
            const std::string id = args.value("id", std::string());
            if (!views.Activate(id)) {
                return CommandResult::Failure(ErrorCode::NotFound,
                    "View '" + id + "' not found.");
            }
            views.Save();
            app.SyncWithBackend(nullptr, &views.GetStore());
            return CommandResult::Success({{"activated", id}});
        };
        c.Idempotent = false;
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "view.refresh_active"; c.Category = "view";
        c.Summary = "Re-sync tickets for the active view from the tracker.";
        c.Handler = [&app, &views](const nlohmann::json&, CommandContext&) {
            app.SyncWithBackend(nullptr, &views.GetStore());
            return CommandResult::Success({{"triggered", true}});
        };
        c.Idempotent = false;
        c.AsyncSafe = false;
        reg.Register(std::move(c));
    }
}

}  // namespace cmd
}  // namespace smatchet
