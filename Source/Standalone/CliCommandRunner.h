#ifndef SMATCHET_TARGET_STANDALONE_CLI_COMMAND_RUNNER_H
#define SMATCHET_TARGET_STANDALONE_CLI_COMMAND_RUNNER_H

// Standalone CLI front-end for the unified Command System.
// When Smatchet.exe is invoked with a `cmd <name>` subcommand, this
// runner short-circuits the GUI boot and talks to a running Smatchet instance
// over MCP HTTP (default discovery: SMATCHET_MCP_HOST/PORT env, then default
// port). The full GUI / windowed paths are taken only in normal (non-`cmd`)
// invocations.
// Exit codes (stable contract — see docs/plans/active/applied/command-system-plan.md §CLI):
//   0  ok
//   2  unknown-command
//   3  missing-required-arg / validation-error
//   4  handler-error / backend-error / not-found
//   5  confirm-required (destructive without --yes)
//   6  not-connected (no running instance)
//   7  transport error
//   8  timeout (spawn-mode wait exceeded)
//   9  dry-run-unsupported

namespace smatchet {
namespace cli {

/// Returns true if argv contains a `cmd` token — used by `main.cpp` to skip the GUI boot.
bool ArgvHasCmdSubcommand(int argc, char** argv);

/// Returns true if argv contains `--ephemeral` — spawned instances pass this to suppress
/// the window and signal they were launched by the CLI for automated scenario/testing use.
bool IsEphemeralMode(int argc, char** argv);

/// Run the `cmd` subcommand. When `SMATCHET_WITH_MCP=ON`, attaches via MCP HTTP (+ optional
/// `--spawn`). When OFF (light builds), boots in-process and dispatches via CommandRegistry.
/// Returns the process exit code. All stdout/stderr is written by this function.
int RunCmdAttach(int argc, char** argv);

}  // namespace cli
}  // namespace smatchet

#endif  // SMATCHET_TARGET_STANDALONE_CLI_COMMAND_RUNNER_H
