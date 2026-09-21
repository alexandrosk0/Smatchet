// view.* command group — registered from SmatchetUI once ViewState is ready.
// See docs/plans/shipped/command-system-plan.md §"Initial command catalogue – view".

#include "Commands/ViewCommands.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "ConfigManager.h"
#include "ViewColumnsPure.h"
#include "Views.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

// The include block + namespace-open + first-JSON-builder-function shape below matches
// PaneCommands.cpp's own opening token-for-token — every Commands/*.cpp command-group file
// follows this identical prologue by the registered idiom (see the "Before you edit" section
// of the Commands subsystem's own AGENTS.md).
// SMATCHET_DEVIATION(rule=duplication; reason=command-group-file prologue idiom; owner=orchestrator; revisit=if a shared prologue header is introduced)
namespace smatchet {
namespace cmd {

namespace {

// nlohmann::json field-by-field builder idiom (j["k"]=v repeated), shared with the
// serializers in ConfigManager_Save.cpp / ConfigManager_Views.cpp — the readable way to
// build a JSON object field-by-field; de-duplicating it needs a reflection layer this
// codebase doesn't have.
nlohmann::json ViewDefToJson(const ViewDefinition& v) {
    nlohmann::json j;
    // SMATCHET_DEVIATION(rule=duplication; reason=JSON field-builder idiom; owner=orchestrator; revisit=if a shared JSON-builder helper is introduced)
    j["id"]     = v.Id;
    j["name"]   = v.Name;
    j["jql"]    = v.Jql;
    // Fields stays for existing consumers (derived, same order as Columns' field entries).
    // Columns is the ordered source of truth: which columns exist, in what order, how wide.
    j["fields"] = v.Fields;
    j["columns"] = nlohmann::json::array();
    for (const auto& col : v.Columns) {
        j["columns"].push_back(nlohmann::json{{"key", col.Key}, {"width", col.Width}});
    }
    return j;
}

/// Read a JSON `columns` arg into an ordered key list. Accepts either a bare array of key
/// strings (`["id","field:summary"]`) or an array of `{"key":...}` objects (a `"width"` in
/// the object form is ignored here — width is set via view.set_column_width, not by naming
/// the column set). Non-string / malformed entries are skipped rather than rejected, matching
/// the existing `fields` param's tolerance.
std::vector<std::string> ParseColumnKeysArg(const nlohmann::json& columnsArg) {
    std::vector<std::string> keys;
    for (const auto& entry : columnsArg) {
        if (entry.is_string()) {
            keys.push_back(entry.get<std::string>());
        } else if (entry.is_object() && entry.contains("key") && entry["key"].is_string()) {
            keys.push_back(entry["key"].get<std::string>());
        }
    }
    return keys;
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

void RegisterViewListCommand(AppController& app, Views& views, CommandRegistry& reg) {
    Command c;
    c.Name = "view.list"; c.Category = "view";
    c.Summary = "List all configured ticket-grid views.";
    c.Params = {[]{ ParamSpec p; p.Name="limit"; p.Type=ParamType::Int; p.Default=std::make_shared<nlohmann::json>(50); return p; }(),
                []{ ParamSpec p; p.Name="offset"; p.Type=ParamType::Int; p.Default=std::make_shared<nlohmann::json>(0); return p; }()};
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

void RegisterViewGetCommand(AppController& app, Views& views, CommandRegistry& reg) {
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

void RegisterViewCurrentCommand(AppController& app, Views& views, CommandRegistry& reg) {
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

void RegisterViewActivateCommand(AppController& app, Views& views, CommandRegistry& reg) {
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

void RegisterViewRefreshActiveCommand(AppController& app, Views& views, CommandRegistry& reg) {
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

// Create a new view from a prototype. After Create() the new view becomes
// active automatically — the Views layer sets ActiveViewId to the new id, so
// the next sync will fetch tickets matching the new JQL.
void RegisterViewCreateCommand(AppController& app, Views& views, CommandRegistry& reg) {
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
            p.Description="JSON array of field ids to display (default: empty). Superseded by "
                          "'columns' when both are given."; return p; }(),
        []{ ParamSpec p; p.Name="columns"; p.Type=ParamType::Json;
            p.Description="Replacement ORDERED column list: a JSON array of key strings "
                          "(\"id\", \"field:<id>\") or {\"key\":...} objects. Takes precedence "
                          "over 'fields' when both are given — this is the full column set,"
                          " in display order; widths default and \"id\" is added automatically"
                          " if omitted."; return p; }(),
        []{ ParamSpec p; p.Name="triggerSync"; p.Type=ParamType::Bool; p.Default=std::make_shared<nlohmann::json>(false);
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
            const bool hasFields = args.contains("fields") && args["fields"].is_array();
            const bool hasColumns = args.contains("columns") && args["columns"].is_array();

            ViewDefinition proto;
            proto.Name = name;
            if (!jql.empty()) proto.Jql = jql;
            if (hasColumns) {
                for (const auto& key : ParseColumnKeysArg(args["columns"])) {
                    proto.Columns.push_back({key, 0.0f});
                }
            } else if (hasFields) {
                for (const auto& f : args["fields"]) {
                    if (f.is_string()) proto.Fields.push_back(f.get<std::string>());
                }
            }
            const bool bothGiven = hasFields && hasColumns;

            if (dryRun) {
                nlohmann::json wd;
                wd["name"] = proto.Name;
                wd["jql"]  = proto.Jql;
                if (hasColumns) {
                    wd["columns"] = args["columns"];
                } else {
                    wd["fields"] = proto.Fields;
                }
                nlohmann::json wdOut = {{"wouldDo", std::move(wd)}};
                if (bothGiven) {
                    wdOut["warning"] = "Both 'fields' and 'columns' were given; 'columns' takes precedence.";
                }
                return CommandResult::Success(std::move(wdOut));
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
            if (bothGiven) {
                out["warning"] = "Both 'fields' and 'columns' were given; 'columns' took precedence.";
            }
            if (triggerSync) {
                app.SyncWithBackend(nullptr, &views.GetStore());
                out["syncTriggered"] = true;
            }
            return CommandResult::Success(std::move(out));
        });
    };
    reg.Register(std::move(c));
}

// Update fields on the currently active view. Pass only the keys you want
// to change; omitted fields preserve their current value.
void RegisterViewUpdateCommand(AppController& app, Views& views, CommandRegistry& reg) {
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
            p.Description="Replacement JSON array of field ids. Superseded by 'columns' when "
                          "both are given; a field kept from the current set retains its "
                          "column position and width."; return p; }(),
        []{ ParamSpec p; p.Name="columns"; p.Type=ParamType::Json;
            p.Description="Replacement ORDERED column list: a JSON array of key strings "
                          "(\"id\", \"field:<id>\") or {\"key\":...} objects. Takes precedence"
                          " over 'fields' when both are given. To reorder or resize without "
                          "replacing the set, prefer view.set_column_order / "
                          "view.set_column_width."; return p; }(),
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
            const bool hasFields = args.contains("fields") && args["fields"].is_array();
            const bool hasColumns = args.contains("columns") && args["columns"].is_array();
            const bool bothGiven = hasFields && hasColumns;
            if (hasColumns) {
                std::vector<ViewColumn> replacement;
                for (const auto& key : ParseColumnKeysArg(args["columns"])) {
                    replacement.push_back({key, 0.0f});
                }
                updated.Columns = std::move(replacement);
            } else if (hasFields) {
                // A bare field-id replacement, not an ordered key list: preserve position and
                // width for any field that survives, by feeding the current Columns in as the
                // legacy column-order input. MigrateLegacyColumns appends anything newly added,
                // in the given order.
                std::vector<std::string> newFields;
                for (const auto& f : args["fields"]) {
                    if (f.is_string()) newFields.push_back(f.get<std::string>());
                }
                std::vector<std::string> currentOrder;
                std::unordered_map<std::string, float> currentWidths;
                for (const auto& col : updated.Columns) {
                    currentOrder.push_back(col.Key);
                    currentWidths[col.Key] = col.Width;
                }
                updated.Columns = MigrateLegacyColumns(newFields, currentOrder, currentWidths);
            }
            // NormalizeViewDefinition runs inside Views::Update — it regenerates Fields from
            // Columns, so `updated.Fields` need not (and must not) be set by hand here: doing
            // so would just be overwritten, and setting it without touching Columns is exactly
            // the drift bug this command surface exists to not reintroduce.

            if (dryRun) {
                nlohmann::json wd;
                wd["from"] = {{"name", active->Name}, {"jql", active->Jql}};
                wd["to"]   = {{"name", updated.Name}, {"jql", updated.Jql}};
                if (hasColumns || hasFields) {
                    nlohmann::json cols = nlohmann::json::array();
                    for (const auto& col : updated.Columns) {
                        cols.push_back(nlohmann::json{{"key", col.Key}, {"width", col.Width}});
                    }
                    wd["to"]["columns"] = std::move(cols);
                }
                nlohmann::json wdOut = {{"wouldDo", std::move(wd)}};
                if (bothGiven) {
                    wdOut["warning"] = "Both 'fields' and 'columns' were given; 'columns' takes precedence.";
                }
                return CommandResult::Success(std::move(wdOut));
            }

            if (!views.UpdateActive(updated)) {
                return CommandResult::Failure(ErrorCode::HandlerError,
                    "Views::UpdateActive() failed.");
            }
            const ViewDefinition* saved = views.GetActiveView();
            nlohmann::json out = {
                {"updated", true},
                {"id",      updated.Id},
                {"name",    updated.Name},
                {"jql",     updated.Jql},
                {"fields",  saved ? saved->Fields : updated.Fields},
            };
            if (bothGiven) {
                out["warning"] = "Both 'fields' and 'columns' were given; 'columns' took precedence.";
            }
            // The success-return-plus-register tail below is the fixed command-registration
            // shape every command handler in this subsystem ends with.
            // SMATCHET_DEVIATION(rule=duplication; reason=command-registration handler-tail idiom; owner=orchestrator; revisit=n/a, this is the contract)
            return CommandResult::Success(std::move(out));
        });
    };
    reg.Register(std::move(c));
}

// Reorder the active (or a named) view's columns, preserving each column's width. A key not
// naming an existing column is reported in `ignored` rather than silently dropped; a column
// not named in `order` keeps its relative position, appended after the ordered ones —
// reordering must never silently remove a column.
void RegisterViewSetColumnOrderCommand(AppController& app, Views& views, CommandRegistry& reg) {
    Command c;
    c.Name = "view.set_column_order"; c.Category = "view";
    c.Summary = "Reorder a view's columns without changing which columns exist or their widths.";
    c.Destructive = false;
    c.Idempotent = true;
    c.DryRunSupported = true;
    c.Params = {
        []{ ParamSpec p; p.Name="order"; p.Type=ParamType::Json; p.Required=true;
            p.Description="JSON array of column keys (\"id\", \"field:<id>\") in the desired "
                          "display order."; return p; }(),
        []{ ParamSpec p; p.Name="id"; p.Type=ParamType::String;
            p.Description="View id to reorder (default: the currently active view)."; return p; }(),
    };
    // view.set_column_order reads + writes Views::Slice_; UI thread only (same race as every
    // other view.* handler — see view.list above).
    c.Handler = [&app, &views](const nlohmann::json& args, const CommandContext& ctx) {
        const bool dryRun = ctx.DryRun;
        return RunOnUiThreadAsCommandResult(app, [&views, args, dryRun]() {
            const std::string id = args.value("id", std::string());
            const ViewDefinition* target = id.empty() ? views.GetActiveView() : views.Find(id);
            if (!target) {
                return CommandResult::Failure(ErrorCode::NotFound,
                    id.empty() ? "No active view." : ("View '" + id + "' not found."));
            }
            if (!args.contains("order") || !args["order"].is_array()) {
                return CommandResult::Failure(ErrorCode::ValidationError, "'order' must be a JSON array.");
            }
            std::vector<std::string> order;
            for (const auto& key : args["order"]) {
                if (key.is_string()) order.push_back(key.get<std::string>());
            }
            ViewDefinition updated = *target;
            const std::vector<std::string> ignored = ReorderViewColumns(order, updated);

            if (dryRun) {
                nlohmann::json cols = nlohmann::json::array();
                for (const auto& col : updated.Columns) {
                    cols.push_back(nlohmann::json{{"key", col.Key}, {"width", col.Width}});
                }
                return CommandResult::Success({{"wouldDo", {{"id", target->Id}, {"columns", std::move(cols)}}},
                                               {"ignored", ignored}});
            }
            if (!views.Update(target->Id, updated)) {
                return CommandResult::Failure(ErrorCode::HandlerError, "Views::Update() failed.");
            }
            return CommandResult::Success({{"updated", true}, {"id", target->Id}, {"ignored", ignored}});
        });
    };
    reg.Register(std::move(c));
}

// Set one column's width on the active (or a named) view. width <= 0 resets it to the kind
// default (DefaultColumnWidthPx) instead of storing an explicit value.
// The Command-field assignments and Params{[]{ParamSpec p; ...; return p;}()} lambda shape
// below are the Command struct's own fixed construction idiom (Commands/Command.h), reused
// identically by every Register*Command in this subsystem, PaneCommands.cpp's pane.focus included.
void RegisterViewSetColumnWidthCommand(AppController& app, Views& views, CommandRegistry& reg) {
    Command c;
    c.Name = "view.set_column_width"; c.Category = "view";
    c.Summary = "Set one column's width on a view (<=0 resets it to the default).";
    c.Destructive = false;
    c.Idempotent = true;
    c.DryRunSupported = true;
    c.Params = {
        []{ ParamSpec p; p.Name="key"; p.Type=ParamType::String; p.Required=true;
            p.Description="Column key (\"id\" or \"field:<id>\")."; return p; }(),
        // SMATCHET_DEVIATION(rule=duplication; reason=ParamSpec-lambda construction idiom; owner=orchestrator; revisit=n/a, this is the contract)
        []{ ParamSpec p; p.Name="width"; p.Type=ParamType::Number; p.Required=true;
            p.Description="Width in pixels. <=0 resets to the kind default."; return p; }(),
        []{ ParamSpec p; p.Name="id"; p.Type=ParamType::String;
            p.Description="View id to edit (default: the currently active view)."; return p; }(),
    };
    // view.set_column_width reads + writes Views::Slice_; UI thread only.
    c.Handler = [&app, &views](const nlohmann::json& args, const CommandContext& ctx) {
        const bool dryRun = ctx.DryRun;
        return RunOnUiThreadAsCommandResult(app, [&views, args, dryRun]() {
            const std::string id = args.value("id", std::string());
            const ViewDefinition* target = id.empty() ? views.GetActiveView() : views.Find(id);
            if (!target) {
                return CommandResult::Failure(ErrorCode::NotFound,
                    id.empty() ? "No active view." : ("View '" + id + "' not found."));
            }
            const std::string key = CanonicalGridColumnKey(args.value("key", std::string()));
            const float width = args.value("width", 0.0f);

            ViewDefinition updated = *target;
            auto it = std::find_if(updated.Columns.begin(), updated.Columns.end(),
                                   [&](const ViewColumn& c) { return c.Key == key; });
            if (it == updated.Columns.end()) {
                return CommandResult::Failure(ErrorCode::NotFound,
                    "Column '" + key + "' not found on view '" + target->Id + "'.");
            }
            it->Width = width > 0.0f ? width : 0.0f;
            NormalizeViewDefinition(updated);

            if (dryRun) {
                return CommandResult::Success({{"wouldDo", {{"id", target->Id}, {"key", key},
                                                            {"width", EffectiveColumnWidth(updated, key)}}}});
            }
            if (!views.Update(target->Id, updated)) {
                return CommandResult::Failure(ErrorCode::HandlerError, "Views::Update() failed.");
            }
            return CommandResult::Success(
                {{"updated", true}, {"id", target->Id}, {"key", key}, {"width", EffectiveColumnWidth(updated, key)}});
        });
    };
    reg.Register(std::move(c));
}

// Delete a view by id. Internally we activate it first (since Views only
// exposes DeleteActive). Refuses to delete the last remaining view.
void RegisterViewDeleteCommand(AppController& app, Views& views, CommandRegistry& reg) {
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

}  // namespace

void RegisterViewCommands(AppController& app, Views& views) {
    CommandRegistry& reg = app.Commands();
    // Idempotent guard — don't re-register on second call.
    if (reg.HasExact("view.list")) return;

    RegisterViewListCommand(app, views, reg);
    RegisterViewGetCommand(app, views, reg);
    RegisterViewCurrentCommand(app, views, reg);
    RegisterViewActivateCommand(app, views, reg);
    RegisterViewRefreshActiveCommand(app, views, reg);
    RegisterViewCreateCommand(app, views, reg);
    RegisterViewUpdateCommand(app, views, reg);
    RegisterViewDeleteCommand(app, views, reg);
    RegisterViewSetColumnOrderCommand(app, views, reg);
    RegisterViewSetColumnWidthCommand(app, views, reg);
}

}  // namespace cmd
}  // namespace smatchet
