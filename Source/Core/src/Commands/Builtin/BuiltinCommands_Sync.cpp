// sync.* — incremental / full sync, refresh, status, and inline-fetch.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "Interfaces/IAppSync.h" // fan-in: sync.* depends on the narrow IAppSync facet, not the full AppController.h.
#include "Sync/SyncTypes.h"      // TrackerIssueFetchPack
#include "CachedTicketTypes.h"   // CachedTicket (pack.Tickets element)
#include <nlohmann/json.hpp>     // this sync command TU uses nlohmann::json directly.

#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;

namespace {

void RegisterSyncIncrementalCommand(CommandRegistry& reg, IAppSync& app) {
    {
        Command c = MakeCommand("sync.incremental", "Delta sync from tracker (last-fetched timestamp onward).",
                                [&app](const nlohmann::json&, const CommandContext& ctx) {
                                    if (ctx.DryRun) {
                                        nlohmann::json out;
                                        out["wouldDo"] = "incremental sync from last-fetched cursor";
                                        return CommandResult::Success(
                                            {{"wouldDo", "incremental sync from last-fetched cursor"}});
                                    }
                                    app.SyncWithBackend();
                                    nlohmann::json out;
                                    out["triggered"] = true;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Destructive = false;
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }
}

void RegisterSyncFullCommand(CommandRegistry& reg, IAppSync& app) {
    {
        Command c =
            MakeCommand("sync.full", "Full sync: wipe local cache and re-fetch all tickets from tracker.",
                        [&app](const nlohmann::json&, const CommandContext& ctx) {
                            if (ctx.DryRun) {
                                return CommandResult::Success({{"wouldDo", "wipe local cache + full re-fetch"}});
                            }
                            std::string err;
                            app.RecreateLocalCacheDatabase(err);
                            app.SyncWithBackend();
                            nlohmann::json out;
                            out["triggered"] = true;
                            if (!err.empty())
                                out["warning"] = err;
                            return CommandResult::Success(std::move(out));
                        });
        c.Destructive = true;
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }
}

void RegisterSyncRefreshLocalCommand(CommandRegistry& reg, IAppSync& app) {
    {
        Command c =
            MakeCommand("sync.refresh_local", "Rebuild in-memory ticket list from the local SQLite cache (no network).",
                        [&app](const nlohmann::json&, const CommandContext&) {
                            app.RefreshLocalData();
                            return CommandResult::Success({{"triggered", true}});
                        });
        reg.Register(std::move(c));
    }
}

void RegisterSyncTrackerStatusCommand(CommandRegistry& reg, IAppSync& app) {
    {
        Command c = MakeCommand("sync.tracker_status",
                                "Last connectivity state and diagnostic from the tracker reachability probe.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    using S = ::TrackerConnectivityState;
                                    const S s = app.GetLastTrackerConnectivityState();
                                    const char* stateStr = "unknown";
                                    switch (s) {
                                    case S::Unknown:
                                        stateStr = "unknown";
                                        break;
                                    case S::AuthenticatedReachable:
                                        stateStr = "authenticated-reachable";
                                        break;
                                    case S::ReachableAuthOrConfigError:
                                        stateStr = "reachable-auth-or-config-error";
                                        break;
                                    case S::TransportDown:
                                        stateStr = "transport-down";
                                        break;
                                    case S::ServiceUnavailable:
                                        stateStr = "service-unavailable";
                                        break;
                                    }
                                    nlohmann::json out;
                                    out["state"] = stateStr;
                                    out["syncWarning"] = app.GetLastTicketSyncWarning();
                                    out["fieldCatalogError"] = app.GetFieldCatalogError();
                                    return CommandResult::Success(std::move(out));
                                });
        reg.Register(std::move(c));
    }
}

void RegisterSyncFetchActiveViewCommand(CommandRegistry& reg, IAppSync& app) {
    {
        Command c =
            MakeCommand("sync.fetch_active_view",
                        "Fetch tickets for the active view from the tracker without caching (returns inline JSON).",
                        [&app](const nlohmann::json&, const CommandContext&) {
                            TrackerIssueFetchPack pack = app.FetchIssuesForActiveView();
                            nlohmann::json items = nlohmann::json::array();
                            for (const CachedTicket& t : pack.Tickets) {
                                nlohmann::json one;
                                one["id"] = t.id;
                                auto itS = t.fieldValues.find("summary");
                                if (itS != t.fieldValues.end())
                                    one["summary"] = itS->second;
                                items.push_back(std::move(one));
                            }
                            nlohmann::json out;
                            out["tickets"] = std::move(items);
                            out["fullSyncCompleted"] = pack.FullSyncCompleted;
                            if (!pack.FetchError.empty())
                                out["error"] = pack.FetchError;
                            if (!pack.Warning.empty())
                                out["warning"] = pack.Warning;
                            return CommandResult::Success(std::move(out));
                        });
        c.Idempotent = false;
        c.AsyncSafe = false;
        reg.Register(std::move(c));
    }
}

} // namespace

void RegisterSyncCommands(CommandRegistry& reg, IAppSync& app) {
    RegisterSyncIncrementalCommand(reg, app);
    RegisterSyncFullCommand(reg, app);
    RegisterSyncRefreshLocalCommand(reg, app);
    RegisterSyncTrackerStatusCommand(reg, app);
    RegisterSyncFetchActiveViewCommand(reg, app);
}

} // namespace cmd
} // namespace smatchet
