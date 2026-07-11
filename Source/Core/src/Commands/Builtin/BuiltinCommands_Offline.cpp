// offline.* — pending creates / field edits queue inspection + replay + prune.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

// fan-in Phase 5: depend on the narrow IAppOfflineQueue facet, not the full AppController.h
// (drops this TU off the AppController.h fan-in count). CachedTicketTypes.h provides the
// pending/dead record element types iterated below; Sync/OfflineQueueTypes.h completes the
// delete-summary return types (`.Deleted`) the facet forward-declares.
#include "Interfaces/IAppOfflineQueue.h"
#include "CachedTicketTypes.h"
#include "Sync/OfflineQueueTypes.h"
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PaginateJsonArray;
using builtin_detail::PInt;

void RegisterOfflineCommands(CommandRegistry& reg, IAppOfflineQueue& app) {
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
                std::vector<std::int64_t> cIds;
                std::vector<std::int64_t> eIds;
                cIds.reserve(deadCreates.size());
                eIds.reserve(deadEdits.size());
                std::transform(deadCreates.begin(), deadCreates.end(), std::back_inserter(cIds),
                               [](const DeadPendingCreate& d) { return d.DeadId; });
                std::transform(deadEdits.begin(), deadEdits.end(), std::back_inserter(eIds),
                               [](const DeadPendingFieldEdit& d) { return d.DeadId; });
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
}

} // namespace cmd
} // namespace smatchet
