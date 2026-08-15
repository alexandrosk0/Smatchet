#include "CliCommandRunner.h"
#include "CliCommandRunner_Internal.h"

#include "CliArgCoercion.h"
#include "StandaloneAppBootstrap.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/SpawnOutLogBasename.h"
#include "ConfigManager.h"
#include "Json/BoundedJsonParse.h"
#include "SmatchetDefaults.h"

#include <nlohmann/json.hpp>
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared CliCommandRunner-TU include block + cli::detail using-prologue is grandfathered across the god-file-split siblings (CliCommandRunner.cpp / CliArgs / CliSpawn / CliDispatch / CliHelpAndAttach) — a behavior-preserving partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared CliCommandRunner TU prologue header is introduced)
// clang-format on

#if defined(SMATCHET_WITH_MCP)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <bcrypt.h> // BCryptGenRandom — spawn-token CSPRNG (backlog 2026-06-28)
#pragma comment(lib, "bcrypt")
#endif

#include <httplib.h>
#endif // SMATCHET_WITH_MCP

#include <ghc/filesystem.hpp>
// SMATCHET_DEVIATION(rule=duplication; reason=shared god-file-split TU prologue clone re-entered the delta scan by an
// include insertion — see the file-top deviation for the full rationale; owner=orchestrator; revisit=when a shared
// CliCommandRunner TU prologue header is introduced)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h> // waitpid(WNOHANG) — PollSpawnedChild liveness probe
// SMATCHET_DEVIATION(rule=duplication; reason=shared god-file-split TU prologue clone re-entered the delta scan by an
// include insertion — see the file-top deviation for the full rationale; owner=orchestrator; revisit=when a shared
// CliCommandRunner TU prologue header is introduced)
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath (CPP_CODE_AUDIT.md #33g — was only transitively included)
#endif
#if defined(__linux__)
#include <cerrno>
#include <sys/random.h> // getrandom — spawn-token CSPRNG (backlog 2026-06-28)
#endif
#endif

namespace fs = ghc::filesystem;

namespace smatchet {
namespace cli {

// Bring the promoted `cli::detail` helpers (declared in CliCommandRunner_Internal.h)
// into scope so the moved bodies below call them exactly as before, unqualified.
using namespace detail;

#if defined(SMATCHET_WITH_MCP)

namespace {

/// Fill `buf` with `n` bytes from the explicit OS CSPRNG — BCryptGenRandom
/// (Windows CNG), arc4random_buf (macOS/BSD), getrandom (Linux). Returns false only when the
/// platform call failed or no platform branch applies; the caller then falls back
/// to std::random_device, which the C++ standard does not guarantee to be
/// crypto-secure or even non-deterministic (backlog 2026-06-28 — the shipped hosts
/// all take an explicit branch, so the fallback never runs there).
bool FillOsCsprng(unsigned char* buf, size_t n) {
#if defined(_WIN32)
    const NTSTATUS status = BCryptGenRandom(nullptr, buf, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(buf, n);
    return true;
#elif defined(__linux__)
    size_t off = 0;
    while (off < n) {
        const ssize_t got = getrandom(buf + off, n - off, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<size_t>(got);
    }
    return true;
#else
    (void)buf;
    (void)n;
    return false;
#endif
}

/// `draws * 8` lowercase hex chars (`draws * 32` bits) from the OS CSPRNG, with a
/// std::random_device fallback only if the platform call fails — never weaker than
/// the previous random_device-only draw. Shared by the spawn-log filename suffix
/// and the per-spawn MCP auth token so the two don't duplicate the RNG /
/// hex-formatting pattern (DRY — single chokepoint).
std::string RandomHexToken(int draws) {
    const size_t byteCount = static_cast<size_t>(draws) * 4u;
    std::vector<unsigned char> bytes(byteCount, 0);
    if (!FillOsCsprng(bytes.data(), byteCount)) {
        std::random_device rd;
        for (size_t i = 0; i < byteCount; i += 4) {
            const unsigned int v = static_cast<unsigned int>(rd());
            std::memcpy(&bytes[i], &v, 4);
        }
    }
    std::string out;
    out.reserve(byteCount * 2);
    char buf[3];
    for (size_t i = 0; i < byteCount; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned int>(bytes[i]));
        out += buf;
    }
    return out;
}

/// Compute a per-spawn log file path so child stdout/stderr survive the
/// parent's --spawn redirection. Format:
/// $TMPDIR/Smatchet-spawn-<pid>-<port>-<rand>.log. The random suffix makes the
/// path unpredictable so it can't be pre-planted (audit #19); the open below
/// pairs it with an exclusive / no-follow create as the actual race guard.
std::string ComputeSpawnLogPath(int port) {
#if defined(_WIN32)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* tmpEnv = std::getenv("TMP");
    if (!tmpEnv)
        tmpEnv = std::getenv("TEMP");
    if (!tmpEnv)
        tmpEnv = "C:\\Windows\\Temp";
    const DWORD parentPid = GetCurrentProcessId();
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
    const char* tmpEnv = std::getenv("TMPDIR");
    if (!tmpEnv)
        tmpEnv = "/tmp";
    const pid_t parentPid = getpid();
#endif
    return std::string(tmpEnv) + "/Smatchet-spawn-" + std::to_string(static_cast<long long>(parentPid)) + "-" +
           std::to_string(port) + "-" + SpawnLogRandomToken() + ".log";
}

} // namespace

namespace detail {

/// Bind to port 0, read back the OS-assigned port, close the socket.
/// Small TOCTOU race is acceptable for ephemeral test scenarios.
int FindFreePort() {
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }
    int len = sizeof(addr);
    if (getsockname(s, reinterpret_cast<struct sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }
    int port = ntohs(addr.sin_port);
    closesocket(s);
    return port;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return 0;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(s);
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(s, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        close(s);
        return 0;
    }
    int port = ntohs(addr.sin_port);
    close(s);
    return port;
#endif
}

/// 16 hex chars (64 bits) of randomness for the spawn-log filename. A predictable
/// `<pid>-<port>` name let any same-user-or-not process pre-plant a file or
/// symlink at the known path that the spawn redirect then wrote through (audit #19).
std::string SpawnLogRandomToken() { return RandomHexToken(2); }

/// 32 hex chars (128 bits) of per-spawn entropy for the ephemeral MCP auth token.
/// The --spawn parent hands this to its child via SMATCHET_MCP_SPAWN_TOKEN so the
/// child (which has no persisted mcp_auth_token) requires + accepts exactly this
/// token under the secure McpRequireTokenOnLoopback default — every other local
/// caller is still denied. Ephemeral, never persisted, cleared from the parent
/// env right after the child is spawned.
std::string SpawnAuthToken() { return RandomHexToken(4); }

SpawnedChild::~SpawnedChild() {
#if defined(_WIN32)
    if (processHandle)
        CloseHandle(static_cast<HANDLE>(processHandle));
#endif
}

SpawnedChild::SpawnedChild(SpawnedChild&& other) noexcept
    : processHandle(other.processHandle), pid(other.pid), logPath(std::move(other.logPath)) {
    other.processHandle = nullptr;
    other.pid = -1;
}

SpawnedChild& SpawnedChild::operator=(SpawnedChild&& other) noexcept {
    if (this != &other) {
#if defined(_WIN32)
        if (processHandle)
            CloseHandle(static_cast<HANDLE>(processHandle));
#endif
        processHandle = other.processHandle;
        pid = other.pid;
        logPath = std::move(other.logPath);
        other.processHandle = nullptr;
        other.pid = -1;
    }
    return *this;
}

SpawnedChildStatus PollSpawnedChild(const SpawnedChild& child, int& outExitCode) {
#if defined(_WIN32)
    if (!child.processHandle)
        return SpawnedChildStatus::Unknown;
    const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(child.processHandle), 0);
    if (wait == WAIT_TIMEOUT)
        return SpawnedChildStatus::Running;
    if (wait != WAIT_OBJECT_0)
        return SpawnedChildStatus::Unknown; // WAIT_FAILED — never misreport a live child as dead
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(child.processHandle), &code))
        return SpawnedChildStatus::Unknown;
    outExitCode = static_cast<int>(code);
    return SpawnedChildStatus::Exited;
#else
    if (child.pid <= 0)
        return SpawnedChildStatus::Unknown;
    int status = 0;
    const pid_t r = waitpid(static_cast<pid_t>(child.pid), &status, WNOHANG);
    if (r == 0)
        return SpawnedChildStatus::Running;
    if (r < 0)
        return SpawnedChildStatus::Unknown; // ECHILD (already reaped) / EINVAL — not observable
    if (WIFEXITED(status)) {
        outExitCode = WEXITSTATUS(status);
        return SpawnedChildStatus::Exited;
    }
    if (WIFSIGNALED(status)) {
        outExitCode = 128 + WTERMSIG(status); // shell convention for signal deaths
        return SpawnedChildStatus::Exited;
    }
    return SpawnedChildStatus::Running; // stopped/continued — the process still exists
#endif
}

bool LaunchEphemeralInstance(const std::string& exePath, int port, SpawnedChild* outChild,
                             const std::string& authToken) {
    if (exePath.empty())
        return false;
    const std::string portStr = std::to_string(port);
    const std::string logPath = ComputeSpawnLogPath(port);
    if (outChild)
        outChild->logPath = logPath;
        // main.cpp parses `--mcp-port <port>` as two separate argv entries (space-separated),
        // NOT `--mcp-port=<port>` — using the equals form would silently fall through.
#if defined(_WIN32)
    // CommandLineToArgvW handles quoted whitespace; pass space-separated tokens.
    std::string cmdLine = "\"" + exePath + "\" --ephemeral --mcp-port " + portStr;
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    // CREATE_NEW fails if the path already exists, so a pre-planted file or
    // symlink at the (now random) path cannot be hijacked (audit #19). The
    // random suffix makes a spurious collision negligible.
    HANDLE hLog =
        CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        // Log capture failure is non-fatal — fall back to inheriting parent
        // handles so the spawn still works, just with no captured diagnostics.
        si.dwFlags &= ~STARTF_USESTDHANDLES;
    } else {
        si.hStdOutput = hLog;
        si.hStdError = hLog;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi = {};
    // M1: hand the per-spawn MCP token to the child via an explicit merged environment
    // block — NEVER by mutating the parent's own global env. The old path called
    // SetEnvironmentVariableA on the parent around CreateProcessA, which (a) raced any
    // concurrent thread reading the parent's env and (b) briefly exposed the secret in
    // the long-lived parent's environment. Here we snapshot the parent's block, strip
    // any inherited SMATCHET_MCP_SPAWN_TOKEN= entry, append our own, and pass it as
    // lpEnvironment so only the child ever sees the token. When there is no token we
    // pass nullptr (plain inherit) — the CLI parent carries no token of its own to leak.
    std::vector<char> envBlock;
    if (!authToken.empty()) {
        char* parentEnv = GetEnvironmentStringsA();
        if (parentEnv) {
            for (const char* p = parentEnv; *p; p += std::strlen(p) + 1) {
                const size_t len = std::strlen(p);
                // Case-INSENSITIVE prefix match: Windows env-var names are case-insensitive
                // (`smatchet_mcp_spawn_token=` and `SMATCHET_MCP_SPAWN_TOKEN=` name the same
                // variable), so a case-mismatched inherited entry must be stripped too — a
                // case-sensitive strncmp would leave a stale/attacker-planted duplicate in the
                // child's block that resolves ahead of / alongside ours (CR security finding).
                if (len >= 25 && _strnicmp(p, "SMATCHET_MCP_SPAWN_TOKEN=", 25) == 0)
                    continue; // drop any inherited token entry — we set our own below
                envBlock.insert(envBlock.end(), p, p + len + 1); // copy entry incl. its NUL
            }
            FreeEnvironmentStringsA(parentEnv);
        }
        const std::string tokenEntry = "SMATCHET_MCP_SPAWN_TOKEN=" + authToken;
        envBlock.insert(envBlock.end(), tokenEntry.begin(), tokenEntry.end());
        envBlock.push_back('\0'); // terminate the token entry
        envBlock.push_back('\0'); // double-NUL terminate the block
    }
    LPVOID lpEnvironment = envBlock.empty() ? nullptr : static_cast<LPVOID>(envBlock.data());
    BOOL ok = CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, TRUE, 0, lpEnvironment, nullptr, &si, &pi);
    if (hLog != INVALID_HANDLE_VALUE) {
        CloseHandle(hLog); // child holds its own dup; parent can release.
    }
    if (ok) {
        // Keep hProcess so the --spawn result wait can poll child liveness (the
        // SpawnedChild dtor releases it); a discarded handle made a crashed child
        // indistinguishable from a slow one. hThread is never queried — release now.
        if (outChild)
            outChild->processHandle = pi.hProcess;
        else
            CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok != FALSE;
#else
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setsid();
        // Redirect stdout + stderr to the log file. open() returns -1 on
        // failure; in that case stdio falls through to whatever the parent had.
        // O_EXCL|O_CREAT refuses to open a path that already exists, and
        // O_NOFOLLOW refuses a symlink at the final component, so a pre-planted
        // file or symlink at the (now random) path can't redirect the write
        // (audit #19 — symlink race). O_NOFOLLOW is POSIX.1-2008; guard it for
        // the rare host that lacks the macro.
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
        int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        // Hand the per-spawn MCP token to the child via its environment. Done in
        // the forked child (before execv) so the parent's own env is never touched.
        if (!authToken.empty())
            setenv("SMATCHET_MCP_SPAWN_TOKEN", authToken.c_str(), 1);
        const char* args[] = {exePath.c_str(), "--ephemeral", "--mcp-port", portStr.c_str(), nullptr};
        execv(exePath.c_str(), const_cast<char* const*>(args));
        _exit(1);
    }
    // Parent: keep the pid so the --spawn result wait can waitpid(WNOHANG) for
    // liveness (setsid detaches the session, not the parent/child relation).
    if (outChild)
        outChild->pid = static_cast<long long>(pid);
    return true;
#endif
}

void PostAppQuitBestEffort(httplib::Client& cli) {
    try {
        nlohmann::json quitBody;
        quitBody["name"] = "app.quit";
        quitBody["arguments"] = {{"__confirm", true}}; // app.quit is Destructive
        cli.Post("/mcp/tools/call", quitBody.dump(), "application/json");
    } catch (...) { // catch-all-ok: spawn teardown must not overwrite the already-emitted command result.
    }
}

void PostAppQuitBestEffort(const std::string& host, int port, const std::string& authToken) {
    if (host.empty() || port <= 0)
        return;
    try {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(1, 0);
        cli.set_read_timeout(1, 0);
        if (!authToken.empty())
            cli.set_default_headers({{"X-Smatchet-Token", authToken}});
        PostAppQuitBestEffort(cli);
    } catch (...) { // catch-all-ok: exception cleanup is best-effort after a spawn failure.
    }
}

/// Swap an ABSOLUTE caller-supplied `outLog` for a confinement-safe basename before the command
/// is forwarded to the spawned child, returning the caller's absolute target for the post-run
/// copy-back. A RELATIVE `outLog` is left untouched (returns empty): develop's forward-raw
/// contract already covers it — the child confines a relative value under <userData>/ui-tests/,
/// which both processes resolve identically without CWD translation. Only an ABSOLUTE outLog is
/// rejected outright by the child's #1566 confinement (ConfinePathUnderSubdir, ui_test.run —
/// absolute / `..` rejected), so only that case needs the trusted --spawn parent to send a
/// confine-safe leaf basename (MakeConfineSafeSpawnOutLogBasename — no separators, entropy-suffixed
/// so concurrent/successive spawns don't collide inside the shared base) and relocate the child's
/// result back to the caller's path afterwards (RelocateChildOutLog). `outPath` and a relative
/// `outLog` are forwarded RAW, unchanged — this COEXISTS with, rather than reverts, develop's
/// forward-raw decision (the relative case CI exercises); it only adds absolute-path support that
/// forward-raw fails closed on.
std::string SwapOutLogForConfineSafeBasename(nlohmann::json& args) {
    if (!args.contains("outLog") || !args["outLog"].is_string())
        return std::string();
    const std::string requested = args["outLog"].get<std::string>();
    if (requested.empty())
        return std::string();

    // Relative outLog: forwarded RAW (develop forward-raw contract — the child confines it under
    // <userData>/ui-tests/). Nothing to swap and nothing to relocate afterwards.
    if (!fs::path(requested).is_absolute())
        return std::string();

    // Absolute outLog: the child's confinement rejects it, so send a confine-safe basename the
    // child accepts and return the caller's (already-absolute) target for the copy-back step.
    args["outLog"] = smatchet::cmd::MakeConfineSafeSpawnOutLogBasename(requested);
    return requested;
}

/// Copy the spawned child's confinement-anchored outLog (the path the child reported in its
/// result JSON, under <userData>/ui-tests/) back to the caller's originally-requested location
/// `requestedOutLog`. This is the relocation half of the basename-swap performed before the
/// command was forwarded (absolute-outLog case only): the child satisfied #1566 by writing inside
/// its confinement base, and the trusted parent now fulfils the caller's absolute target.
/// Best-effort + non-fatal — a missing/identical source only means there is nothing to relocate
/// (e.g. no tests ran, or the caller passed a relative outLog that was forwarded raw).
void RelocateChildOutLog(const std::string& requestedOutLog, const std::string& childResolvedOutLog) {
    if (requestedOutLog.empty() || childResolvedOutLog.empty())
        return;
    std::error_code ec;
    const fs::path src(childResolvedOutLog);
    const fs::path dst(requestedOutLog);
    if (fs::equivalent(src, dst, ec)) // already the same file (e.g. caller targeted the base)
        return;
    ec.clear();
    if (!fs::exists(src, ec) || ec) {
        std::fprintf(stderr, "[spawn] outLog: child wrote no log at %s (nothing to relocate)\n",
                     childResolvedOutLog.c_str()); // CLI stdout — product output, not logging
        return;
    }
    const fs::path parent = dst.parent_path();
    if (!parent.empty()) {
        ec.clear();
        fs::create_directories(parent, ec); // idempotent; copy below surfaces a real failure
    }
    ec.clear();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "[spawn] outLog: could not relocate %s -> %s (%s)\n", childResolvedOutLog.c_str(),
                     requestedOutLog.c_str(), ec.message().c_str()); // CLI stdout — product output, not logging
        return;
    }
    std::fprintf(stderr, "[spawn] outLog: relocated child log -> %s\n",
                 requestedOutLog.c_str()); // CLI stdout — product output, not logging
}

} // namespace detail

#endif // SMATCHET_WITH_MCP

} // namespace cli
} // namespace smatchet
