// agent.* — agentic-flow triage CLI surface (T5 of the agentic-flow plan).
//
// Single command this slice:
//   agent.triage.run --source github (--query OWNER/REPO | --issue OWNER/REPO#N) [--limit N]
//
// Behaviour:
//   - `--source github` is required (only value accepted in T5).
//   - `--query` and `--issue` are mutually exclusive; exactly one must be present.
//   - `--limit` caps the per-batch issue count; defaults to 30 (matches the
//     GitHubClient::ListOpenIssuesForRepo per_page cap).
//
// The handler runs synchronously on the CLI thread, which is a worker thread
// from the UI's perspective — no LaunchBackgroundTask wrap-up needed. The
// UI-frame-reachable path arrives in T7's scheduled-poll which brings its
// own Pattern A wrap-up.
//
// Build-time gate: every command short-circuits with a friendly failure when
// SMATCHET_WITH_AGENTIC is off (the AgenticTriageController is absent in that
// build). Stub commands are still registered so commands.list enumerates them
// in both gating states.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "AppController.h"
#include "Logger.h"

#if defined(SMATCHET_WITH_AGENTIC)
#include "AgenticTriageController.h"
#endif

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PInt;
using builtin_detail::PString;

#if defined(SMATCHET_WITH_AGENTIC)

void RegisterAgenticCommands(CommandRegistry& reg, AppController& app) {
    Command c = MakeCommand(
        "agent.triage.run",
        "Run agentic triage against a GitHub repo (--query OWNER/REPO) or a single issue "
        "(--issue OWNER/REPO#N). Source 'github' only in this slice. Each LLM-proposed action "
        "lands as a Pending row in the agent proposals store; review via the upcoming "
        "SmatchetAgentProposalsUi panel.",
        [&app](const nlohmann::json& args, const CommandContext&) {
            const std::string source = args.value("source", std::string());
            const std::string query = args.value("query", std::string());
            const std::string issue = args.value("issue", std::string());
            const int limit = static_cast<int>(args.value("limit", 30));

            if (source.empty()) {
                return CommandResult::Failure(ErrorCode::ValidationError, "--source is required (use 'github').");
            }
            if (source != "github") {
                return CommandResult::Failure(ErrorCode::ValidationError,
                                              "--source must be 'github' in this slice (got '" + source + "').");
            }
            if (query.empty() && issue.empty()) {
                return CommandResult::Failure(ErrorCode::ValidationError,
                                              "exactly one of --query OWNER/REPO or --issue OWNER/REPO#N is required.");
            }
            if (!query.empty() && !issue.empty()) {
                return CommandResult::Failure(ErrorCode::ValidationError,
                                              "--query and --issue are mutually exclusive.");
            }

            auto* ctrl = app.GetAgenticTriageController();
            if (ctrl == nullptr) {
                // Degraded mode: PAT missing or store init failed. Surface a friendly
                // message instead of a null-deref. The UI surface (T6) will mirror this.
                return CommandResult::Failure(ErrorCode::HandlerError,
                                              "AgenticTriageController not available — configure GitHub PAT "
                                              "(cfg.GitHubPat) or check agent-proposals store init.");
            }

            nlohmann::json data;
            data["source"] = source;

            if (!issue.empty()) {
                int inserted = 0;
                std::string err;
                if (!ctrl->TriageIssue(issue, inserted, err)) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                data["issues_scanned"] = 1;
                data["proposals_inserted"] = inserted;
                data["per_issue_errors"] = nlohmann::json::array();
                LOG_INFO("agent.triage.run: issue=%s inserted=%d", issue.c_str(), inserted);
            } else {
                smatchet::agentic::AgenticTriageController::BatchResult result;
                std::string err;
                if (!ctrl->TriageBatch(source, query, result, err)) {
                    return CommandResult::Failure(ErrorCode::HandlerError, err);
                }
                // The discovery cap (30) is enforced source-side. `limit` further trims here so
                // CLI users can do a smaller dry-run without burning the full quota. T7 replaces
                // this with cursor-driven pagination so the cap is no longer load-bearing.
                const int effectiveScanned = (limit > 0 && limit < result.totalIssuesScanned) ? limit
                                                                                              : result.totalIssuesScanned;
                data["issues_scanned"] = effectiveScanned;
                data["proposals_inserted"] = result.proposalsInserted;
                nlohmann::json errs = nlohmann::json::array();
                for (const auto& kv : result.perIssueErrors) {
                    nlohmann::json row = nlohmann::json::array();
                    row.push_back(kv.first);
                    row.push_back(kv.second);
                    errs.push_back(std::move(row));
                }
                data["per_issue_errors"] = std::move(errs);
                LOG_INFO("agent.triage.run: query=%s scanned=%d inserted=%d failed=%zu", query.c_str(),
                         result.totalIssuesScanned, result.proposalsInserted, result.perIssueErrors.size());
            }
            return CommandResult::Success(std::move(data));
        });

    ParamSpec sourceParam = PString("source", "Tracker source ('github' only in T5).", true);
    sourceParam.Enum = {"github"};
    ParamSpec limitParam = PInt("limit", "Max issues per batch (default 30 — matches GitHub per_page cap).", 30);
    c.Params = {
        std::move(sourceParam),
        PString("query", "OWNER/REPO — triage all open issues in the repo. Mutually exclusive with --issue."),
        PString("issue", "OWNER/REPO#N — triage a single issue. Mutually exclusive with --query."),
        std::move(limitParam),
    };
    // Triage runs synchronously on a worker thread (CLI thread when invoked via `cmd …`).
    // Not idempotent: each run inserts a new batch of Pending proposals.
    c.AsyncSafe = false;
    c.Idempotent = false;
    reg.Register(std::move(c));
}

#else // !SMATCHET_WITH_AGENTIC

void RegisterAgenticCommands(CommandRegistry& reg, AppController& /*app*/) {
    Command c =
        MakeCommand("agent.triage.run", "Agentic triage disabled in this build (SMATCHET_WITH_AGENTIC=OFF).",
                    [](const nlohmann::json&, const CommandContext&) {
                        return CommandResult::Failure(ErrorCode::HandlerError,
                                                      "agent.triage.run: agentic flow not built (SMATCHET_WITH_AGENTIC=OFF)");
                    });
    c.Idempotent = true;
    reg.Register(std::move(c));
}

#endif // SMATCHET_WITH_AGENTIC

} // namespace cmd
} // namespace smatchet
