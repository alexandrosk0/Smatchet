// app.* — application lifecycle, version, read-only mode, update checks.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

// fan-in Phase 5: depend on the narrow IAppMeta facet, not the full AppController.h.
#include "Interfaces/IAppMeta.h"
#include "Types/AppUpdateTypes.h" // AppUpdateInfo, named directly by the app.check_updates handler
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.
#include "ConfigManager.h"

#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;

void RegisterAppCommands(CommandRegistry& reg, IAppMeta& app) {
    {
        Command c = MakeCommand("app.version", "Application version + build metadata.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    nlohmann::json out;
                                    out["version"] = app.GetAppVersion();
                                    out["releaseRepo"] = app.GetGitHubReleaseRepo();
                                    return CommandResult::Success(std::move(out));
                                });
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("app.quit", "Request graceful shutdown of the running instance.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    app.RequestAppQuit();
                                    nlohmann::json out;
                                    out["requested"] = true;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Destructive = true; // takes the window down — requires --yes / __confirm
        c.Idempotent = true;
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("app.check_updates", "Check GitHub for a newer Smatchet release.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    const AppUpdateInfo info = app.CheckForAppUpdate(false);
                                    nlohmann::json out;
                                    out["ok"] = info.Ok;
                                    out["updateAvailable"] = info.UpdateAvailable;
                                    out["currentVersion"] = info.CurrentVersion;
                                    out["latestVersion"] = info.LatestVersion;
                                    out["releaseUrl"] = info.ReleaseUrl;
                                    if (!info.Error.empty())
                                        out["error"] = info.Error;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Idempotent = false;
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("app.set_readonly", "Enable or disable read-only mode (persists to config).",
                                [](const nlohmann::json& args, const CommandContext& ctx) {
                                    const bool on = args.value("on", false);
                                    if (ctx.DryRun) {
                                        return CommandResult::Success({{"wouldDo", {{"readOnlyMode", on}}}});
                                    }
                                    TrackerConfig cfg = ConfigManager::Load();
                                    cfg.ReadOnlyMode = on;
                                    ConfigManager::Save(cfg);
                                    ConfigManager::InvalidateCache();
                                    return CommandResult::Success({{"readOnlyMode", on}});
                                });
        c.Destructive = true;
        c.DryRunSupported = true;
        c.Params = {
            {[] {
                ParamSpec p;
                p.Name = "on";
                p.Type = ParamType::Bool;
                p.Required = true;
                p.Description = "true = enable read-only, false = disable.";
                return p;
            }()},
        };
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
