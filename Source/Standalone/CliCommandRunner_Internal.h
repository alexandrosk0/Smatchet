#ifndef SMATCHET_TARGET_STANDALONE_CLI_COMMAND_RUNNER_INTERNAL_H
#define SMATCHET_TARGET_STANDALONE_CLI_COMMAND_RUNNER_INTERNAL_H

// Implementation-detail header shared by the CliCommandRunner spine and its companion
// stage TUs (`CliArgs.cpp`, `CliSpawn.cpp`, `CliDispatch.cpp`, `CliHelpAndAttach.cpp`).
// Part of the god-file-splits partition of the former single `CliCommandRunner.cpp`:
// each stage TU is a byte-identical body move; helpers used across the new TU boundary
// were promoted from the old anonymous namespace into `smatchet::cli::detail` and are
// declared here (declaration-only — every function is defined in exactly one TU). Not
// part of the public surface — never include from `CliCommandRunner.h` or any other
// public header; the public entry points stay in `CliCommandRunner.h`.
//
// Defining TU per group:
//   * pure arg/JSON/env/emit helpers (unguarded)      -> CliArgs.cpp
//   * MCP-only spawn primitives                        -> CliSpawn.cpp
//   * MCP-only SpawnAndRun orchestration + extract     -> CliDispatch.cpp
//   * MCP-only RunCmdAttach phase helpers              -> CliHelpAndAttach.cpp
// The non-MCP in-process runner + public entry points stay in the spine and are not
// re-exposed here (they call the promoted helpers via `using namespace detail;`).

#include <string>

#include <nlohmann/json.hpp>

// Only a reference is taken to httplib::Client in one declared overload; forward-declare
// it so this header does not drag <httplib.h> (and its winsock preamble) into every TU.
namespace httplib {
class Client;
}

namespace smatchet {
namespace cli {
namespace detail {

struct ParsedArgs {
    std::string commandName;   ///< first positional after `cmd`
    bool wantHelp = false;     ///< --help / -h after a command name
    bool wantListHelp = false; ///< `cmd --help` with no command name
    bool pretty = false;
    bool quiet = false;
    bool yes = false;
    bool dryRun = false;                            ///< --dry-run → inject __dry_run:true
    bool tokens = false;                            ///< --tokens → estimate output size, print to stderr, no stdout
    bool spawn = false;                             ///< --spawn → launch app subprocess if no instance reachable
    int timeoutMs = 0;                              ///< --timeout=<ms> → passed as __timeout_ms; 0=no cap
    std::string mcpHost;                            ///< empty -> default
    int mcpPort = 0;                                ///< 0 -> resolve from env / default
    nlohmann::json args = nlohmann::json::object(); ///< collected --key=value arguments
};

// Process exit-code map — kept in sync with the plan / CliCommandRunner.h header.
// `constexpr int` at namespace scope has internal linkage, so each including TU gets its
// own copy (no ODR object, no linker symbol) — the SmatchetDefaults.h precedent.
constexpr int kExitOk = 0;
constexpr int kExitUnknownCommand = 2;
constexpr int kExitValidation = 3;
constexpr int kExitHandler = 4;
constexpr int kExitConfirmRequired = 5;
constexpr int kExitNotConnected = 6;
constexpr int kExitTransport = 7;
constexpr int kExitTimeout = 8;
// 9 (dry-run-unsupported) is reachable once dry-run lands.

// -------- pure arg / JSON / env / emit helpers (defined in CliArgs.cpp) ---------------
// Defaults are declared here (visible to every caller); the definitions omit them.
bool SafeBool(const nlohmann::json& j, const char* key, bool fallback);
std::string SafeString(const nlohmann::json& j, const char* key, const std::string& fallback = {});
int SafeInt(const nlohmann::json& j, const char* key, int fallback);
nlohmann::json SafeObject(const nlohmann::json& j, const char* key);
bool SafeParseJson(const std::string& text, nlohmann::json& out);
nlohmann::json MakeErrorEnvelope(const std::string& command, const std::string& code, const std::string& message,
                                 const std::string& hint = {});
int ExitCodeForErrorCode(const std::string& code);
std::string GetExePath();
bool WaitForFile(const std::string& outPath, int timeoutMs);
std::string EnvOr(const char* name, std::string fallback);
int EnvIntOr(const char* name, int fallback);
bool ParseArgs(int argc, char** argv, ParsedArgs& out, std::string& outError);
void EmitEnvelope(const nlohmann::json& envelope, bool pretty, bool quiet);
void EmitErrorToStderr(const nlohmann::json& envelope);

#if defined(SMATCHET_WITH_MCP)
// -------- MCP-only spawn primitives (defined in CliSpawn.cpp) -------------------------
int FindFreePort();
std::string SpawnLogRandomToken();
std::string SpawnAuthToken();

/// Tracks the spawned ephemeral child so the --spawn result wait can tell "child died
/// before writing its result" apart from "child still running at the deadline" — a
/// dead child used to burn the full wait and then be misreported as a timeout with a
/// --timeout hint that cannot fix a crash (test 2026-08-03 teardown-assert entry).
/// Owns the Windows process HANDLE (void* keeps <windows.h> out of this header;
/// released in the destructor); POSIX tracks the fork pid (waitpid(WNOHANG) needs no
/// release). Move-only — copying would double-close the HANDLE.
class SpawnedChild {
  public:
    SpawnedChild() = default;
    ~SpawnedChild();
    SpawnedChild(const SpawnedChild&) = delete;
    SpawnedChild& operator=(const SpawnedChild&) = delete;
    SpawnedChild(SpawnedChild&& other) noexcept;
    SpawnedChild& operator=(SpawnedChild&& other) noexcept;

    void* processHandle = nullptr; ///< Windows HANDLE; nullptr = not tracked
    long long pid = -1;            ///< POSIX child pid; -1 = not tracked
    std::string logPath;           ///< child stdout/stderr capture file ("" = capture unavailable)
};

enum class SpawnedChildStatus {
    Running, ///< child is (or must be presumed) still alive
    Exited,  ///< child has exited; outExitCode is filled (POSIX signal deaths map to 128+sig)
    Unknown  ///< liveness not observable (untracked handle / query failed) — treat as Running
};
SpawnedChildStatus PollSpawnedChild(const SpawnedChild& child, int& outExitCode);

/// Launches the ephemeral --spawn instance. On success fills outChild (process
/// handle/pid + the child's stdout/stderr capture path) when non-null.
bool LaunchEphemeralInstance(const std::string& exePath, int port, SpawnedChild* outChild,
                             const std::string& authToken);
void PostAppQuitBestEffort(httplib::Client& cli);
void PostAppQuitBestEffort(const std::string& host, int port, const std::string& authToken);
std::string SwapOutLogForConfineSafeBasename(nlohmann::json& args);
void RelocateChildOutLog(const std::string& requestedOutLog, const std::string& childResolvedOutLog);

// -------- MCP-only dispatch orchestration (defined in CliDispatch.cpp) ----------------
nlohmann::json ExtractEnvelopeFromMcpResult(const nlohmann::json& body);
int SpawnAndRun(const ParsedArgs& pa, const std::string& commandName, const nlohmann::json& argsToSendRaw);

// -------- MCP-only RunCmdAttach phase helpers (defined in CliHelpAndAttach.cpp) -------
bool RunCmdAttachResolveHostPort(int argc, char** argv, ParsedArgs& outPa, std::string& outHost, int& outPort);
bool RunCmdAttachHandleHelp(const ParsedArgs& pa, const std::string& host, int port, std::string& outToolName,
                            nlohmann::json& outArgs);
int RunCmdAttachDispatch(const ParsedArgs& pa, const std::string& host, int port, const std::string& toolName,
                         const nlohmann::json& argsToSend, nlohmann::json& outEnvelope, bool& outSpawnHandled);
int RunCmdAttachProcessResult(const ParsedArgs& pa, const nlohmann::json& envelope, bool doTokenEstimate);
#endif // SMATCHET_WITH_MCP

} // namespace detail

} // namespace cli
} // namespace smatchet

#endif // SMATCHET_TARGET_STANDALONE_CLI_COMMAND_RUNNER_INTERNAL_H
