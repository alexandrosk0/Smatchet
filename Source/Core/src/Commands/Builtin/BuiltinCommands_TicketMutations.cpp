// ticket.* — single-ticket mutations (set_field, set_fields, add_comment,
// add_worklog, transition, create).

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

// fan-in Phase 5: depend on the narrow IAppTicketMutations facet, not the full AppController.h.
// The facet forward-declares the rank-3 Tracker payload types; this TU includes their full
// definitions (below) since it constructs/derefs them and reads IssueCreateResult from the future.
#include "Interfaces/IAppTicketMutations.h"
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.
#include "IssueDraft.h"
#include "LocalCacheManager.h"
#include "IssueCreatePipeline.h" // IssueCreateResult (returned by CreateIssueAsync().get())
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PString;

namespace {

static void RegisterSetFieldCommand(CommandRegistry& reg, IAppTicketMutations& app) {
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
                    auto it =
                        std::find_if(snap->begin(), snap->end(), [&id](const CachedTicket& t) { return t.id == id; });
                    if (it != snap->end()) {
                        auto fit = it->fieldValues.find(field);
                        if (fit != it->fieldValues.end())
                            from = fit->second;
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
            const VoidResult r = app.SubmitFieldEdit(id, *fieldMeta, {value});
            if (!r.has_value()) {
                return CommandResult::Failure(ErrorCode::BackendError, "Field edit failed: " + r.error(),
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

static void RegisterAddCommentCommand(CommandRegistry& reg, IAppTicketMutations& app) {
    Command c = MakeCommand("ticket.add_comment", "Post a plain-text comment on a ticket.",
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

static void RegisterAddWorklogCommand(CommandRegistry& reg, IAppTicketMutations& app) {
    Command c = MakeCommand("ticket.add_worklog", "Log time worked on a ticket.",
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

static void RegisterTransitionCommand(CommandRegistry& reg, IAppTicketMutations& app) {
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
                        const VoidResult r = app.SubmitFieldEdit(id, *statusField, {toStatus});
                        if (!r.has_value()) {
                            return CommandResult::Failure(ErrorCode::BackendError, "Transition failed: " + r.error());
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

static void RegisterSetFieldsCommand(CommandRegistry& reg, IAppTicketMutations& app) {
    Command c = MakeCommand(
        "ticket.set_fields", "Update multiple fields on a ticket in one call.",
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
                std::string val = kv.value().is_string() ? kv.value().get<std::string>() : kv.value().dump();
                const VoidResult r = app.SubmitFieldEdit(id, *f, {val});
                results[kv.key()] = {{"ok", r.has_value()}, {"error", r.has_value() ? std::string() : r.error()}};
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

static void RegisterCreateCommand(CommandRegistry& reg, IAppTicketMutations& app) {
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
            p.Default = std::make_shared<nlohmann::json>(false);
            p.Description = "Queue offline rather than creating live.";
            return p;
        }()},
    };
    reg.Register(std::move(c));
}

} // namespace

void RegisterTicketMutationCommands(CommandRegistry& reg, IAppTicketMutations& app) {
    RegisterSetFieldCommand(reg, app);
    RegisterAddCommentCommand(reg, app);
    RegisterAddWorklogCommand(reg, app);
    RegisterTransitionCommand(reg, app);
    RegisterSetFieldsCommand(reg, app);
    RegisterCreateCommand(reg, app);
}

} // namespace cmd
} // namespace smatchet
