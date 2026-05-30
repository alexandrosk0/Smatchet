// bug.report — file a bug to the fixed dev GitHub repo (modal in-app, or
// headless text-only from CLI/MCP/Lua). docs/plans/active/log-a-bug-github.md
// Slice 3. Strict-lint zone (Commands/): LOG_* only, no raw new/delete,
// const& non-trivial params, obj["k"]=v.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"

#include "AppController.h"
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

void RegisterBugReportCommands(CommandRegistry& reg, AppController& app) {
    Command c = MakeCommand(
        "bug.report", "File a bug to the configured dev GitHub repo (modal, or headless with --description).",
        [&app](const nlohmann::json& args, const CommandContext& ctx) {
            const std::string description = args.value("description", std::string());
            const bool screenshot = args.value("screenshot", true);
            const bool censored = args.value("censored", false);

            if (ctx.DryRun) {
                const TrackerConfig cfg = ConfigManager::Load();
                const char* envTok = std::getenv("SMATCHET_BUGREPORT_GITHUB_TOKEN");
                const diagnostics::ResolvedBugTarget target =
                    diagnostics::ResolveBugReportTarget(cfg, envTok ? std::string(envTok) : std::string());
                nlohmann::json out = nlohmann::json::object();
                out["resolved"] = target.Ok;
                if (target.Ok) {
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
                return RunOnUiThreadAsCommandResult(app, [screenshot, censored]() {
                    g_ui.showBugReport = true;
                    g_ui.bugReportOpenLatch = true;
                    g_ui.bugReportInclScreenshot = screenshot;
                    g_ui.bugReportShotMode = censored ? 1 : 0;
                    return CommandResult::Success({{"modalOpened", true}});
                });
            }

            // Headless mode (description set) — text-only submit (no live frame in CLI/MCP/Lua).
            // The `censored` param is meaningful only in modal mode; ignored here (no screenshot).
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
            p.Default = true;
            p.Description = "Modal only: attach a screenshot (default true).";
            return p;
        }()},
        {[] {
            ParamSpec p;
            p.Name = "censored";
            p.Type = ParamType::Bool;
            p.Default = false;
            p.Description = "Modal only: censor the screenshot so no text is readable (default false).";
            return p;
        }()},
    };
    reg.Register(std::move(c));
}

} // namespace cmd
} // namespace smatchet
