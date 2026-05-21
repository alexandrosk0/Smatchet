// coderabbit-react.* — CodeRabbit react-loop CLI surface (phase 8 of the
// coderabbit-react-loop plan, docs/design/coderabbit-react-loop.md).
//
// Four commands:
//   coderabbit-react.start                — enable + restart the poll worker.
//   coderabbit-react.stop                 — disable + restart the poll worker.
//   coderabbit-react.status               — print interval / bot allow-list /
//                                           iteration budget / last-tick info.
//   coderabbit-react.poll-now <pr-url>    — fire one tick of the watcher (manual
//                                           probe; respects the watcher's own
//                                           cursor + classifier wiring).
//
// All four are gated SMATCHET_WITH_AGENTIC; the OFF build registers friendly
// short-circuit stubs so `commands.list` enumerates them in both gating states.
//
// Start / stop / poll-now are Destructive (may mutate config, dispatch spawns,
// post GitHub comments); status is read-only.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"

#if defined(SMATCHET_WITH_AGENTIC)
#include "PrCommentWatcher.h"
#endif

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PString;

#if defined(SMATCHET_WITH_AGENTIC)

namespace {

// Centralise the master-toggle flip + persist + poll restart. Same shape used
// by both `coderabbit-react.start` and `coderabbit-react.stop`. The
// `RestartAgenticPollAsync` call is the production-safe entry point — UI
// threads cannot synchronously join the poll worker (a mid-batch LLM call can
// block for minutes), so we always go through the deferred-join path.
CommandResult FlipCoderabbitReactEnabled(AppController& app, bool enabled) {
    LOG_TRACE("FlipCoderabbitReactEnabled enter enabled=%d", static_cast<int>(enabled));
    TrackerConfig cfg = ConfigManager::Load();
    if (cfg.CoderabbitReact.Enabled == enabled) {
        LOG_DEBUG("FlipCoderabbitReactEnabled: already in target state enabled=%d — no-op", static_cast<int>(enabled));
        nlohmann::json data = nlohmann::json::object();
        data["enabled"] = enabled;
        data["restarted"] = false;
        data["reason"] = "already in target state";
        return CommandResult::Success(std::move(data));
    }
    cfg.CoderabbitReact.Enabled = enabled;
    ConfigManager::Save(cfg);
    LOG_DEBUG("FlipCoderabbitReactEnabled: persisted cfg.CoderabbitReact.Enabled=%d; restarting poll worker async",
              static_cast<int>(enabled));
    app.RestartAgenticPollAsync();
    LOG_INFO("coderabbit-react: master toggle flipped to %s", enabled ? "enabled" : "disabled");
    nlohmann::json data = nlohmann::json::object();
    data["enabled"] = enabled;
    data["restarted"] = true;
    return CommandResult::Success(std::move(data));
}

} // namespace

void RegisterCoderabbitCommands(CommandRegistry& reg, AppController& app) {
    LOG_TRACE("RegisterCoderabbitCommands enter (SMATCHET_WITH_AGENTIC=ON)");
    // ─── coderabbit-react.start ───────────────────────────────────────
    {
        Command c = MakeCommand("coderabbit-react.start",
                                "Enable the CodeRabbit react loop. Persists cfg.coderabbit_react.enabled=true and "
                                "restarts the scheduled-poll worker so the next tick picks up the change without "
                                "an app restart.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    LOG_TRACE("cmd coderabbit-react.start");
                                    CommandResult r = FlipCoderabbitReactEnabled(app, true);
                                    LOG_INFO("cmd coderabbit-react.start result ok=%d", static_cast<int>(r.Ok));
                                    return r;
                                });
        c.AsyncSafe = false;
        c.Idempotent = true; // multiple `start` calls converge on enabled=true
        c.Destructive = true;
        reg.Register(std::move(c));
    }

    // ─── coderabbit-react.stop ────────────────────────────────────────
    {
        Command c = MakeCommand("coderabbit-react.stop",
                                "Disable the CodeRabbit react loop. Persists cfg.coderabbit_react.enabled=false and "
                                "restarts the scheduled-poll worker (the next tick observes the disabled flag and "
                                "skips the comment-watcher path).",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    LOG_TRACE("cmd coderabbit-react.stop");
                                    CommandResult r = FlipCoderabbitReactEnabled(app, false);
                                    LOG_INFO("cmd coderabbit-react.stop result ok=%d", static_cast<int>(r.Ok));
                                    return r;
                                });
        c.AsyncSafe = false;
        c.Idempotent = true;
        c.Destructive = true;
        reg.Register(std::move(c));
    }

    // ─── coderabbit-react.status ──────────────────────────────────────
    {
        Command c = MakeCommand(
            "coderabbit-react.status",
            "Print the CodeRabbit react loop's runtime state — config (interval, bot allow-list, "
            "iteration budget, short-circuit-reject), last-tick wall-clock timestamp, and the "
            "watcher's current iteration-budget snapshot.",
            [&app](const nlohmann::json&, const CommandContext&) {
                LOG_TRACE("cmd coderabbit-react.status");
                const TrackerConfig cfg = ConfigManager::Load();
                nlohmann::json data = nlohmann::json::object();
                data["enabled"] = cfg.CoderabbitReact.Enabled;
                data["poll_interval_sec"] = cfg.CoderabbitReact.PollIntervalSec;
                data["watched_base_branches"] = cfg.CoderabbitReact.WatchedBaseBranches;
                data["bot_logins"] = cfg.CoderabbitReact.BotLogins;
                data["short_circuit_reject_enabled"] = cfg.CoderabbitReact.ShortCircuitRejectEnabled;
                data["auto_dispatch_fixes"] = cfg.CoderabbitReact.AutoDispatchFixes;
                data["iteration_budget_per_pr"] = cfg.CoderabbitReact.IterationBudgetPerPr;
                data["ad_hoc_worktree_root"] = cfg.CoderabbitReact.AdHocWorktreeRoot;
                data["last_tick_at_sec"] = app.GetAgenticLastPollAtSec();
                auto* watcher = app.GetAgenticPrCommentWatcher();
                data["watcher_present"] = (watcher != nullptr);
                if (watcher != nullptr) {
                    data["watcher_iteration_budget"] = watcher->GetIterationBudget();
                }
                LOG_INFO("cmd coderabbit-react.status result ok=1 enabled=%d watcher_present=%d",
                         static_cast<int>(cfg.CoderabbitReact.Enabled), static_cast<int>(watcher != nullptr));
                return CommandResult::Success(std::move(data));
            });
        c.AsyncSafe = true;
        c.Idempotent = true;
        c.Destructive = false;
        reg.Register(std::move(c));
    }

    // ─── coderabbit-react.poll-now ────────────────────────────────────
    {
        Command c = MakeCommand(
            "coderabbit-react.poll-now",
            "Fire one tick of the CodeRabbit PR-comment watcher right now (synchronous on the "
            "caller's thread — wrap in a background task from the UI). Useful for manual probes; "
            "respects the watcher's existing classifier + dispatcher wiring, may post replies and "
            "dispatch ad-hoc spawns. `pr_url` is reserved for a future per-PR filter; the phase-8 "
            "watcher walks every row in `agent_open_pr_watch` regardless of this argument.",
            [&app](const nlohmann::json& args, const CommandContext&) {
                const std::string prUrl = args.value("pr_url", std::string());
                LOG_TRACE("cmd coderabbit-react.poll-now pr_url=%s", prUrl.c_str());
                // Lazy-construct the watcher via the handoff-controller entry
                // point so a never-touched session lights up before the tick.
                (void)app.GetAgenticHandoffController();
                auto* watcher = app.GetAgenticPrCommentWatcher();
                if (watcher == nullptr) {
                    LOG_WARN("cmd coderabbit-react.poll-now: PrCommentWatcher not constructed");
                    LOG_INFO("cmd coderabbit-react.poll-now result ok=0 (watcher null)");
                    return CommandResult::Failure(
                        ErrorCode::HandlerError,
                        "PrCommentWatcher not constructed — configure GitHub PAT (cfg.GitHubPat) or "
                        "check the agent-proposals store init.");
                }
                LOG_DEBUG("cmd coderabbit-react.poll-now: invoking watcher->Tick() pr_url=%s", prUrl.c_str());
                const int actioned = watcher->Tick();
                LOG_INFO("coderabbit-react.poll-now: tick actioned=%d (pr_url=%s)", actioned, prUrl.c_str());
                LOG_INFO("cmd coderabbit-react.poll-now result ok=1 actioned=%d", actioned);
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
}

#else // !SMATCHET_WITH_AGENTIC

void RegisterCoderabbitCommands(CommandRegistry& reg, AppController& /*app*/) {
    const char* const kNames[] = {"coderabbit-react.start", "coderabbit-react.stop", "coderabbit-react.status",
                                  "coderabbit-react.poll-now"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        const std::string name = kNames[i];
        Command c =
            MakeCommand(name, "CodeRabbit react loop disabled in this build (SMATCHET_WITH_AGENTIC=OFF).",
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
