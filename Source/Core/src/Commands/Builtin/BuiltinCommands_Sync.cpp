// sync.* — incremental / full sync, refresh, status, stats, and inline-fetch.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/IMainThreadPoster.h"
#include "Commands/MainThreadDispatch.h" // RunOnUiThreadAsCommandResult

#include "Interfaces/IAppSync.h" // fan-in: sync.* depends on the narrow IAppSync facet, not the full AppController.h.
#include "Sync/SyncMode.h"
#include "Sync/SyncSessionStats.h"
#include "Sync/SyncTypes.h"    // TrackerIssueFetchPack
#include "CachedTicketTypes.h" // CachedTicket (pack.Tickets element)
#include <nlohmann/json.hpp>   // this sync command TU uses nlohmann::json directly.

#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;

namespace {

void RegisterSyncIncrementalCommand(CommandRegistry& reg, IAppSync& app, IMainThreadPoster& poster) {
    {
        Command c = MakeCommand("sync.incremental",
                                "Re-sync the active view from the tracker, downloading only the tickets that "
                                "changed since they were last loaded (asynchronous).",
                                [&app, &poster](const nlohmann::json&, const CommandContext& ctx) {
                                    if (ctx.DryRun) {
                                        return CommandResult::Success({{"wouldDo", "re-sync the active view"}});
                                    }
                                    // SyncWithBackend reads UI-owned state (views, the focused pane's sync
                                    // service); hop so an MCP/CLI caller cannot race the frame loop.
                                    return RunOnUiThreadAsCommandResult(poster, [&app]() {
                                        app.SyncWithBackend();
                                        nlohmann::json out;
                                        out["triggered"] = true;
                                        return CommandResult::Success(std::move(out));
                                    });
                                });
        c.Destructive = false;
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }
}

void RegisterSyncFullCommand(CommandRegistry& reg, IAppSync& app, IMainThreadPoster& poster) {
    {
        Command c = MakeCommand("sync.full",
                                "Re-download the whole active view from the tracker (asynchronous). Keeps the "
                                "local cache and the offline queue; use cache.recreate to wipe the cache.",
                                [&app, &poster](const nlohmann::json&, const CommandContext& ctx) {
                                    if (ctx.DryRun) {
                                        return CommandResult::Success(
                                            {{"wouldDo", "re-download the whole active view (cache kept)"}});
                                    }
                                    return RunOnUiThreadAsCommandResult(poster, [&app]() {
                                        app.SyncWithBackendMode(nullptr, nullptr, smatchet::sync::SyncMode::Full);
                                        nlohmann::json out;
                                        out["triggered"] = true;
                                        return CommandResult::Success(std::move(out));
                                    });
                                });
        c.Destructive = false; // it no longer wipes anything
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }
}

void RegisterCacheRecreateCommand(CommandRegistry& reg, IAppSync& app, IMainThreadPoster& poster) {
    {
        Command c = MakeCommand("cache.recreate",
                                "Wipe the local cache database (cached tickets AND queued offline edits) and "
                                "re-fetch the active view from the tracker. Runs on the UI thread and waits for "
                                "every pane's in-flight sync to stop, so the UI may pause briefly.",
                                [&app, &poster](const nlohmann::json&, const CommandContext& ctx) {
                                    if (ctx.DryRun) {
                                        return CommandResult::Success(
                                            {{"wouldDo", "wipe local cache + offline queue, then full re-fetch"}});
                                    }
                                    // RecreateLocalCacheDatabase swaps the cache and joins every pane's
                                    // worker: UI-thread-owned state, so the whole sequence runs there.
                                    return RunOnUiThreadAsCommandResult(poster, [&app]() {
                                        const VoidResult recreated = app.RecreateLocalCacheDatabase();
                                        app.SyncWithBackendMode(nullptr, nullptr, smatchet::sync::SyncMode::Full);
                                        nlohmann::json out;
                                        out["triggered"] = true;
                                        if (!recreated.has_value())
                                            out["warning"] = recreated.error();
                                        return CommandResult::Success(std::move(out));
                                    });
                                });
        c.Destructive = true; // deletes queued offline edits: relies on the registry's confirm gate
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        reg.Register(std::move(c));
    }
}

void RegisterSyncRefreshLocalCommand(CommandRegistry& reg, IAppSync& app, IMainThreadPoster& poster) {
    {
        Command c =
            MakeCommand("sync.refresh_local",
                        "Rebuild in-memory ticket list from the local SQLite cache (no network). Fire-and-forget: "
                        "returns once the read is started; the grid updates when it lands.",
                        [&app, &poster](const nlohmann::json&, const CommandContext&) {
                            return RunOnUiThreadAsCommandResult(poster, [&app]() {
                                // Resolves the focused pane here; the SQLite read runs on a worker.
                                app.RefreshLocalDataAsync();
                                return CommandResult::Success({{"triggered", true}});
                            });
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

void RegisterSyncFetchActiveViewCommand(CommandRegistry& reg, IAppSync& app, IMainThreadPoster& poster) {
    {
        Command c =
            MakeCommand("sync.fetch_active_view",
                        "Fetch tickets for the active view from the tracker without caching (returns inline JSON).",
                        [&app, &poster](const nlohmann::json&, const CommandContext&) {
                            // The fetch blocks on network I/O for as long as the view is large; on the UI
                            // thread (the palette) that would freeze every frame (Pillar 2). CLI/MCP run
                            // it on their own thread.
                            if (poster.IsOnUiThread()) {
                                return CommandResult::Failure(
                                    ErrorCode::HandlerError,
                                    "sync.fetch_active_view blocks on network I/O and must not run on the UI thread.",
                                    "Use sync.incremental from the palette, or call this over the CLI / MCP.");
                            }
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

nlohmann::json SyncStatsToJson(const smatchet::sync::SyncSessionStats& stats) {
    nlohmann::json out;
    out["valid"] = stats.Valid;
    out["requestId"] = stats.RequestId;
    out["mode"] = stats.Mode;
    out["rosterSize"] = stats.RosterSize;
    out["bodiesFetched"] = stats.BodiesFetched;
    out["unchanged"] = stats.Unchanged;
    out["requests"] = stats.Requests;
    out["downloadBytes"] = stats.DownloadBytes;
    out["uploadBytes"] = stats.UploadBytes;
    out["durationMs"] = stats.DurationMs;
    out["fallbackReason"] = stats.FallbackReason;
    out["completed"] = stats.Completed;
    out["error"] = stats.Error;
    return out;
}

void RegisterSyncStatsCommand(CommandRegistry& reg, IAppSync& app, IMainThreadPoster& poster) {
    Command c = MakeCommand("sync.stats",
                            "What the last finished sync session cost: mode (full/delta), roster size, bodies "
                            "fetched, unchanged rows, requests, bytes, duration and fallback reason.",
                            [&app, &poster](const nlohmann::json&, const CommandContext&) {
                                // Resolves the focused pane, which is UI-thread-owned state.
                                return RunOnUiThreadAsCommandResult(poster, [&app]() {
                                    return CommandResult::Success(SyncStatsToJson(app.GetLastSyncSessionStats()));
                                });
                            });
    c.Idempotent = true;
    reg.Register(std::move(c));
}

} // namespace

void RegisterSyncCommands(CommandRegistry& reg, IAppSync& app, IMainThreadPoster& poster) {
    RegisterSyncIncrementalCommand(reg, app, poster);
    RegisterSyncFullCommand(reg, app, poster);
    RegisterCacheRecreateCommand(reg, app, poster);
    RegisterSyncRefreshLocalCommand(reg, app, poster);
    RegisterSyncTrackerStatusCommand(reg, app);
    RegisterSyncFetchActiveViewCommand(reg, app, poster);
    RegisterSyncStatsCommand(reg, app, poster);
}

} // namespace cmd
} // namespace smatchet
