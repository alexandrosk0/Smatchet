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
#include <memory>
#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PString;

namespace {

// Success data of a field edit, comment or worklog: {"ok": true}, plus "queued": true and the queue row
// id as "offlineId" when the tracker was unreachable and the write was saved to send on reconnect
// (offline queues, Quality Pillar 6), and "needsReview": true when it is NOT resent on its own — a
// worklog whose send may have landed waits for the user to check the issue.
nlohmann::json PendingActionSuccessData(const PendingActionSubmitResult& result) {
    nlohmann::json data = {{"ok", true}};
    if (result.K == PendingActionSubmitResult::Kind::Queued) {
        data["queued"] = true;
        data["offlineId"] = result.QueueId;
        if (result.NeedsReview) {
            data["needsReview"] = true;
        }
    }
    return data;
}

std::string PendingActionError(const PendingActionSubmitResult& result) {
    return result.Error.empty() ? std::string("the tracker did not accept it.") : result.Error;
}

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
            const PendingActionTarget target = app.LatchPendingActionTarget();
            const PendingActionSubmitResult r = app.SubmitFieldEditOrQueue(target, id, *fieldMeta, {value});
            if (r.K == PendingActionSubmitResult::Kind::Failed) {
                return CommandResult::Failure(ErrorCode::BackendError, "Field edit failed: " + PendingActionError(r),
                                              "Check tracker connectivity.");
            }
            return CommandResult::Success(PendingActionSuccessData(r));
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
                                const PendingActionTarget target = app.LatchPendingActionTarget();
                                const PendingActionSubmitResult r = app.SubmitOrQueueComment(target, id, body);
                                if (r.K == PendingActionSubmitResult::Kind::Failed) {
                                    return CommandResult::Failure(ErrorCode::BackendError,
                                                                  "Comment failed: " + PendingActionError(r));
                                }
                                return CommandResult::Success(PendingActionSuccessData(r));
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
    Command c = MakeCommand(
        "ticket.add_worklog", "Log time worked on a ticket.",
        [&app](const nlohmann::json& args, const CommandContext&) {
            const int seconds = args.value("seconds", 0);
            // Reject a non-positive duration HERE, not at the tracker. The registry's
            // Required check only catches an omitted `seconds`; 0 (the value.value()
            // default, and what a caller sending `"seconds": 0` means) and a negative
            // both used to reach the tracker as timeSpent="0s" / "-30s" and come back
            // as an opaque backend rejection the caller cannot act on (#2054).
            if (seconds <= 0) {
                return CommandResult::Failure(ErrorCode::ValidationError,
                                              "Argument 'seconds' for 'ticket.add_worklog' must be > 0.",
                                              "Pass --seconds=<positive integer>.");
            }
            const std::string id = args.value("id", std::string());
            const std::string comment = args.value("comment", std::string());
            const std::string started = args.value("started", std::string());
            const std::string timeSpent = builtin_detail::FormatWorklogTimeSpent(seconds);
            const PendingActionTarget target = app.LatchPendingActionTarget();
            const PendingActionSubmitResult r =
                app.SubmitOrQueueWorklog(target, id, timeSpent, "", "auto", comment, started);
            if (r.K == PendingActionSubmitResult::Kind::Failed) {
                return CommandResult::Failure(ErrorCode::BackendError, "Worklog failed: " + PendingActionError(r));
            }
            nlohmann::json data = PendingActionSuccessData(r);
            data["timeSpent"] = timeSpent;
            return CommandResult::Success(std::move(data));
        });
    // Destructive-from-automation (CLI/MCP/Lua) audit logging is centralized in
    // CommandRegistry::Dispatch (snapshot.Destructive && IsAutomationSource) — handlers do not log
    // it per-command (that would double-log).
    c.Destructive = true;
    c.Idempotent = false;
    {
        ParamSpec ps;
        ps.Name = "seconds";
        ps.Type = ParamType::Int;
        ps.Required = true;
        ps.Description = "Time worked in seconds (e.g. 3600 = 1 hour).";
        // Declare the bound as well as checking it in the handler (#2054). ValidateAndResolveArgs
        // enforces MinInt centrally — which also publishes the bound in the command schema that
        // `--help` and the MCP tool list render, so a caller sees "minimum 1" instead of discovering
        // it by having the tracker reject a timeSpent of "0s". The handler guard stays as the net
        // for any path that resolves args without the validator (the BuiltinCommands_Debug
        // `lines` param sets the same belt-and-braces precedent).
        ps.MinInt = std::make_shared<long long>(1);
        c.Params.push_back(std::move(ps));
    }
    c.Params.push_back(PString("id", "Ticket id.", true));
    c.Params.push_back(PString("started", "ISO 8601 start timestamp (optional; uses now if empty)."));
    c.Params.push_back(PString("comment", "Worklog description."));
    reg.Register(std::move(c));
}

static void RegisterTransitionCommand(CommandRegistry& reg, IAppTicketMutations& app) {
    Command c = MakeCommand(
        "ticket.transition", "Transition a ticket to a new status.",
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
            const PendingActionTarget target = app.LatchPendingActionTarget();
            const PendingActionSubmitResult r = app.SubmitFieldEditOrQueue(target, id, *statusField, {toStatus});
            if (r.K == PendingActionSubmitResult::Kind::Failed) {
                return CommandResult::Failure(ErrorCode::BackendError, "Transition failed: " + PendingActionError(r));
            }
            return CommandResult::Success(PendingActionSuccessData(r));
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
                        // One latch for the whole call: every field goes to the same pane's tracker.
                        const PendingActionTarget target = app.LatchPendingActionTarget();
                        for (const auto& kv : fieldsMap.items()) {
                            const TrackerField* f = app.FindFieldById(kv.key());
                            if (!f) {
                                results[kv.key()]["ok"] = false;
                                results[kv.key()]["error"] = "field not found";
                                continue;
                            }
                            std::string val =
                                kv.value().is_string() ? kv.value().get<std::string>() : kv.value().dump();
                            const PendingActionSubmitResult r = app.SubmitFieldEditOrQueue(target, id, *f, {val});
                            const bool failed = r.K == PendingActionSubmitResult::Kind::Failed;
                            nlohmann::json entry = failed ? nlohmann::json{{"ok", false}} : PendingActionSuccessData(r);
                            entry["error"] = failed ? PendingActionError(r) : std::string();
                            results[kv.key()] = std::move(entry);
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
