// app.* — application lifecycle, version, read-only mode, update checks.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

// fan-in Phase 5: depend on the narrow IAppMeta facet, not the full AppController.h.
#include "Interfaces/IAppMeta.h"
#include "Types/AppUpdateTypes.h" // AppUpdateInfo, named directly by the app.check_updates handler
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.
#include "ConfigManager.h"
#include "Diagnostics/AboutInfo.h"

#include <cstddef>
#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;

namespace {

/// Fold the Help > About snapshot into `app.version`'s JSON. Same
/// diagnostics::AboutInfo the dialog renders, so the two can never disagree —
/// which is the whole point of exposing it here: a bug thread can ask for
/// `Smatchet.exe cmd app.version` instead of a screenshot. Kept out of the
/// handler lambda so `app.version` stays readable; the assembly is a straight
/// field copy with no logic of its own.
void AddAboutFields(nlohmann::json& out, IAppMeta& app) {
    const diagnostics::AboutInfo info = diagnostics::GatherAboutInfo(app.GetAppVersion(), app.GetGitHubReleaseRepo());

    out["repoUrl"] = info.RepoUrl;

    nlohmann::json build;
    build["config"] = info.Build.Config;
    build["compilerId"] = info.Build.CompilerId;
    // clang-format off
    // SMATCHET_DEVIATION(rule=duplication; reason=the flat `obj["key"] = struct.Field;` run below token-matches BuildAnnotateExportJson in Ui/AnnotateAnalysisUi_Modals.cpp — same nlohmann idiom, different structs, disjoint field names. Factoring it out would couple the app command layer to the P4-annotate UI for no shared behaviour, which AGENTS.md Pillar 5 names as the CRITICAL failure mode; owner=commands; revisit=if a declarative struct-to-json reflection helper ever lands)
    // clang-format on
    build["compilerVersion"] = info.Build.CompilerVersion;
    build["cmakeVersion"] = info.Build.CMakeVersion;
    build["dateTime"] = info.Build.DateTime;
    build["imguiVersion"] = info.Build.ImGuiVersion;
    build["targetArch"] = info.Build.TargetArch;
    build["buildTag"] = info.Build.BuildTag;
    out["build"] = std::move(build);

    nlohmann::json git;
    git["sha"] = info.Git.Sha;
    git["shaShort"] = info.Git.ShaShort;
    git["branch"] = info.Git.Branch;
    git["dirty"] = info.Git.Dirty;
    git["shallow"] = info.Git.Shallow;
    out["git"] = std::move(git);

    nlohmann::json runtime;
    runtime["os"] = info.Runtime.Os;
    runtime["arch"] = info.Runtime.Arch;
    // Omitted rather than guessed when the probe never resolved (pre-1709 Windows,
    // non-Windows) — same contract as the dialog and BuildAboutReportText.
    if (info.Runtime.EmulationResolved) {
        runtime["nativeArch"] = info.Runtime.NativeArch;
        runtime["emulated"] = info.Runtime.Emulated;
    }
    runtime["tracker"] = info.Runtime.Tracker;
    runtime["userDataDir"] = info.Runtime.UserDataDir;
    runtime["configPath"] = info.Runtime.ConfigPath;
    out["runtime"] = std::move(runtime);

    nlohmann::json deps = nlohmann::json::array();
    for (std::size_t i = 0; i < info.Deps.size(); ++i) {
        const diagnostics::AboutDep& dep = info.Deps[i];
        nlohmann::json d;
        d["name"] = dep.Name;
        d["version"] = dep.Version;
        d["license"] = dep.License;
        d["url"] = dep.Url;
        deps.push_back(std::move(d));
    }
    out["deps"] = std::move(deps);
}

} // namespace

void RegisterAppCommands(CommandRegistry& reg, IAppMeta& app) {
    {
        Command c = MakeCommand("app.version", "Application version + build metadata.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    nlohmann::json out;
                                    // Pre-existing keys, kept verbatim for back-compat with any
                                    // script already parsing this command.
                                    out["version"] = app.GetAppVersion();
                                    out["releaseRepo"] = app.GetGitHubReleaseRepo();
                                    AddAboutFields(out, app);
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
