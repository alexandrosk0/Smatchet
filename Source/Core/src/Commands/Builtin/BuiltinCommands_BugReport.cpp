// bug.report — file a bug to the fixed dev GitHub repo (modal in-app, or
// headless text-only from CLI/MCP/Lua). docs/plans/shipped/log-a-bug-github.md
// Slice 3. Strict-lint zone (Commands/): LOG_* only, no raw new/delete,
// const& non-trivial params, obj["k"]=v.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"

// This TU only passes the narrow IAppMeta& through to diagnostics::SubmitBugReport and uses the
// poster for the modal UI-thread hop — no AppController dependency at all; the dispatcher upcasts
// the concrete object into both narrow refs.
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.
#include "ConfigManager.h"
#include "Diagnostics/BugReportService.h"
#include "SmatchetUiSession.h"

#include <cstdlib>
#include <string>
#include <utility>

// Same g_ui singleton extern used by BuiltinCommands_Debug.cpp / ViewToggleCommands.cpp.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PString;

void RegisterBugReportCommands(CommandRegistry& reg, IAppMeta& app, IMainThreadPoster& poster) {
    Command c = MakeCommand(
        "bug.report", "File a bug to the configured dev GitHub repo (modal, or headless with --description).",
        [&app, &poster](const nlohmann::json& args, const CommandContext& ctx) {
            const std::string description = args.value("description", std::string());
            const bool screenshot = args.value("screenshot", true);

            if (ctx.DryRun) {
                const TrackerConfig cfg = ConfigManager::Load();
                const char* envTok = std::getenv("SMATCHET_BUGREPORT_GITHUB_TOKEN");
                const diagnostics::ResolvedBugTarget target =
                    diagnostics::ResolveBugReportTarget(cfg, envTok ? std::string(envTok) : std::string());
                nlohmann::json out = nlohmann::json::object();
                out["resolved"] = target.Ok;
                if (target.Ok && target.UseRelay) {
                    out["mode"] = "relay";
                    out["relayUrl"] = target.RelayUrl;
                    out["relayKeyPresent"] = !target.RelayKey.empty();
                } else if (target.Ok) {
                    out["mode"] = "direct";
                    out["owner"] = target.Owner;
                    out["repo"] = target.Repo;
                    out["patPresent"] = !target.Pat.empty();
                } else {
                    out["error"] = target.Error;
                }
                return CommandResult::Success(out);
            }

            // Modal mode (no description) — flip the UI latch on the UI thread; no submit here.
            if (description.empty()) {
                return RunOnUiThreadAsCommandResult(poster, [screenshot]() {
                    g_ui.showBugReport = true;
                    g_ui.bugReportOpenLatch = true;
                    g_ui.bugReportInclScreenshot = screenshot;
                    return CommandResult::Success({{"modalOpened", true}});
                });
            }

            // Headless mode (description set) — text-only submit (no live frame in CLI/MCP/Lua).
            diagnostics::BugReportOptions opts;
            opts.UserDescription = description;
            opts.IncludeScreenshot = false;
            const diagnostics::SubmitResult r = diagnostics::SubmitBugReport(app, opts);
            if (!r.Ok) {
                return CommandResult::Failure(ErrorCode::BackendError, "Bug report failed: " + r.Error);
            }
            nlohmann::json out = nlohmann::json::object();
            out["ok"] = true;
            out["issueKey"] = r.IssueKey;
            out["url"] = r.Url;
            return CommandResult::Success(out);
        });
    c.Destructive = true;
    c.Idempotent = false;
    c.DryRunSupported = true;
    c.AsyncSafe = false;
    c.Params = {
        PString("description",
                "Bug description. When set, files headlessly (text-only). When omitted, opens the modal."),
        {[] {
            ParamSpec p;
            p.Name = "screenshot";
            p.Type = ParamType::Bool;
            p.Default = std::make_shared<nlohmann::json>(true);
            p.Description = "Modal only: attach a screenshot (always text-redacted; default true).";
            return p;
        }()},
    };
    reg.Register(std::move(c));
}

} // namespace cmd
} // namespace smatchet
