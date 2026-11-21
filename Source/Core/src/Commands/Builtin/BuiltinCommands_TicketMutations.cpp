// ticket.* — ticket mutations (set_field, set_fields, set_field_bulk, add_comment,
// add_worklog, transition, create).

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/IAppThreading.h"

// fan-in Phase 5: depend on the narrow IAppTicketMutations facet, not the full AppController.h.
// The facet forward-declares the rank-3 Tracker payload types; this TU includes their full
// definitions (below) since it constructs/derefs them and reads IssueCreateResult from the future.
#include "Interfaces/IAppTicketMutations.h"
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.
#include "IssueDraft.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "IssueCreatePipeline.h" // IssueCreateResult (returned by CreateIssueAsync().get())
#include "TrackerFieldSchema.h"
#include "Tracker/TrackerFieldValueUtils.h"
#include "Ui/BulkFieldEditPure.h"
#include "Ui/SmatchetToast.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

// ticket.set_field_bulk — one field, one value, N tickets (docs/plans/bulk-edit-rows.md). Each ticket is its own
// SubmitFieldEditOrQueue, so every row is saved or queued offline and audited exactly like ticket.set_field.
constexpr std::size_t kMaxBulkTickets = 500;

namespace bulk = smatchet::grid::bulk;

struct BulkCommandArgs {
    std::vector<std::string> Ids;
    std::string FieldId;
    std::vector<std::string> Picked;
    std::string ModeName = "replace";
    bulk::BulkMode Mode = bulk::BulkMode::Replace;
};

struct BulkCommandRow {
    std::string Id;
    std::vector<std::string> Values; ///< what this ticket gets written
    std::string From;                ///< the cached display value before the edit ("" when not cached)
    std::string Skip;                ///< non-empty: not sent, and why
};

// Reads and validates the arguments. Returns "" on success, else the message for a validation error.
std::string ParseBulkArgs(const nlohmann::json& args, BulkCommandArgs& out) {
    const nlohmann::json idsJson = args.value("ids", nlohmann::json::array());
    if (!idsJson.is_array() || idsJson.empty()) {
        return "'ids' must be a non-empty JSON array of ticket ids.";
    }
    if (idsJson.size() > kMaxBulkTickets) {
        return "'ids' holds more than " + std::to_string(kMaxBulkTickets) + " tickets; split the call.";
    }
    for (const nlohmann::json& v : idsJson) {
        if (!v.is_string() || v.get<std::string>().empty()) {
            return "Every entry of 'ids' must be a non-empty string.";
        }
        if (std::find(out.Ids.begin(), out.Ids.end(), v.get<std::string>()) == out.Ids.end()) {
            out.Ids.push_back(v.get<std::string>());
        }
    }
    out.FieldId = args.value("field", std::string());
    const nlohmann::json valuesJson = args.value("values", nlohmann::json());
    if (valuesJson.is_array()) {
        for (const nlohmann::json& v : valuesJson) {
            out.Picked.push_back(v.is_string() ? v.get<std::string>() : v.dump());
        }
    } else if (args.contains("value")) {
        out.Picked.push_back(args.value("value", std::string()));
    } else {
        return "Pass 'value' or a 'values' array.";
    }
    out.ModeName = args.value("mode", std::string("replace"));
    if (out.ModeName == "add") {
        out.Mode = bulk::BulkMode::Add;
    } else if (out.ModeName == "remove") {
        out.Mode = bulk::BulkMode::Remove;
    } else if (out.ModeName != "replace") {
        return "'mode' must be replace, add or remove.";
    }
    return std::string();
}

// Plans every row from the cached tickets (memory only): the before-value for the diff and, for add / remove, the
// merged set this ticket gets. A row whose current value cannot be merged is skipped with its reason.
std::vector<BulkCommandRow> PlanBulkRows(IAppTicketMutations& app, const TrackerField& field,
                                         const BulkCommandArgs& a) {
    const bulk::BulkFieldKind kind = bulk::ClassifyBulkField(field);
    // SMATCHET_DEVIATION(rule=duplication; reason=command plans via IAppTicketMutations while the grid plans via AppController and the pane target; owner=grid-engine; revisit=2027-05-15)
    const std::shared_ptr<const std::vector<CachedTicket>> snap = app.GetActiveTicketsSnapshot();
    std::unordered_map<std::string, const CachedTicket*> byId;
    if (snap) {
        byId.reserve(snap->size());
        for (const CachedTicket& t : *snap) {
            byId.emplace(t.id, &t);
        }
    }
    std::vector<std::string> pickedIds;
    for (const std::string& p : a.Picked) {
        pickedIds.push_back(kind == bulk::BulkFieldKind::Labels ? p
                                                                : TrackerFieldValueUtils::ResolveOptionId(field, p));
    }
    std::vector<BulkCommandRow> rows;
    rows.reserve(a.Ids.size());
    for (const std::string& id : a.Ids) {
        BulkCommandRow row;
        row.Id = id;
        const auto it = byId.find(id);
        const CachedTicket* ticket = it != byId.end() ? it->second : nullptr;
        row.From = ticket ? ticket->GetFieldValue(a.FieldId) : std::string();
        if (a.Mode == bulk::BulkMode::Replace) {
            row.Values = a.Picked;
        } else if (!ticket) {
            row.Skip = "not in the cached tickets, so add / remove has no current value to merge with";
        } else {
            const std::vector<std::string> before = bulk::BeforeValuesFor(field, kind, row.From);
            if (kind == bulk::BulkFieldKind::MultiChoice && !bulk::AllEntriesResolve(field, before)) {
                row.Skip = "current value is not in the option list, so it cannot be merged";
            } else {
                row.Values = bulk::NewValuesForRow(kind, a.Mode, before, pickedIds);
            }
        }
        // Writing what the ticket already holds is N tracker updates, audit entries and notifications for nothing.
        if (row.Skip.empty() && ticket && kind != bulk::BulkFieldKind::Unsupported) {
            const std::vector<std::string> before = bulk::BeforeValuesFor(field, kind, row.From);
            if (bulk::IsRowUnchanged(kind, before, bulk::NewValuesForRow(kind, a.Mode, before, pickedIds))) {
                row.Skip = "already has this value";
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

nlohmann::json BulkDryRunData(const BulkCommandArgs& a, const std::vector<BulkCommandRow>& rows) {
    nlohmann::json would = nlohmann::json::array();
    for (const BulkCommandRow& row : rows) {
        nlohmann::json entry = {{"id", row.Id}, {"from", row.From}, {"to", row.Values}};
        if (!row.Skip.empty()) {
            entry["skip"] = row.Skip;
        }
        would.push_back(std::move(entry));
    }
    return {{"wouldDo", {{"field", a.FieldId}, {"mode", a.ModeName}, {"rows", would}}}};
}

struct BulkRunTotals {
    nlohmann::json Rows = nlohmann::json::array();
    bulk::BulkSummary Summary;
};

BulkRunTotals RunBulkRows(IAppTicketMutations& app, const PendingActionTarget& target, const TrackerField& field,
                          const std::vector<BulkCommandRow>& rows) {
    BulkRunTotals t;
    for (const BulkCommandRow& row : rows) {
        nlohmann::json entry = {{"id", row.Id}};
        const bool closing = app.IsShuttingDown(); // quitting joins this worker: stop instead of N more round-trips
        if (!row.Skip.empty() || closing) {
            entry["state"] = "skipped";
            entry["message"] = closing ? std::string("the app is closing") : row.Skip;
            ++t.Summary.Skipped;
        } else {
            // Read now, not at dispatch: the tracker may have dropped mid-run, and a stale "online" would make every
            // remaining row wait out the HTTP retry window before it queues (the grid pump does the same per send).
            PendingActionTarget rowTarget = target;
            rowTarget.Connectivity = app.GetLastTrackerConnectivityState();
            const PendingActionSubmitResult r = app.SubmitFieldEditOrQueue(rowTarget, row.Id, field, row.Values);
            if (r.K == PendingActionSubmitResult::Kind::Failed) {
                entry["state"] = "failed";
                entry["message"] = PendingActionError(r);
                ++t.Summary.Failed;
            } else if (r.K == PendingActionSubmitResult::Kind::Queued) {
                entry["state"] = "queued";
                entry["offlineId"] = r.QueueId;
                ++t.Summary.Queued;
            } else {
                entry["state"] = "saved";
                ++t.Summary.Saved;
            }
        }
        t.Rows.push_back(std::move(entry));
    }
    LOG_INFO("ticket.set_field_bulk: field=%s saved=%d queued=%d failed=%d skipped=%d", field.Id.c_str(),
             t.Summary.Saved, t.Summary.Queued, t.Summary.Failed, t.Summary.Skipped);
    return t;
}

nlohmann::json BulkResultData(const BulkRunTotals& t) {
    return {{"rows", t.Rows},
            {"saved", t.Summary.Saved},
            {"queued", t.Summary.Queued},
            {"failed", t.Summary.Failed},
            {"skipped", t.Summary.Skipped}};
}

// The background run has no caller to return the per-row result to, so the totals reach the user as one toast.
void PostBulkTotalsToast(IMainThreadPoster& poster, const TrackerField& field, const bulk::BulkSummary& summary) {
    const std::string name = field.Name.empty() ? field.Id : field.Name;
    const std::string title = SmatchetLocalization::Format("grid.bulk.done_title", "Bulk edit: %s", name.c_str());
    const std::string message = bulk::BulkSummaryText(summary);
    const ToastType type = bulk::BulkSummaryToastType(summary);
    poster.PostCompletionToMainThread(
        [title, message, type]() { SmatchetToastManager::Instance().Push(title, message, type, 6000); });
}

// Checks the `mode` / `values` pair against the field before any row is planned: add / remove needs a multi-value
// field and at least one value, and every picked option of a closed list must exist. Empty string = valid.
std::string ValidateBulkMode(const TrackerField& field, const BulkCommandArgs& a) {
    if (a.Mode == bulk::BulkMode::Replace) {
        return std::string();
    }
    const bulk::BulkFieldKind kind = bulk::ClassifyBulkField(field);
    if (!bulk::IsMultiValueKind(kind)) {
        return "'mode' add / remove needs a multi-value field (labels, components, multi-select); '" + a.FieldId +
               "' holds one value.";
    }
    if (a.Picked.empty()) {
        return "'mode' add / remove needs at least one value in 'values'.";
    }
    if (kind == bulk::BulkFieldKind::MultiChoice) {
        std::vector<std::string> pickedIds;
        for (const std::string& p : a.Picked) {
            pickedIds.push_back(TrackerFieldValueUtils::ResolveOptionId(field, p));
        }
        if (!bulk::AllEntriesResolve(field, pickedIds)) {
            return "A value of 'values' is not in the option list of '" + a.FieldId +
                   "'. Check the spelling, or load the field's options first.";
        }
    }
    return std::string();
}

static void RegisterSetFieldBulkCommand(CommandRegistry& reg, IAppTicketMutations& app) {
    Command c = MakeCommand(
        "ticket.set_field_bulk", "Set one field to the same value on several tickets.",
        [&app](const nlohmann::json& args, const CommandContext& ctx) {
            BulkCommandArgs a;
            const std::string argError = ParseBulkArgs(args, a);
            if (!argError.empty()) {
                return CommandResult::Failure(ErrorCode::ValidationError, argError);
            }
            const TrackerField* fieldMeta = app.FindFieldById(a.FieldId);
            if (!fieldMeta) {
                return CommandResult::Failure(ErrorCode::NotFound, "Field '" + a.FieldId + "' not found in catalog.",
                                              "Run fields.refresh_catalog first.");
            }
            const std::string modeError = ValidateBulkMode(*fieldMeta, a);
            if (!modeError.empty()) {
                return CommandResult::Failure(ErrorCode::ValidationError, modeError);
            }
            // One latch for the whole call, taken right before planning (which reads the active tickets): every row
            // is written to that pane's tracker even if focus moves while a background run is in flight.
            const PendingActionTarget target = app.LatchPendingActionTarget();
            const std::vector<BulkCommandRow> rows = PlanBulkRows(app, *fieldMeta, a);
            if (ctx.DryRun) {
                return CommandResult::Success(BulkDryRunData(a, rows));
            }
            if (!ctx.Threading) {
                // A context without the threading facet cannot say which thread it is on, and N tracker round-trips
                // on the UI thread would freeze the frame (Pillar 2): refuse instead of guessing.
                return CommandResult::Failure(
                    ErrorCode::HandlerError,
                    "ticket.set_field_bulk needs a threading context (palette, CLI, MCP or Lua).");
            }
            // N tracker round-trips must never run on the UI thread (Pillar 2). The palette dispatches there, and so
            // does a Lua ticket-action script, so the choice is the calling THREAD, not the command source: on the UI
            // thread run the rows on a tracked worker and return at once (the totals come back as a toast); anywhere
            // else the caller already waits on its own thread. The one-shot in-process CLI is "on the UI thread" with
            // no frame loop: it shuts the app down the moment the command returns, so it must run the rows inline.
            if (ctx.Threading->IsOnUiThread() && ctx.Threading->HasFrameLoop()) {
                const TrackerField field = *fieldMeta;
                IAppTicketMutations* appPtr = &app;
                IAppThreading* threading = ctx.Threading;
                threading->LaunchBackgroundTask([appPtr, threading, target, field, rows]() {
                    PostBulkTotalsToast(*threading, field, RunBulkRows(*appPtr, target, field, rows).Summary);
                });
                return CommandResult::Success(
                    {{"started", true}, {"rowCount", static_cast<int>(rows.size())}, {"field", a.FieldId}});
            }
            return CommandResult::Success(BulkResultData(RunBulkRows(app, target, *fieldMeta, rows)));
        });
    c.Destructive = true;
    c.Idempotent = false;
    c.DryRunSupported = true;
    c.Description =
        "Writes `field` on every ticket in `ids`, one tracker update per ticket (each is saved, or queued offline, "
        "and audited like ticket.set_field). `mode` add / remove merges `values` into the current set of a "
        "multi-value field (labels, components, multi-select); replace (default) writes `value` / `values` as is. "
        "Returns {rows:[{id, state: saved|queued|failed|skipped, message?, offlineId?}], saved, queued, failed, "
        "skipped}; when dispatched on the UI thread (the command palette, a Lua ticket action) it returns "
        "{started:true, rowCount:N} at once, saves in the background and shows the totals as a toast. "
        "--dry-run returns {wouldDo:{rows:[{id, from, to, skip?}]}}. Example: "
        "ticket.set_field_bulk --ids='[\"PROJ-1\",\"PROJ-2\"]' --field=assignee --value=<accountId> --yes";
    c.Params = {
        {[] {
            ParamSpec p;
            p.Name = "ids";
            p.Type = ParamType::Json;
            p.Required = true;
            p.Description = "JSON array of ticket ids, e.g. [\"PROJ-1\",\"PROJ-2\"] (at most 500).";
            return p;
        }()},
        PString("field", "Field id (e.g. 'assignee', 'priority', 'labels').", true),
        PString("value", "New value (raw string, as ticket.set_field takes it). Use 'values' for several."),
        {[] {
            ParamSpec p;
            p.Name = "values";
            p.Type = ParamType::Json;
            p.Description = "JSON array of values for a multi-value field, e.g. [\"ui\",\"backend\"].";
            return p;
        }()},
        {[] {
            ParamSpec p;
            p.Name = "mode";
            p.Type = ParamType::String;
            p.Default = std::make_shared<nlohmann::json>("replace");
            p.Enum = {"replace", "add", "remove"};
            p.Description = "replace (default), or add / remove for a multi-value field.";
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
                std::string queueRefusal;
                const std::int64_t qid = app.QueueCreateOffline(draft, queueRefusal);
                if (qid <= 0) {
                    return CommandResult::Failure(ErrorCode::HandlerError,
                                                  queueRefusal.empty() ? std::string("Failed to queue offline create.")
                                                                       : queueRefusal);
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
    RegisterSetFieldBulkCommand(reg, app);
    RegisterCreateCommand(reg, app);
}

} // namespace cmd
} // namespace smatchet
