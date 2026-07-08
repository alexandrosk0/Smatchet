// perf.* — UI performance monitor snapshot, reset, dump, and panel toggle.
// debug.grid.edit-burst — headless cell-edit burst harness (docs/plans/shipped/grid-cell-edit-perf.md).

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "Commands/PathConfinement.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "CachedTicketTypes.h"
#include "ConfigManager.h"
#include "MemoryTelemetry.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetImageTextureCache.h"
#include "SmatchetUiSession.h"
#include "TrackerFieldSchema.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;

extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PInt;
using builtin_detail::PString;

namespace {

void RegisterPerfSnapshotCommand(CommandRegistry& reg) {
    Command c = MakeCommand("perf.snapshot", "Per-scope UI perf rows from the last drawn frame.",
                            [](const nlohmann::json&, const CommandContext&) {
                                std::vector<UiPerfRow> rows =
                                    UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
                                nlohmann::json arr = nlohmann::json::array();
                                for (const UiPerfRow& r : rows) {
                                    nlohmann::json one;
                                    one["name"] = r.name;
                                    one["lastTotalMs"] = r.lastTotalMs;
                                    one["avgPerCallMs"] = r.avgPerCallMs;
                                    one["maxMs"] = r.maxMs;
                                    one["calls"] = r.calls;
                                    one["lifetimeHits"] = r.lifetimeHits;
                                    one["emaAvgMs"] = r.emaAvgMs;
                                    one["p99Ms"] = r.p99Ms;
                                    arr.push_back(std::move(one));
                                }
                                nlohmann::json out;
                                out["rows"] = std::move(arr);
                                return CommandResult::Success(std::move(out));
                            });
    c.Description = "Returns array of UI perf rows ({name, lastTotalMs, avgPerCallMs, maxMs, calls, lifetimeHits, "
                    "emaAvgMs, p99Ms}).";
    reg.Register(std::move(c));
}

void RegisterPerfMemoryCommand(CommandRegistry& reg, AppController& app) {
    // perf.memory — pull-based memory-pressure snapshot (S3 of
    // docs/plans/shipped/memory-budget-and-lifetime-hardening.md). Each gauge reads its
    // source under that source's existing lock at snapshot time; no push wiring in hot paths.
    Command c = MakeCommand(
        "perf.memory", "Pull-based memory snapshot: RSS, dispatcher queue, icon-cache occupancy, tickets.",
        [&app](const nlohmann::json&, const CommandContext&) {
            smatchet::memtel::MemorySnapshot s;
            s.RssBytes = smatchet::memtel::ProcessRssBytes();
            s.DispatcherQueueLen = app.mainThreadDispatcher.QueueLen();
            s.DispatcherLastDrainTasks = app.mainThreadDispatcher.LastDrainTaskCount();
            s.DispatcherLastDrainDeferred = app.mainThreadDispatcher.LastDrainDeferredCount();
            s.IconCacheEntries = SmatchetImageTextureCache::IconCacheEntryCount();
            s.IconCacheApproxBytes = SmatchetImageTextureCache::IconCacheApproxBytes();
            if (auto tickets = app.GetActiveTicketsSnapshot()) {
                s.ActiveTicketCount = tickets->size();
            }
            s.PendingThumbnailUploads = smatchet::memtel::PendingThumbnailUploads().load(std::memory_order_relaxed);
            s.PlanCacheApproxBytes = smatchet::memtel::PlanCacheApproxBytes().load(std::memory_order_relaxed);
            nlohmann::json out;
            out["rssBytes"] = s.RssBytes;
            out["dispatcherQueueLen"] = s.DispatcherQueueLen;
            out["dispatcherLastDrainTasks"] = s.DispatcherLastDrainTasks;
            out["dispatcherLastDrainDeferred"] = s.DispatcherLastDrainDeferred;
            out["iconCacheEntries"] = s.IconCacheEntries;
            out["iconCacheApproxBytes"] = s.IconCacheApproxBytes;
            out["activeTicketCount"] = s.ActiveTicketCount;
            out["pendingThumbnailUploads"] = s.PendingThumbnailUploads;
            out["planCacheApproxBytes"] = s.PlanCacheApproxBytes;
            return CommandResult::Success(std::move(out));
        });
    c.Description = "Returns {rssBytes, dispatcherQueueLen, dispatcherLastDrainTasks, dispatcherLastDrainDeferred, "
                    "iconCacheEntries, iconCacheApproxBytes, activeTicketCount, pendingThumbnailUploads, "
                    "planCacheApproxBytes}. Gauges, not invariants.";
    reg.Register(std::move(c));
}

void RegisterPerfFrameCountCommand(CommandRegistry& reg) {
    Command c = MakeCommand("perf.frame_count", "Total count of profiled scope entries in the last frame.",
                            [](const nlohmann::json&, const CommandContext&) {
                                std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
                                nlohmann::json out;
                                out["scopeCount"] = rows.size();
                                const std::uint64_t totalCalls = std::accumulate(
                                    rows.begin(), rows.end(), static_cast<std::uint64_t>(0),
                                    [](std::uint64_t acc, const UiPerfRow& r) { return acc + r.calls; });
                                out["totalCalls"] = totalCalls;
                                return CommandResult::Success(std::move(out));
                            });
    reg.Register(std::move(c));
}

void RegisterPerfDumpCommand(CommandRegistry& reg) {
    Command c =
        MakeCommand("perf.dump", "Write perf snapshot to a JSON file and return {file, count}.",
                    [](const nlohmann::json& args, const CommandContext&) {
                        const std::string& userDataDir = ConfigManager::GetUserDataDirectory();
                        std::string outPath = args.value("outPath", std::string());
                        if (outPath.empty()) {
                            std::time_t t = std::time(nullptr);
                            char ts[64] = {};
                            if (const std::tm* lt = std::localtime(&t))
                                std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", lt);
                            outPath = (fs::path(userDataDir) / "perf" / (std::string("perf-snapshot-") + ts + ".json"))
                                          .string();
                        } else {
                            // SECURITY: a caller-supplied outPath is untrusted (CLI/Lua/MCP). Confine it
                            // under a dedicated <userData>/perf/ subdir — not the user-data root, which
                            // holds smatchet_config.json and other state — so perf.dump cannot write or
                            // clobber an arbitrary file.
                            std::string resolved, confineErr;
                            if (!smatchet::cmd::ConfinePathUnderSubdir(userDataDir, "perf", outPath, resolved,
                                                                       confineErr)) {
                                return CommandResult::Failure(ErrorCode::ValidationError,
                                                              "perf.dump outPath rejected: " + confineErr);
                            }
                            outPath = resolved;
                        }
                        std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
                        nlohmann::json doc = nlohmann::json::array();
                        std::transform(rows.begin(), rows.end(), std::back_inserter(doc), [](const UiPerfRow& r) {
                            return nlohmann::json{{"name", r.name},
                                                  {"lastTotalMs", r.lastTotalMs},
                                                  {"avgPerCallMs", r.avgPerCallMs},
                                                  {"maxMs", r.maxMs},
                                                  {"calls", r.calls},
                                                  {"emaAvgMs", r.emaAvgMs},
                                                  {"p99Ms", r.p99Ms}};
                        });
                        // Serialise before opening the file so a throw (bad_alloc, JSON error)
                        // cannot leak an open FILE* handle.
                        const std::string s = doc.dump(2);
                        std::error_code ec;
                        fs::path outFs(outPath);
                        fs::create_directories(outFs.parent_path(), ec);
                        std::FILE* f = std::fopen(outPath.c_str(), "wb");
                        if (!f) {
                            return CommandResult::Failure(ErrorCode::HandlerError,
                                                          "Could not write perf dump to '" + outPath + "'.");
                        }
                        std::fwrite(s.data(), 1, s.size(), f);
                        std::fclose(f);
                        nlohmann::json out;
                        out["file"] = outPath;
                        out["count"] = static_cast<int>(rows.size());
                        return CommandResult::Success(std::move(out));
                    });
    c.Params = {
        PString("outPath", "Output file path, confined under <userData>/perf/ (default: perf-snapshot-<ts>.json).")};
    reg.Register(std::move(c));
}

void RegisterPerfResetCommand(CommandRegistry& reg) {
    Command c = MakeCommand("perf.reset", "Clear all accumulated UI perf measurements (use before a benchmark run).",
                            [](const nlohmann::json&, const CommandContext&) {
                                UiPerfMonitor::Instance().Reset();
                                return CommandResult::Success({{"reset", true}});
                            });
    reg.Register(std::move(c));
}

void RegisterPerfTogglePanelCommand(CommandRegistry& reg, AppController& app) {
    Command c = MakeCommand("perf.toggle_panel", "Show or hide the Performance Monitor panel (persists to config).",
                            [&app](const nlohmann::json& args, const CommandContext& ctx) -> CommandResult {
                                const bool hasOpen = args.contains("open");
                                const bool wantOpen = args.value("open", false);
                                if (ctx.DryRun) {
                                    const bool current = ConfigManager::Load().ShowPerformanceWindow;
                                    const bool newOpen = hasOpen ? wantOpen : !current;
                                    return CommandResult::Success({{"wouldDo", {{"showPerformance", newOpen}}}});
                                }
                                // DR27: writing only the config left the RUNNING panel untouched
                                // (g_ui.showPerformance is read once at startup). Flip the live flag
                                // on the UI thread — g_ui is UI-thread-owned and this handler may run
                                // on an MCP / Lua worker thread — then persist OFF the UI thread.
                                // WriteConfigJson does blocking file I/O; running it inside the UI
                                // callback would stall the render-thread Drain() (Pillar 2), so only
                                // the flag flip is marshalled and the write happens on this handler's
                                // (worker / CLI) thread.
                                try {
                                    const bool newOpen = RunOnUiThread<bool>(app, [hasOpen, wantOpen]() -> bool {
                                        const bool v = hasOpen ? wantOpen : !g_ui.showPerformance;
                                        g_ui.showPerformance = v;
                                        return v;
                                    });
                                    nlohmann::json cfgJson = ConfigManager::LoadMergedConfigJson();
                                    cfgJson["show_performance_window"] = newOpen;
                                    ConfigManager::WriteConfigJson(cfgJson);
                                    ConfigManager::InvalidateCache();
                                    return CommandResult::Success({{"showPerformance", newOpen}});
                                } catch (const std::exception& e) {
                                    return CommandResult::Failure(ErrorCode::HandlerError,
                                                                  std::string("perf.toggle_panel failed: ") + e.what());
                                }
                            });
    c.DryRunSupported = true;
    c.Params = {
        {[] {
            ParamSpec p;
            p.Name = "open";
            p.Type = ParamType::Bool;
            p.Description = "true=show, false=hide, omit=toggle current state.";
            return p;
        }()},
    };
    reg.Register(std::move(c));
}

CommandResult RunGridEditBurstOnUi(AppController& app, int count, const std::string& fieldId,
                                   const std::string& issueIdArg, const std::string& newValue) {
    // Resolve issueId: explicit arg wins; otherwise pick the first cached
    // ticket. Works in --spawn mode with a seeded cache; for an empty
    // cache we synthesize a sentinel ID — the commit fails fast at the
    // worker (no backend) and the UI-thread cost we want to measure is
    // unchanged.
    std::string issueId = issueIdArg;
    if (issueId.empty()) {
        const auto tickets = app.GetActiveTicketsSnapshot();
        if (tickets && !tickets->empty()) {
            issueId = tickets->front().id;
        } else {
            issueId = "SMATCHET-PERF-1";
        }
    }

    TrackerField field;
    if (const TrackerField* meta = app.FindFieldById(fieldId)) {
        field = *meta;
    } else {
        field.Id = fieldId;
        field.Name = fieldId;
        field.Type = "string";
    }

    // Build the synthetic pending-edit batch once and reuse a fresh
    // copy per iteration. The vector input shape matches the
    // SmatchetActiveProjectGridUi caller.
    PendingFieldEdit oneEdit;
    oneEdit.IssueId = issueId;
    oneEdit.Field = field;
    oneEdit.Values = std::vector<std::string>{newValue};

    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    static const std::vector<CachedTicket> kEmptyTickets;
    const std::vector<CachedTicket>& tickets = ticketsSnap ? *ticketsSnap : kEmptyTickets;

    std::vector<double> samplesMs;
    samplesMs.reserve(static_cast<size_t>(count));

    const auto runStart = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) {
        std::vector<PendingFieldEdit> pendingEdits;
        pendingEdits.push_back(oneEdit);
        const auto t0 = std::chrono::steady_clock::now();
        ProcessGridFieldEdits(app, g_ui, tickets, pendingEdits, /*readOnlyMode=*/false);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        samplesMs.push_back(ms);
    }
    const auto runEnd = std::chrono::steady_clock::now();
    const double wallMs = std::chrono::duration<double, std::milli>(runEnd - runStart).count();

    // Stats: mean, p50, p95, p99, max from samplesMs.
    std::vector<double> sorted = samplesMs;
    std::sort(sorted.begin(), sorted.end());
    const auto pct = [&sorted](double q) -> double {
        if (sorted.empty()) {
            return 0.0;
        }
        const size_t n = sorted.size();
        const double idxD = q * static_cast<double>(n - 1);
        const double capD = static_cast<double>(n - 1);
        const size_t idx = static_cast<size_t>(idxD < capD ? idxD : capD);
        return sorted[idx];
    };
    const double mean = samplesMs.empty() ? 0.0
                                          : std::accumulate(samplesMs.begin(), samplesMs.end(), 0.0) /
                                                static_cast<double>(samplesMs.size());

    nlohmann::json out;
    out["field"] = fieldId;
    out["issue"] = issueId;
    out["iterations"] = count;
    out["wall_clock_ms"] = wallMs;
    out["per_event_mean_ms"] = mean;
    out["per_event_p50_ms"] = pct(0.50);
    out["per_event_p95_ms"] = pct(0.95);
    out["per_event_p99_ms"] = pct(0.99);
    out["per_event_max_ms"] = sorted.empty() ? 0.0 : sorted.back();
    out["target_mean_ms"] = 6.94;
    out["target_p99_ms"] = 10.0;
    out["ok_mean"] = mean <= 6.94;
    out["ok_p99"] = pct(0.99) <= 10.0;
    return CommandResult::Success(std::move(out));
}

void RegisterDebugGridEditBurstCommand(CommandRegistry& reg, AppController& app) {
    // debug.grid.edit-burst — headless harness that exercises the grid cell
    // commit pipeline `count` times and reports per-invocation wall-clock
    // statistics. Used by `scripts/dev/test-grid-edit-perf-postfix.sh` to
    // assert the UI thread never blocks for an HTTP roundtrip on edit.
    // The handler hops to the UI thread (RunOnUiThreadAsCommandResult)
    // because ProcessGridFieldEdits mutates the singleton UiDrawSession
    // (g_ui) which is UI-thread owned. Per-iteration timing is therefore a
    // true measure of UI-thread cost — the same code path the user pays.
    // With `Backend == nullptr` (typical `--spawn` mode) the worker
    // short-circuits at "no tracker backend"; the regression check is
    // about UI-thread cost, not the HTTP transport. Live-tracker E2E is
    // manual.
    Command c =
        MakeCommand("debug.grid.edit-burst",
                    "Drive N synthetic cell commits through ProcessGridFieldEdits and report wall-clock stats.",
                    [&app](const nlohmann::json& args, const CommandContext& /*ctx*/) {
                        const int count = (std::max)(1, (std::min)(static_cast<int>(args.value("count", 200)), 100000));
                        const std::string fieldId = args.value("field", std::string("summary"));
                        const std::string issueIdArg = args.value("issue", std::string());
                        const std::string newValue = args.value("value", std::string("burst-edit"));

                        return RunOnUiThreadAsCommandResult(app, [&app, count, fieldId, issueIdArg, newValue]() {
                            return RunGridEditBurstOnUi(app, count, fieldId, issueIdArg, newValue);
                        });
                    });
    std::vector<ParamSpec> params;
    params.push_back(PInt("count", "Number of synthetic edits to drive. Clamped to [1, 100000].", 200));
    params.push_back(PString("field", "Tracker field id (default 'summary').", false));
    params.push_back(
        PString("issue", "Issue id (default: first cached ticket; or sentinel if cache is empty).", false));
    params.push_back(PString("value", "New value to write (default 'burst-edit').", false));
    c.Params = std::move(params);
    c.Destructive = false; // No persistent side-effects when --spawn + no backend.
    c.Idempotent = false;
    reg.Register(std::move(c));
}

} // namespace

void RegisterPerfCommands(CommandRegistry& reg, AppController& app) {
    RegisterPerfSnapshotCommand(reg);
    RegisterPerfMemoryCommand(reg, app);
    RegisterPerfFrameCountCommand(reg);
    RegisterPerfDumpCommand(reg);
    RegisterPerfResetCommand(reg);
    RegisterPerfTogglePanelCommand(reg, app);
    RegisterDebugGridEditBurstCommand(reg, app);
}

} // namespace cmd
} // namespace smatchet
