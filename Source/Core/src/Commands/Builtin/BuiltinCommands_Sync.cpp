// sync.* — incremental / full sync, refresh, status, stats, and inline-fetch.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/IMainThreadPoster.h"
#include "Commands/MainThreadDispatch.h" // RunOnUiThreadAsCommandResult

#include "Interfaces/IAppSync.h" // fan-in: sync.* depends on the narrow IAppSync facet, not the full AppController.h.
#include "Sync/CacheRecreateReportPure.h"
#include "Sync/SyncMode.h"
#include "Sync/SyncSessionStats.h"
#include "Sync/SyncTypes.h"    // TrackerIssueFetchPack
#include "CachedTicketTypes.h" // CachedTicket (pack.Tickets element)
#include <nlohmann/json.hpp>   // this sync command TU uses nlohmann::json directly.

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
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

// The final result of a finished recreate: a failure says whether the cache files were already deleted, so a caller
// never believes queued offline edits survived a wipe that went through (or were cleared when it did not).
CommandResult CacheRecreateOutcomeToResult(const smatchet::sync::CacheRecreateOutcome& outcome) {
    if (!outcome.Ok) {
        return CommandResult::Failure(ErrorCode::HandlerError, smatchet::sync::DescribeCacheRecreateOutcome(outcome));
    }
    nlohmann::json out;
    out["triggered"] = true;
    out["completed"] = true;
    out["wiped"] = outcome.Wiped;
    return CommandResult::Success(std::move(out));
}

// The recreate's outcome arrives as a task posted to the UI thread, and this caller IS the UI thread with no frame
// loop to run it (the one-shot in-process CLI): pump the queue until it has run, for at most `bound`. A recreate that
// has not finished by then is abandoned when the process exits, so that is a failure, never a "started" success.
CommandResult WaitForRecreateByPumping(std::future<smatchet::sync::CacheRecreateOutcome>& finished,
                                       IMainThreadPoster& poster, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (finished.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return CommandResult::Failure(
                ErrorCode::HandlerError,
                "The cache recreate did not finish within " + std::to_string(bound.count() / 1000) +
                    " s. This process has no frame loop to finish it, so it is abandoned when the process exits "
                    "(the cache may or may not have been wiped).",
                "Raise --timeout, or run cache.recreate against the running application.");
        }
        poster.DrainMainThreadOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    try {
        return CacheRecreateOutcomeToResult(finished.get());
    } catch (const std::future_error&) {
        return CommandResult::Failure(ErrorCode::HandlerError,
                                      "The application shut down before the cache recreate finished.");
    }
}

// cache.recreate: the wipe runs on a worker behind AppController's "recreating" gate, so the UI thread never waits
// for a pane's in-flight request. Its outcome arrives on the UI thread; on success the recreate itself starts the
// follow-up full sync of the focused pane before this completion runs.
// A caller on another thread (MCP / CLI / Lua) waits for the outcome HERE, off the UI thread, and returns the final
// result including whether the wipe happened. A caller already on the UI thread with a frame loop (the command
// palette) cannot wait, so it gets a "started" result at once; the completion toast and the log report the outcome.
// A caller on the UI thread WITHOUT a frame loop (the one-shot in-process CLI) pumps the dispatcher itself until the
// outcome arrives (WaitForRecreateByPumping).
CommandResult RunCacheRecreate(IAppSync& app, IMainThreadPoster& poster, const CommandContext& ctx) {
    auto outcome = std::make_shared<std::promise<smatchet::sync::CacheRecreateOutcome>>();
    std::future<smatchet::sync::CacheRecreateOutcome> finished = outcome->get_future();
    const bool onUiThread = poster.IsOnUiThread();
    const CommandResult started = RunOnUiThreadAsCommandResult(poster, [&app, outcome]() {
        const VoidResult startedOk =
            app.StartRecreateLocalCacheDatabase([outcome](const smatchet::sync::CacheRecreateOutcome& done) {
                outcome->set_value(done); // the follow-up full sync (on success) is started by the recreate itself
            });
        if (!startedOk.has_value()) {
            smatchet::sync::CacheRecreateOutcome refused; // nothing was deleted
            refused.Error = startedOk.error();
            return CacheRecreateOutcomeToResult(refused);
        }
        return CommandResult::Success({{"started", true}});
    });
    if (!started.Ok) {
        return started;
    }
    nlohmann::json pending;
    pending["triggered"] = true;
    pending["started"] = true;
    pending["completed"] = false;
    const std::chrono::milliseconds bound =
        ctx.TimeoutMs > 0 ? std::chrono::milliseconds(ctx.TimeoutMs) : std::chrono::milliseconds(5 * 60 * 1000);
    if (onUiThread && !poster.HasFrameLoop()) {
        return WaitForRecreateByPumping(finished, poster, bound);
    }
    if (onUiThread) {
        return CommandResult::Success(std::move(pending));
    }
    if (finished.wait_for(bound) != std::future_status::ready) {
        return CommandResult::Success(std::move(pending)); // still running: it reports through the toast and the log
    }
    try {
        return CacheRecreateOutcomeToResult(finished.get());
    } catch (const std::future_error&) {
        return CommandResult::Failure(ErrorCode::HandlerError,
                                      "The application shut down before the cache recreate finished.");
    }
}

void RegisterCacheRecreateCommand(CommandRegistry& reg, IAppSync& app, IMainThreadPoster& poster) {
    {
        Command c = MakeCommand(
            "cache.recreate",
            "Wipe the local cache database (cached tickets AND queued offline edits) and re-fetch the active view "
            "from the tracker. The wipe runs on a worker behind a \"recreating\" gate that holds back new syncs and "
            "cache access; the UI never waits for a running request. MCP / CLI / Lua callers wait for the outcome "
            "and get the final result (a failure says whether the cache was already wiped); the command palette "
            "gets \"started\" at once and a toast reports the outcome.",
            [&app, &poster](const nlohmann::json&, const CommandContext& ctx) {
                if (ctx.DryRun) {
                    return CommandResult::Success(
                        {{"wouldDo", "wipe local cache + offline queue, then full re-fetch"}});
                }
                return RunCacheRecreate(app, poster, ctx);
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
    // SMATCHET_DEVIATION(rule=duplication; reason=one flat key-to-field copy in the repo's out["k"] = v style, structural like every other command serializer, and it must stay in step with SyncSessionStats; owner=command-system; revisit=2027-10-14)
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
    Command c =
        MakeCommand("sync.stats",
                    "What the last finished sync session cost: mode (full/delta), roster size, bodies "
                    "fetched, unchanged rows, requests, bytes, duration and fallback reason. The request and byte "
                    "counts are approximate: they sample a traffic counter shared by every pane, so two panes "
                    "syncing at once cross-count each other.",
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
