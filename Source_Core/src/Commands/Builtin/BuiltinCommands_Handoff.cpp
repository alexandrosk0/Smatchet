// handoff.* — agentic-handoff CLI surface (H4 of the agentic-flow plan).
//
// Five commands:
//   handoff.start    --proposal-id <id> [--dry-run]
//   handoff.cancel   --proposal-id <id>
//   handoff.list
//   handoff.clarify  --proposal-id <id> --answer <text>
//   handoff.dry-run  --proposal-id <id> [--use-stub-claude]
//
// `handoff.dry-run` is a developer convenience that swaps the runner's bin
// path with `stub_claude.exe` for the run. It is the entry point exercised
// by `scripts/dev/test-agentic-handoff-cli.sh` from H4 onward.
//
// In the AGENTIC=OFF build every command short-circuits with a friendly
// failure so the registry surface stays stable across gating states.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "AppController.h"
#include "Logger.h"

#if defined(SMATCHET_WITH_AGENTIC)
#include "AgenticHandoffController.h"
#include "HarnessRunState.h"
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

// Convert an ActiveHandoff to JSON in the canonical shape every handoff.*
// command returns. Keeps the CLI and the H8 UI panel in sync on field names.
nlohmann::json ToJson(const ::smatchet::agentic::AgenticHandoffController::ActiveHandoff& h) {
    nlohmann::json j = nlohmann::json::object();
    j["proposal_id"] = h.proposalId;
    j["worktree"] = h.worktreeDir;
    j["branch"] = h.branchName;
    j["state"] = CodingHarness::RunStateToString(h.state);
    j["pr_url"] = h.prUrl;
    j["last_error"] = h.lastError;
    j["started_at_sec"] = h.startedAtSec;
    return j;
}

// Common precondition: the handoff controller is available. H4 ships
// `GetAgenticHandoffController()` on AppController behind the AGENTIC gate.
::smatchet::agentic::AgenticHandoffController* FetchController(AppController& app, std::string& outError) {
    auto* ctrl = app.GetAgenticHandoffController();
    if (ctrl == nullptr) {
        outError = "AgenticHandoffController not available — configure GitHub PAT (cfg.GitHubPat) "
                   "or check agent-proposals store init.";
    }
    return ctrl;
}

} // namespace

void RegisterHandoffCommands(CommandRegistry& reg, AppController& app) {
    // ─── handoff.start ────────────────────────────────────────────────
    {
        Command c = MakeCommand(
            "handoff.start",
            "Begin an agentic handoff for an approved ImplementIssue proposal. Spawns the configured "
            "coding-harness runner in an isolated worktree (`.claude/worktrees/agent-<id>`) on a "
            "fresh branch (`agent/<id>/<short-slug>`). With --dry-run the command returns the "
            "computed branch / worktree without spawning the runner.",
            [&app](const nlohmann::json& args, const CommandContext&) {
                const long long proposalId =
                    static_cast<long long>(args.value("proposal_id", static_cast<long long>(0)));
                const bool dryRun = args.value("dry_run", false);
                if (proposalId <= 0) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "--proposal-id is required.");
                }
                if (dryRun) {
                    // Resolve the issue key best-effort so the preview reflects
                    // what Start() would compute. On store-read failure we fall
                    // back to a placeholder so the operator at least sees the
                    // branch shape.
                    std::string issueKey = "<unknown>";
                    auto* store = app.GetAgentProposalStore();
                    if (store) {
                        AgentProposal row;
                        std::string lookupErr;
                        if (store->Find(static_cast<std::int64_t>(proposalId), row, lookupErr)) {
                            issueKey = row.issueKey;
                        }
                    }
                    nlohmann::json data = nlohmann::json::object();
                    data["proposal_id"] = proposalId;
                    data["worktree"] = ::smatchet::agentic::AgenticHandoffController::BuildWorktreeDir(
                        static_cast<std::int64_t>(proposalId));
                    data["branch"] = ::smatchet::agentic::AgenticHandoffController::BuildBranchName(
                        static_cast<std::int64_t>(proposalId), issueKey);
                    data["state"] = "Pending";
                    data["dry_run"] = true;
                    return CommandResult::Success(std::move(data));
                }
                std::string err;
                auto* ctrl = FetchController(app, err);
                if (ctrl == nullptr) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                ::smatchet::agentic::AgenticHandoffController::ActiveHandoff h;
                if (!ctrl->Start(static_cast<std::int64_t>(proposalId), h, err)) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                LOG_INFO("handoff.start: proposal_id=%lld state=%s branch=%s", proposalId,
                         CodingHarness::RunStateToString(h.state), h.branchName.c_str());
                return CommandResult::Success(ToJson(h));
            });
        ParamSpec idParam = PInt("proposal_id", "Proposal row id (AgentProposalStore ROWID).", 0);
        idParam.Required = true;
        ParamSpec dryParam;
        dryParam.Name = "dry_run";
        dryParam.Description = "If true, return the computed branch + worktree without spawning the runner.";
        dryParam.Type = ParamType::Bool;
        dryParam.Default = false;
        c.Params = {std::move(idParam), std::move(dryParam)};
        c.AsyncSafe = false;
        c.Idempotent = false;
        reg.Register(std::move(c));
    }

    // ─── handoff.cancel ───────────────────────────────────────────────
    {
        Command c = MakeCommand(
            "handoff.cancel",
            "Request cancellation of an in-flight handoff. Raises the cancel atom; the runner "
            "honours it within a few seconds and the controller transitions to Cancelled.",
            [&app](const nlohmann::json& args, const CommandContext&) {
                const long long proposalId =
                    static_cast<long long>(args.value("proposal_id", static_cast<long long>(0)));
                if (proposalId <= 0) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "--proposal-id is required.");
                }
                std::string err;
                auto* ctrl = FetchController(app, err);
                if (ctrl == nullptr) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                if (!ctrl->Cancel(static_cast<std::int64_t>(proposalId), err)) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                nlohmann::json data;
                data["proposal_id"] = proposalId;
                data["cancel_requested"] = true;
                return CommandResult::Success(std::move(data));
            });
        ParamSpec idParam = PInt("proposal_id", "Proposal row id whose handoff to cancel.", 0);
        idParam.Required = true;
        c.Params = {std::move(idParam)};
        c.AsyncSafe = true;
        c.Idempotent = true;
        reg.Register(std::move(c));
    }

    // ─── handoff.list ─────────────────────────────────────────────────
    {
        Command c = MakeCommand(
            "handoff.list",
            "List in-flight handoffs (snapshot). Returns one row per active record with state, "
            "branch, worktree, and PR URL (when known).",
            [&app](const nlohmann::json&, const CommandContext&) {
                std::string err;
                auto* ctrl = FetchController(app, err);
                if (ctrl == nullptr) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& h : ctrl->SnapshotActive()) {
                    arr.push_back(ToJson(h));
                }
                nlohmann::json data;
                data["handoffs"] = std::move(arr);
                return CommandResult::Success(std::move(data));
            });
        c.AsyncSafe = true;
        c.Idempotent = true;
        reg.Register(std::move(c));
    }

    // ─── handoff.clarify ──────────────────────────────────────────────
    {
        Command c = MakeCommand(
            "handoff.clarify",
            "Answer an AwaitingUser clarification — writes USER_RESPONSE.json into the worktree "
            "and prods the runner. Fails if the handoff is not currently AwaitingUser.",
            [&app](const nlohmann::json& args, const CommandContext&) {
                const long long proposalId =
                    static_cast<long long>(args.value("proposal_id", static_cast<long long>(0)));
                const std::string answer = args.value("answer", std::string());
                if (proposalId <= 0) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "--proposal-id is required.");
                }
                if (answer.empty()) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "--answer is required.");
                }
                std::string err;
                auto* ctrl = FetchController(app, err);
                if (ctrl == nullptr) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                if (!ctrl->ProvideClarification(static_cast<std::int64_t>(proposalId), answer, err)) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                nlohmann::json data;
                data["proposal_id"] = proposalId;
                data["delivered"] = true;
                return CommandResult::Success(std::move(data));
            });
        ParamSpec idParam = PInt("proposal_id", "Proposal row id of the AwaitingUser handoff.", 0);
        idParam.Required = true;
        c.Params = {std::move(idParam),
                    PString("answer", "Free-form text written into USER_RESPONSE.json's `answer` field.", true)};
        c.AsyncSafe = false;
        c.Idempotent = false;
        reg.Register(std::move(c));
    }

    // ─── handoff.dry-run ──────────────────────────────────────────────
    {
        Command c = MakeCommand(
            "handoff.dry-run",
            "Developer convenience — same as handoff.start --dry-run today. Reserved for the "
            "stub-claude end-to-end smoke; future revisions will accept --use-stub-claude to "
            "swap the runner's bin path with the bundled stub.",
            [&app](const nlohmann::json& args, const CommandContext&) {
                const long long proposalId =
                    static_cast<long long>(args.value("proposal_id", static_cast<long long>(0)));
                const bool useStub = args.value("use_stub_claude", false);
                if (proposalId <= 0) {
                    return CommandResult::Failure(ErrorCode::ValidationError, "--proposal-id is required.");
                }
                std::string issueKey = "<unknown>";
                auto* store = app.GetAgentProposalStore();
                if (store) {
                    AgentProposal row;
                    std::string lookupErr;
                    if (store->Find(static_cast<std::int64_t>(proposalId), row, lookupErr)) {
                        issueKey = row.issueKey;
                    }
                }
                nlohmann::json data = nlohmann::json::object();
                data["proposal_id"] = proposalId;
                data["worktree"] = ::smatchet::agentic::AgenticHandoffController::BuildWorktreeDir(
                    static_cast<std::int64_t>(proposalId));
                data["branch"] = ::smatchet::agentic::AgenticHandoffController::BuildBranchName(
                    static_cast<std::int64_t>(proposalId), issueKey);
                data["use_stub_claude"] = useStub;
                data["dry_run"] = true;
                return CommandResult::Success(std::move(data));
            });
        ParamSpec idParam = PInt("proposal_id", "Proposal row id to dry-run.", 0);
        idParam.Required = true;
        ParamSpec stubParam;
        stubParam.Name = "use_stub_claude";
        stubParam.Description = "Reserved — H4 reports the flag back without action.";
        stubParam.Type = ParamType::Bool;
        stubParam.Default = false;
        c.Params = {std::move(idParam), std::move(stubParam)};
        c.AsyncSafe = true;
        c.Idempotent = true;
        reg.Register(std::move(c));
    }
}

#else // !SMATCHET_WITH_AGENTIC

void RegisterHandoffCommands(CommandRegistry& reg, AppController& /*app*/) {
    const char* const kNames[] = {"handoff.start", "handoff.cancel", "handoff.list", "handoff.clarify",
                                  "handoff.dry-run"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        const std::string name = kNames[i];
        Command c = MakeCommand(name, "Agentic handoff disabled in this build (SMATCHET_WITH_AGENTIC=OFF).",
                                [name](const nlohmann::json&, const CommandContext&) {
                                    return CommandResult::Failure(
                                        ErrorCode::HandlerError,
                                        name + ": agentic flow not built (SMATCHET_WITH_AGENTIC=OFF)");
                                });
        c.Idempotent = true;
        reg.Register(std::move(c));
    }
}

#endif // SMATCHET_WITH_AGENTIC

} // namespace cmd
} // namespace smatchet
