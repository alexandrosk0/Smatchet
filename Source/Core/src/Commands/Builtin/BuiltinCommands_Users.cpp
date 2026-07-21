// users.* — search tracker users, list watchers/voters for an issue.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

// fan-in Phase 5: depend on the narrow IAppUsers facet, not the full AppController.h.
#include "Interfaces/IAppUsers.h"
#include "ITrackerCollaboration.h"      // TrackerIssueVotes — users.votes reads Result<TrackerIssueVotes>
#include "Tracker/TrackerFieldSchema.h" // TrackerUser (the facet only forward-declares it)
#include <nlohmann/json.hpp>            // this TU constructs nlohmann::json directly.

#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PaginateJsonArray;
using builtin_detail::PInt;
using builtin_detail::PString;

void RegisterUsersCommands(CommandRegistry& reg, IAppUsers& app) {
    {
        Command c = MakeCommand("users.search", "Search tracker users by display-name substring.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string query = args.value("query", std::string());
                                    const int limit = args.value("limit", 20);
                                    // DR27: previously the bool return was dropped, so a backend
                                    // failure surfaced as ok:true with an empty list. Propagate it.
                                    Result<std::vector<TrackerUser>> usersResult = app.SearchUsersByQuery(query);
                                    if (!usersResult.has_value()) {
                                        return CommandResult::Failure(ErrorCode::BackendError,
                                                                      "User search failed: " + usersResult.error());
                                    }
                                    const std::vector<TrackerUser>& users = usersResult.value();
                                    // SMATCHET_DEVIATION(rule=duplication; reason=builtin-command result boilerplate
                                    // (backend call + Failure envelope + JSON items array) is uniform across the
                                    // users/fields command TUs by design; a command-generic wrapper spanning
                                    // independent builtin TUs is not worth the coupling; owner=deep-review;
                                    // revisit=2026-10-01)
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
                Result<std::vector<TrackerUser>> watchersResult = app.FetchIssueWatchers(ticketId);
                if (!watchersResult.has_value()) {
                    return CommandResult::Failure(ErrorCode::BackendError,
                                                  "Watchers fetch failed: " + watchersResult.error());
                }
                const std::vector<TrackerUser>& watchers = watchersResult.value();
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
        Command c = MakeCommand("users.votes", "List voters (and vote count) for a ticket.",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string ticketId = args.value("ticketId", std::string());
                                    Result<TrackerIssueVotes> votesResult = app.FetchIssueVotes(ticketId);
                                    if (!votesResult.has_value()) {
                                        return CommandResult::Failure(ErrorCode::BackendError,
                                                                      "Votes fetch failed: " + votesResult.error());
                                    }
                                    const TrackerIssueVotes& votes = votesResult.value();
                                    nlohmann::json items = nlohmann::json::array();
                                    for (const TrackerUser& u : votes.Voters) {
                                        nlohmann::json one;
                                        one["id"] = u.AccountId;
                                        one["displayName"] = u.DisplayName;
                                        items.push_back(std::move(one));
                                    }
                                    nlohmann::json out;
                                    out["voteCount"] = votes.VoteCount;
                                    out["voters"] = std::move(items);
                                    return CommandResult::Success(std::move(out));
                                });
        c.Params = {PString("ticketId", "Ticket id.", true)};
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
