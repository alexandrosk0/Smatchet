// ci-react.* — CI react-loop CLI surface (phase 8 of the
// coderabbit-react-loop plan, docs/design/coderabbit-react-loop.md).
//
// Five commands:
//   ci-react.start                       — enable + restart the poll worker.
//   ci-react.stop                        — disable + restart the poll worker.
//   ci-react.status                      — print interval / check-run lists /
//                                          iteration budget / transient-rerun
//                                          cap / last-tick info.
//   ci-react.poll-now <pr-url>           — fire one tick of the check-run
//                                          watcher (manual probe).
//   ci-react.rerun <pr-url> <workflow-id>
//                                        — invoke `RerunWorkflowRun(runId)` for
//                                          the named PR/workflow without
//                                          spawning a harness.
//
// Gated SMATCHET_WITH_AGENTIC; OFF build registers short-circuit stubs.
//
// All `start/stop/poll-now/rerun` are Destructive; `status` is read-only.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"

#if defined(SMATCHET_WITH_AGENTIC)
#include "PrCheckRunWatcher.h"
#endif

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PInt;
using builtin_detail::PString;

#if defined(SMATCHET_WITH_AGENTIC)

namespace {

CommandResult FlipCiReactEnabled(AppController& app, bool enabled) {
    LOG_TRACE("FlipCiReactEnabled enter enabled=%d", static_cast<int>(enabled));
    TrackerConfig cfg = ConfigManager::Load();
    if (cfg.CiReact.Enabled == enabled) {
        LOG_DEBUG("FlipCiReactEnabled: already in target state enabled=%d", static_cast<int>(enabled));
        nlohmann::json data = nlohmann::json::object();
        data["enabled"] = enabled;
        data["restarted"] = false;
        data["reason"] = "already in target state";
        return CommandResult::Success(std::move(data));
    }
    cfg.CiReact.Enabled = enabled;
    ConfigManager::Save(cfg);
    app.RestartAgenticPollAsync();
    LOG_INFO("ci-react: master toggle flipped to %s", enabled ? "enabled" : "disabled");
    nlohmann::json data = nlohmann::json::object();
    data["enabled"] = enabled;
    data["restarted"] = true;
    return CommandResult::Success(std::move(data));
}

} // namespace

void RegisterCiReactCommands(CommandRegistry& reg, AppController& app) {
    LOG_TRACE("RegisterCiReactCommands enter");
    // ─── ci-react.start ───────────────────────────────────────────────
    {
        Command c = MakeCommand("ci-react.start",
                                "Enable the CI react loop. Persists cfg.ci_react.enabled=true and restarts the "
                                "scheduled-poll worker.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    LOG_TRACE("cmd ci-react.start");
                                    CommandResult r = FlipCiReactEnabled(app, true);
                                    LOG_INFO("cmd ci-react.start result ok=%d", static_cast<int>(r.Ok));
                                    return r;
                                });
        c.AsyncSafe = false;
        c.Idempotent = true;
        c.Destructive = true;
        reg.Register(std::move(c));
    }

    // ─── ci-react.stop ────────────────────────────────────────────────
    {
        Command c = MakeCommand("ci-react.stop",
                                "Disable the CI react loop. Persists cfg.ci_react.enabled=false and restarts "
                                "the scheduled-poll worker.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    LOG_TRACE("cmd ci-react.stop");
                                    CommandResult r = FlipCiReactEnabled(app, false);
                                    LOG_INFO("cmd ci-react.stop result ok=%d", static_cast<int>(r.Ok));
                                    return r;
                                });
        c.AsyncSafe = false;
        c.Idempotent = true;
        c.Destructive = true;
        reg.Register(std::move(c));
    }

    // ─── ci-react.status ──────────────────────────────────────────────
    {
        Command c =
            MakeCommand("ci-react.status",
                        "Print the CI react loop's runtime state — config (interval, watched / ignored check "
                        "names, auto-dispatch toggles, transient-rerun cap), watcher iteration-budget + rerun "
                        "cap snapshot, and last-tick wall-clock timestamp.",
                        [&app](const nlohmann::json&, const CommandContext&) {
                            LOG_TRACE("cmd ci-react.status");
                            const TrackerConfig cfg = ConfigManager::Load();
                            nlohmann::json data = nlohmann::json::object();
                            data["enabled"] = cfg.CiReact.Enabled;
                            data["poll_interval_sec"] = cfg.CiReact.PollIntervalSec;
                            data["watched_base_branches"] = cfg.CiReact.WatchedBaseBranches;
                            data["watched_check_names"] = cfg.CiReact.WatchedCheckNames;
                            data["ignored_check_names"] = cfg.CiReact.IgnoredCheckNames;
                            data["auto_dispatch_build_doctor"] = cfg.CiReact.AutoDispatchBuildDoctor;
                            data["auto_dispatch_test_rig"] = cfg.CiReact.AutoDispatchTestRig;
                            data["auto_dispatch_debug_detective"] = cfg.CiReact.AutoDispatchDebugDetective;
                            data["transient_rerun_enabled"] = cfg.CiReact.TransientRerunEnabled;
                            data["transient_rerun_max_per_pr"] = cfg.CiReact.TransientRerunMaxPerPr;
                            data["iteration_budget_per_pr"] = cfg.CiReact.IterationBudgetPerPr;
                            data["annotation_fetch_count"] = cfg.CiReact.AnnotationFetchCount;
                            data["log_tail_lines"] = cfg.CiReact.LogTailLines;
                            data["last_tick_at_sec"] = app.GetAgenticLastPollAtSec();
                            auto* watcher = app.GetAgenticPrCheckRunWatcher();
                            data["watcher_present"] = (watcher != nullptr);
                            if (watcher != nullptr) {
                                data["watcher_iteration_budget"] = watcher->GetIterationBudget();
                                data["watcher_transient_rerun_cap"] = watcher->GetTransientRerunCap();
                            }
                            LOG_INFO("cmd ci-react.status result ok=%d enabled=%d watcher_present=%d", 1,
                                     static_cast<int>(cfg.CiReact.Enabled), static_cast<int>(watcher != nullptr));
                            return CommandResult::Success(std::move(data));
                        });
        c.AsyncSafe = true;
        c.Idempotent = true;
        c.Destructive = false;
        reg.Register(std::move(c));
    }

    // ─── ci-react.poll-now ────────────────────────────────────────────
    {
        Command c =
            MakeCommand("ci-react.poll-now",
                        "Fire one tick of the CI check-run watcher right now (synchronous on the caller's "
                        "thread — wrap in a background task from the UI). Useful for manual probes; respects "
                        "the watcher's existing classifier + dispatcher wiring, may issue transient reruns or "
                        "dispatch ad-hoc spawns. `pr_url` is reserved — phase-8 watcher walks every row in "
                        "`agent_open_pr_watch`.",
                        [&app](const nlohmann::json& args, const CommandContext&) {
                            LOG_TRACE("cmd ci-react.poll-now");
                            const std::string prUrl = args.value("pr_url", std::string());
                            (void)app.GetAgenticHandoffController();
                            auto* watcher = app.GetAgenticPrCheckRunWatcher();
                            if (watcher == nullptr) {
                                LOG_INFO("cmd ci-react.poll-now result ok=%d (watcher unwired)", 0);
                                return CommandResult::Failure(
                                    ErrorCode::HandlerError,
                                    "PrCheckRunWatcher not constructed — configure GitHub PAT (cfg.GitHubPat) or "
                                    "check the agent-proposals store init.");
                            }
                            const int actioned = watcher->Tick();
                            LOG_INFO("ci-react.poll-now: tick actioned=%d (pr_url=%s)", actioned, prUrl.c_str());
                            LOG_INFO("cmd ci-react.poll-now result ok=%d actioned=%d", 1, actioned);
                            nlohmann::json data = nlohmann::json::object();
                            data["pr_url"] = prUrl;
                            data["actioned"] = actioned;
                            return CommandResult::Success(std::move(data));
                        });
        c.Params = {PString("pr_url",
                            "Canonical PR URL (https://github.com/owner/repo/pull/N). Reserved — "
                            "phase 8 ticks every watched PR regardless.",
                            false)};
        c.AsyncSafe = false;
        c.Idempotent = false;
        c.Destructive = true;
        reg.Register(std::move(c));
    }

    // ─── ci-react.rerun ───────────────────────────────────────────────
    {
        Command c = MakeCommand(
            "ci-react.rerun",
            "Manually invoke `RerunWorkflowRun(runId)` against the GitHub Actions REST API. Same "
            "code path the watcher uses for transient flakes — useful when an operator decides a "
            "failure was transient and wants to rerun without waiting for the next poll. Requires "
            "the PAT (cfg.GitHubPat) to carry the `actions:write` scope.",
            [&app](const nlohmann::json& args, const CommandContext&) {
                LOG_TRACE("cmd ci-react.rerun");
                const std::string prUrl = args.value("pr_url", std::string());
                const long long workflowId =
                    static_cast<long long>(args.value("workflow_id", static_cast<long long>(0)));
                if (prUrl.empty()) {
                    LOG_INFO("cmd ci-react.rerun result ok=%d (missing pr_url)", 0);
                    return CommandResult::Failure(ErrorCode::ValidationError, "--pr-url is required.");
                }
                if (workflowId <= 0) {
                    LOG_INFO("cmd ci-react.rerun result ok=%d (missing/invalid workflow_id)", 0);
                    return CommandResult::Failure(ErrorCode::ValidationError,
                                                  "--workflow-id is required (positive integer).");
                }
                std::string owner;
                std::string repo;
                std::string parseErr;
                if (!smatchet::agentic::PrCheckRunWatcher::ParseOwnerRepoFromPrUrl(prUrl, owner, repo, parseErr)) {
                    LOG_INFO("cmd ci-react.rerun result ok=%d (malformed pr_url)", 0);
                    return CommandResult::Failure(ErrorCode::ValidationError,
                                                  "malformed --pr-url '" + prUrl + "': " + parseErr);
                }
                LOG_DEBUG("ci-react.rerun: invoking RerunWorkflowRun owner=%s repo=%s workflowId=%lld", owner.c_str(),
                          repo.c_str(), workflowId);
                std::string err;
                if (!app.RerunAgenticWorkflowRun(owner, repo, static_cast<std::int64_t>(workflowId), err)) {
                    LOG_INFO("cmd ci-react.rerun result ok=%d (RerunAgenticWorkflowRun failed)", 0);
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                LOG_INFO("ci-react.rerun: %s/%s runId=%lld ok", owner.c_str(), repo.c_str(), workflowId);
                LOG_INFO("cmd ci-react.rerun result ok=%d", 1);
                nlohmann::json data = nlohmann::json::object();
                data["pr_url"] = prUrl;
                data["owner"] = owner;
                data["repo"] = repo;
                data["workflow_id"] = workflowId;
                return CommandResult::Success(std::move(data));
            });
        ParamSpec prUrlParam = PString("pr_url", "Canonical PR URL (https://github.com/owner/repo/pull/N).", true);
        ParamSpec runIdParam = PInt("workflow_id", "GitHub Actions workflow-run id to rerun (positive integer).", 0);
        runIdParam.Required = true;
        c.Params = {std::move(prUrlParam), std::move(runIdParam)};
        c.AsyncSafe = false;
        c.Idempotent = false;
        c.Destructive = true;
        reg.Register(std::move(c));
    }
}

#else // !SMATCHET_WITH_AGENTIC

void RegisterCiReactCommands(CommandRegistry& reg, AppController& /*app*/) {
    const char* const kNames[] = {"ci-react.start", "ci-react.stop", "ci-react.status", "ci-react.poll-now",
                                  "ci-react.rerun"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        const std::string name = kNames[i];
        Command c =
            MakeCommand(name, "CI react loop disabled in this build (SMATCHET_WITH_AGENTIC=OFF).",
                        [name](const nlohmann::json&, const CommandContext&) {
                            return CommandResult::Failure(
                                ErrorCode::HandlerError, name + ": agentic flow not built (SMATCHET_WITH_AGENTIC=OFF)");
                        });
        c.Idempotent = true;
        reg.Register(std::move(c));
    }
}

#endif // SMATCHET_WITH_AGENTIC

} // namespace cmd
} // namespace smatchet
