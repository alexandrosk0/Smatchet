// view.* command group — registered from SmatchetUI once ViewState is ready.
// See docs/design/applied/command-system-plan.md §"Initial command catalogue – view".

#include "Commands/ViewCommands.h"

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
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
        // view.list reads ViewState.Slice_.Views — written by Views Dashboard window on
        // the UI thread without an internal mutex. Hop to UI thread for race-free read.
        c.Handler = [&app, &views](const nlohmann::json& args, const CommandContext&) {
            return RunOnUiThreadAsCommandResult(app, [&views, args]() {
                const ViewsStore& store = views.GetStore();
                return CommandResult::Success(
                    PaginateViewDefs(store.Views,
                                     args.value("limit", 50),
                                     args.value("offset", 0)));
            });
        };
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "view.get"; c.Category = "view";
        c.Summary = "Get definition of a single view by id.";
        c.Params = {[]{ ParamSpec p; p.Name="id"; p.Type=ParamType::String;
                        p.Required=true; p.Description="View id."; return p; }()};
        c.Handler = [&app, &views](const nlohmann::json& args, const CommandContext&) {
            return RunOnUiThreadAsCommandResult(app, [&views, args]() {
                const std::string id = args.value("id", std::string());
                const ViewsStore& store = views.GetStore();
                for (const ViewDefinition& v : store.Views) {
                    if (v.Id == id) return CommandResult::Success(ViewDefToJson(v));
                }
                return CommandResult::Failure(ErrorCode::NotFound,
                    "View '" + id + "' not found.");
            });
        };
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "view.current"; c.Category = "view";
        c.Summary = "Get the currently active view.";
        c.Handler = [&app, &views](const nlohmann::json&, const CommandContext&) {
            return RunOnUiThreadAsCommandResult(app, [&views]() {
                const ViewDefinition* active = views.GetActiveView();
                if (!active) {
                    return CommandResult::Failure(ErrorCode::NotFound,
                        "No active view configured.");
                }
                return CommandResult::Success(ViewDefToJson(*active));
            });
        };
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "view.activate"; c.Category = "view";
        c.Summary = "Switch the active view by id.";
        c.Params = {[]{ ParamSpec p; p.Name="id"; p.Type=ParamType::String;
                        p.Required=true; p.Description="View id from view.list."; return p; }()};
        // view.activate mutates ActiveViewId + persists views + kicks a sync. All three
        // touch UI-thread-owned state; do the mutation on the UI thread, then return.
        c.Handler = [&app, &views](const nlohmann::json& args, const CommandContext&) {
            return RunOnUiThreadAsCommandResult(app, [&app, &views, args]() {
                const std::string id = args.value("id", std::string());
                if (!views.Activate(id)) {
                    return CommandResult::Failure(ErrorCode::NotFound,
                        "View '" + id + "' not found.");
                }
                views.Save();
                app.SyncWithBackend(nullptr, &views.GetStore());
                return CommandResult::Success({{"activated", id}});
            });
        };
        c.Idempotent = false;
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "view.refresh_active"; c.Category = "view";
        c.Summary = "Re-sync tickets for the active view from the tracker.";
        c.Handler = [&app, &views](const nlohmann::json&, const CommandContext&) {
            // SyncWithBackend reads ViewState.Slice_.Views to build the JQL; hop to UI thread
            // so the read is serialised with any concurrent Views Dashboard mutation.
            return RunOnUiThreadAsCommandResult(app, [&app, &views]() {
                app.SyncWithBackend(nullptr, &views.GetStore());
                return CommandResult::Success({{"triggered", true}});
            });
        };
        c.Idempotent = false;
        c.AsyncSafe = false;
        reg.Register(std::move(c));
    }

    // ---- view.create -----------------------------------------------------
    // Create a new view from a prototype. After Create() the new view becomes
    // active automatically (Views::Create sets ActiveViewId to the new id), so
    // the next sync will fetch tickets matching the new JQL.
    {
        Command c;
        c.Name = "view.create"; c.Category = "view";
        c.Summary = "Create a new ticket-grid view.";
        c.Description = "Creates a view with the given name and (optionally) JQL + fields. "
                        "The new view is auto-activated; pass triggerSync=true to also "
                        "kick off a sync immediately after creation.";
        c.Destructive = false;   // creation is easily undone with view.delete
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Params = {
            []{ ParamSpec p; p.Name="name"; p.Type=ParamType::String; p.Required=true;
                p.Description="Human-readable view name (also used to derive id)."; return p; }(),
            []{ ParamSpec p; p.Name="jql"; p.Type=ParamType::String;
                p.Description="JQL filter (default: assignee=currentUser())."; return p; }(),
            []{ ParamSpec p; p.Name="fields"; p.Type=ParamType::Json;
                p.Description="JSON array of field ids to display (default: empty)."; return p; }(),
            []{ ParamSpec p; p.Name="triggerSync"; p.Type=ParamType::Bool; p.Default=false;
                p.Description="If true, also sync from tracker immediately after create."; return p; }(),
        };
        // view.create writes Views::Slice_ + persists to disk + may trigger a sync.
        // All steps run on the UI thread to avoid racing the Views Dashboard.
        c.Handler = [&app, &views](const nlohmann::json& args, const CommandContext& ctx) {
            const bool dryRun = ctx.DryRun;
            return RunOnUiThreadAsCommandResult(app, [&app, &views, args, dryRun]() {
                const std::string name = args.value("name", std::string());
                const std::string jql  = args.value("jql",  std::string());
                const bool triggerSync = args.value("triggerSync", false);

                ViewDefinition proto;
                proto.Name = name;
                if (!jql.empty()) proto.Jql = jql;
                if (args.contains("fields") && args["fields"].is_array()) {
                    for (const auto& f : args["fields"]) {
                        if (f.is_string()) proto.Fields.push_back(f.get<std::string>());
                    }
                }

                if (dryRun) {
                    nlohmann::json wd;
                    wd["name"]   = proto.Name;
                    wd["jql"]    = proto.Jql;
                    wd["fields"] = proto.Fields;
                    return CommandResult::Success({{"wouldDo", std::move(wd)}});
                }

                if (!views.Create(proto)) {
                    return CommandResult::Failure(ErrorCode::HandlerError,
                        "Views::Create() failed.");
                }
                const ViewDefinition* created = views.GetActiveView();
                nlohmann::json out;
                if (created) {
                    out["id"]   = created->Id;
                    out["name"] = created->Name;
                    out["jql"]  = created->Jql;
                }
                out["created"] = true;
                if (triggerSync) {
                    app.SyncWithBackend(nullptr, &views.GetStore());
                    out["syncTriggered"] = true;
                }
                return CommandResult::Success(std::move(out));
            });
        };
        reg.Register(std::move(c));
    }

    // ---- view.update -----------------------------------------------------
    // Update fields on the currently active view. Pass only the keys you want
    // to change; omitted fields preserve their current value.
    {
        Command c;
        c.Name = "view.update"; c.Category = "view";
        c.Summary = "Update one or more attributes of the currently active view.";
        c.Description = "Edits the active view in place. Pass only the params you want to "
                        "change — omitted keys preserve their current value. To switch which "
                        "view is active first, use view.activate.";
        c.Destructive = false;
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Params = {
            []{ ParamSpec p; p.Name="name"; p.Type=ParamType::String;
                p.Description="New display name (id is preserved)."; return p; }(),
            []{ ParamSpec p; p.Name="jql"; p.Type=ParamType::String;
                p.Description="New JQL filter."; return p; }(),
            []{ ParamSpec p; p.Name="fields"; p.Type=ParamType::Json;
                p.Description="Replacement JSON array of field ids."; return p; }(),
        };
        // view.update reads + writes the active ViewDefinition; UI thread only.
        c.Handler = [&app, &views](const nlohmann::json& args, const CommandContext& ctx) {
            const bool dryRun = ctx.DryRun;
            return RunOnUiThreadAsCommandResult(app, [&views, args, dryRun]() {
                const ViewDefinition* active = views.GetActiveView();
                if (!active) {
                    return CommandResult::Failure(ErrorCode::NotFound,
                        "No active view to update.");
                }
                ViewDefinition updated = *active;
                if (args.contains("name") && args["name"].is_string()) {
                    updated.Name = args["name"].get<std::string>();
                }
                if (args.contains("jql") && args["jql"].is_string()) {
                    updated.Jql = args["jql"].get<std::string>();
                }
                if (args.contains("fields") && args["fields"].is_array()) {
                    updated.Fields.clear();
                    for (const auto& f : args["fields"]) {
                        if (f.is_string()) updated.Fields.push_back(f.get<std::string>());
                    }
                }

                if (dryRun) {
                    nlohmann::json wd;
                    wd["from"] = {{"name", active->Name}, {"jql", active->Jql}};
                    wd["to"]   = {{"name", updated.Name}, {"jql", updated.Jql},
                                  {"fields", updated.Fields}};
                    return CommandResult::Success({{"wouldDo", std::move(wd)}});
                }

                if (!views.UpdateActive(updated)) {
                    return CommandResult::Failure(ErrorCode::HandlerError,
                        "Views::UpdateActive() failed.");
                }
                return CommandResult::Success({
                    {"updated", true},
                    {"id",      updated.Id},
                    {"name",    updated.Name},
                    {"jql",     updated.Jql},
                });
            });
        };
        reg.Register(std::move(c));
    }

    // ---- view.delete -----------------------------------------------------
    // Delete a view by id. Internally we activate it first (since Views only
    // exposes DeleteActive). Refuses to delete the last remaining view.
    {
        Command c;
        c.Name = "view.delete"; c.Category = "view";
        c.Summary = "Delete a view by id (refuses if it would leave zero views).";
        c.Destructive = true;   // requires --yes
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Params = {
            []{ ParamSpec p; p.Name="id"; p.Type=ParamType::String; p.Required=true;
                p.Description="View id to delete."; return p; }(),
        };
        // view.delete reads + mutates Slice_; runs on UI thread.
        c.Handler = [&app, &views](const nlohmann::json& args, const CommandContext& ctx) {
            const bool dryRun = ctx.DryRun;
            return RunOnUiThreadAsCommandResult(app, [&views, args, dryRun]() {
                const std::string id = args.value("id", std::string());
                const ViewsStore& store = views.GetStore();
                const ViewDefinition* target = nullptr;
                for (const ViewDefinition& v : store.Views) {
                    if (v.Id == id) { target = &v; break; }
                }
                if (!target) {
                    return CommandResult::Failure(ErrorCode::NotFound,
                        "View '" + id + "' not found.");
                }
                if (store.Views.size() <= 1) {
                    return CommandResult::Failure(ErrorCode::HandlerError,
                        "Cannot delete the last remaining view.");
                }

                if (dryRun) {
                    return CommandResult::Success({{"wouldDo",
                        {{"id", target->Id}, {"name", target->Name}, {"jql", target->Jql}}}});
                }

                // Activate the target first (Views only exposes DeleteActive).
                if (!views.Activate(id)) {
                    return CommandResult::Failure(ErrorCode::HandlerError,
                        "Could not activate view '" + id + "' for deletion.");
                }
                if (!views.DeleteActive()) {
                    return CommandResult::Failure(ErrorCode::HandlerError,
                        "Views::DeleteActive() failed.");
                }
                return CommandResult::Success({{"deleted", id}});
            });
        };
        reg.Register(std::move(c));
    }
}

}  // namespace cmd
}  // namespace smatchet
