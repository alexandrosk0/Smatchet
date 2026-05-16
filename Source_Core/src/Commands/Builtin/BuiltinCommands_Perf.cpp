// perf.* — UI performance monitor snapshot, reset, dump, and panel toggle.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "ConfigManager.h"
#include "UiPerfMonitor.h"

#include <algorithm>
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

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PString;

void RegisterPerfCommands(CommandRegistry& reg, AppController& /*app*/) {
    {
        Command c = MakeCommand("perf.snapshot", "Per-scope UI perf rows from the last drawn frame.",
                                [](const nlohmann::json&, const CommandContext&) {
                                    std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
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
                                        arr.push_back(std::move(one));
                                    }
                                    nlohmann::json out;
                                    out["rows"] = std::move(arr);
                                    return CommandResult::Success(std::move(out));
                                });
        c.Description =
            "Returns array of UI perf rows ({name, lastTotalMs, avgPerCallMs, maxMs, calls, lifetimeHits, emaAvgMs}).";
        reg.Register(std::move(c));
    }

    {
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

    {
        Command c =
            MakeCommand("perf.dump", "Write perf snapshot to a JSON file and return {file, count}.",
                        [](const nlohmann::json& args, const CommandContext&) {
                            const std::string& userDataDir = ConfigManager::GetUserDataDirectory();
                            std::string outPath = args.value("outPath", std::string());
                            if (outPath.empty()) {
                                std::time_t t = std::time(nullptr);
                                char ts[64] = {};
                                std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&t));
                                outPath = userDataDir + "perf-snapshot-" + ts + ".json";
                            }
                            std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
                            nlohmann::json doc = nlohmann::json::array();
                            std::transform(rows.begin(), rows.end(), std::back_inserter(doc), [](const UiPerfRow& r) {
                                return nlohmann::json{{"name", r.name},
                                                      {"lastTotalMs", r.lastTotalMs},
                                                      {"avgPerCallMs", r.avgPerCallMs},
                                                      {"maxMs", r.maxMs},
                                                      {"calls", r.calls},
                                                      {"emaAvgMs", r.emaAvgMs}};
                            });
                            // Write file.
                            std::error_code ec;
                            fs::path outFs(outPath);
                            fs::create_directories(outFs.parent_path(), ec);
                            std::FILE* f = std::fopen(outPath.c_str(), "wb");
                            if (!f) {
                                return CommandResult::Failure(ErrorCode::HandlerError,
                                                              "Could not write perf dump to '" + outPath + "'.");
                            }
                            const std::string s = doc.dump(2);
                            std::fwrite(s.data(), 1, s.size(), f);
                            std::fclose(f);
                            nlohmann::json out;
                            out["file"] = outPath;
                            out["count"] = static_cast<int>(rows.size());
                            return CommandResult::Success(std::move(out));
                        });
        c.Params = {PString("outPath", "Output file path (default: <userData>/perf-snapshot-<ts>.json).")};
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("perf.reset", "Clear all accumulated UI perf measurements (use before a benchmark run).",
                        [](const nlohmann::json&, const CommandContext&) {
                            UiPerfMonitor::Instance().Reset();
                            return CommandResult::Success({{"reset", true}});
                        });
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("perf.toggle_panel", "Show or hide the Performance Monitor panel (persists to config).",
                                [](const nlohmann::json& args, const CommandContext& ctx) {
                                    // Read current value to compute toggle if `open` not supplied.
                                    TrackerConfig cfg = ConfigManager::Load();
                                    const bool currentlyOpen = cfg.ShowPerformanceWindow;
                                    const bool newOpen =
                                        args.contains("open") ? args.value("open", false) : !currentlyOpen;
                                    if (ctx.DryRun) {
                                        return CommandResult::Success({{"wouldDo", {{"showPerformance", newOpen}}}});
                                    }
                                    nlohmann::json cfgJson = ConfigManager::LoadMergedConfigJson();
                                    cfgJson["show_performance_window"] = newOpen;
                                    ConfigManager::WriteConfigJson(cfgJson);
                                    ConfigManager::InvalidateCache();
                                    return CommandResult::Success({{"showPerformance", newOpen}});
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
}

} // namespace cmd
} // namespace smatchet
