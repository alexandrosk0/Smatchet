#include "SubprocessCapture.h"

#include "Logger.h"
#include "SubprocessCapturePure.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SubprocessCapture {

namespace {

constexpr int kKillExitCode = 124;
// CPP_CODE_AUDIT.md #26: safety-net timeout applied only when a caller passed
// a zero timeout budget AND no cancel token — the one combination with no way
// to ever interrupt the pump loop (e.g. a p4/git command stuck against a dead
// server hangs the worker thread forever). Callers that supplied a cancel
// token keep their existing unbounded-until-cancelled behavior unchanged. Ten
// minutes covers the slowest legitimate p4/git operations in this codebase's
// call sites while still bounding a truly stuck child.
constexpr int kFallbackTimeoutMsWhenUnguarded = 10 * 60 * 1000;
#ifndef _WIN32
constexpr int kCancelExitCode = 130; // 128 + SIGINT — conventional cancel exit on POSIX (unused on Windows).
#endif

void AppendCapped(std::string& dst, const char* src, size_t count, size_t cap, bool& capped) {
    if (capped || count == 0) {
        return;
    }
    const size_t remaining = (dst.size() < cap) ? (cap - dst.size()) : 0;
    const size_t toAppend = std::min(remaining, count);
    if (toAppend > 0) {
        dst.append(src, toAppend);
    }
    if (toAppend < count || dst.size() >= cap) {
        capped = true;
        static const char* kSuffix = "\n... [capture capped]";
        dst.append(kSuffix);
    }
}

// CPP_CODE_AUDIT.md #31: hard cap on a single accumulated (newline-free) line.
// `AppendCapped`'s byte cap bounds the aggregate `dst` text but not `pending` —
// a child that never emits '\n' would otherwise grow `pending` without bound
// (host OOM) even while `dst` is already capped. 16 MiB matches the default
// `stdoutByteCap` magnitude; no current caller's line-oriented protocol
// (NDJSON/stream-json) legitimately produces a single line anywhere near it.
constexpr size_t kMaxPendingLineBytes = 16u * 1024u * 1024u;

// Newline-terminated line dispatcher. Accumulates inter-chunk bytes in
// `pending` and fires `cb` once per complete line (trailing '\n' stripped).
// CR before LF is also stripped so Windows children that emit "\r\n" deliver
// the same logical line shape as POSIX children. Final unterminated bytes
// stay in `pending` for the FlushLinesEof tail call after the child exits.
void DispatchLines(const char* src, size_t count, std::string& pending,
                   const std::function<void(const std::string&)>& cb) {
    if (!cb || count == 0) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const char c = src[i];
        if (c == '\n') {
            if (!pending.empty() && pending.back() == '\r') {
                pending.pop_back();
            }
            cb(pending);
            pending.clear();
        } else if (pending.size() >= kMaxPendingLineBytes) {
            // Overflow: flush what's accumulated (flagged, not silently dropped) and
            // start fresh rather than growing further. The flushed chunk isn't a real
            // line — callers parsing NDJSON will simply fail to parse it, same as any
            // other malformed line.
            pending.append("...[line capped]");
            cb(pending);
            pending.clear();
            pending.push_back(c); // don't drop the byte that triggered the flush
        } else {
            pending.push_back(c);
        }
    }
}

// At end-of-stream emit any trailing unterminated line so callers that hand
// the runner a child that exits without a final newline still receive every
// JSON object (the stream-json spec emits NDJSON but treat-as-best-effort is
// the safer contract).
void FlushLinesEof(std::string& pending, const std::function<void(const std::string&)>& cb) {
    if (!cb || pending.empty()) {
        pending.clear();
        return;
    }
    if (pending.back() == '\r') {
        pending.pop_back();
    }
    if (!pending.empty()) {
        cb(pending);
    }
    pending.clear();
}

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::wstring ResolveApplicationName(const std::string& argv0) {
    std::wstring wexe = Utf8ToWide(argv0);
    if (wexe.empty()) {
        return std::wstring();
    }
    if (wexe.find(L'\\') != std::wstring::npos || wexe.find(L'/') != std::wstring::npos) {
        return wexe;
    }
    // Resolve a bare exe name to its absolute path so CreateProcessW receives a
    // fully-qualified lpApplicationName rather than a name the loader would
    // re-search against PATH (audit #16 — prefer an absolute trusted path over a
    // bare name). On a resolution miss we fall back to the bare name and warn:
    // CreateProcessW will then do its own PATH search, which is the planting
    // surface the audit flags — surfacing it in the log keeps it observable.
    wchar_t found[MAX_PATH];
    wchar_t* fname = nullptr;
    if (SearchPathW(nullptr, wexe.c_str(), L".exe", MAX_PATH, found, &fname) > 0) {
        return std::wstring(found);
    }
    LOG_WARN("SubprocessCapture: could not resolve \"%s\" to an absolute path via SearchPathW; "
             "falling back to PATH-based launch (binary-planting surface, audit #16)",
             argv0.c_str());
    return wexe;
}

// Build command line: quoted argv0 followed by quoted args. The
// first token must round-trip through CommandLineToArgvW for
// argv[0] inside the child to look right.
std::wstring BuildWindowsCommandLine(const CaptureOptions& opts) {
    std::string cmdUtf8 = SubprocessCapturePure::QuoteArgvWindows(opts.argv0);
    for (size_t i = 0; i < opts.args.size(); ++i) {
        cmdUtf8.push_back(' ');
        cmdUtf8.append(SubprocessCapturePure::QuoteArgvWindows(opts.args[i]));
    }
    return Utf8ToWide(cmdUtf8);
}

std::string LowerAscii(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= 'A' && s[i] <= 'Z') {
            s[i] = static_cast<char>(s[i] + ('a' - 'A'));
        }
    }
    return s;
}

// Pull parent env, drop entries shadowed by opts.env (and, when `scrub` is
// set, any secret-bearing parent var per IsSensitiveEnvName), then append the
// overrides. The Windows env block is case-insensitive at name lookup so dedupe
// on a lowercased key. Returns the merged (parent-minus-shadowed-minus-scrubbed,
// then overrides) entry vector.
std::vector<std::pair<std::string, std::string>>
MergeParentEnvWindows(const std::vector<std::pair<std::string, std::string>>& overrides, bool scrub) {
    std::vector<std::string> overrideKeys;
    overrideKeys.reserve(overrides.size());
    for (size_t i = 0; i < overrides.size(); ++i) {
        overrideKeys.push_back(LowerAscii(overrides[i].first));
    }
    std::vector<std::pair<std::string, std::string>> merged;
    LPWCH parentBlock = GetEnvironmentStringsW();
    if (parentBlock != nullptr) {
        for (LPWCH p = parentBlock; *p != L'\0';) {
            const size_t entryLen = wcslen(p);
            // Convert each NAME=VAL to UTF-8, split on first '='.
            const int u8len =
                WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(entryLen), nullptr, 0, nullptr, nullptr);
            if (u8len > 0) {
                std::string entry(static_cast<size_t>(u8len), '\0');
                WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(entryLen), &entry[0], u8len, nullptr, nullptr);
                const size_t eq = entry.find('=');
                if (eq != std::string::npos && eq > 0) {
                    std::string name = entry.substr(0, eq);
                    std::string value = entry.substr(eq + 1);
                    const std::string lname = LowerAscii(name);
                    const bool shadowed = std::any_of(overrideKeys.begin(), overrideKeys.end(),
                                                      [&lname](const std::string& k) { return k == lname; });
                    if (!shadowed && !(scrub && SubprocessCapturePure::IsSensitiveEnvName(name))) {
                        merged.emplace_back(std::move(name), std::move(value));
                    }
                }
            }
            p += entryLen + 1;
        }
        FreeEnvironmentStringsW(parentBlock);
    }
    for (size_t i = 0; i < overrides.size(); ++i) {
        merged.push_back(overrides[i]);
    }
    return merged;
}

// Build the wide env block for CreateProcessW. With no overrides AND no scrub
// requested, returns an empty string and the caller passes a null
// lpEnvironment, so the child inherits the parent's env. The replaceParentEnv
// flag gives the child ONLY the supplied entries — the allow-list shape.
// Otherwise the supplied entries merge on top of the parent's env, mimicking
// the POSIX additive setenv semantic so P4Annotate-style overrides hold
// cross-platform. When scrubSensitiveEnv is set the parent's secret-bearing
// vars are dropped from that merge per audit #15, and the merge runs even with
// no overrides so the scrubbed block actually replaces the full inheritance.
// UTF-16 env block — CREATE_UNICODE_ENVIRONMENT pairs with this so
// CreateProcessW reads it as wide characters. The pure helper handles
// UTF-8 → UTF-16 conversion so non-ASCII values (Unicode paths,
// locale-translated user dirs) round-trip cleanly. Without UTF-16,
// ANSI interpretation corrupts any byte > 0x7F.
std::wstring BuildWindowsEnvBlock(const CaptureOptions& opts) {
    const bool scrub = opts.scrubSensitiveEnv && !opts.replaceParentEnv;
    if (opts.env.empty() && !scrub) {
        return std::wstring();
    }
    std::vector<std::pair<std::string, std::string>> effectiveEnv =
        opts.replaceParentEnv ? opts.env : MergeParentEnvWindows(opts.env, scrub);
    return SubprocessCapturePure::BuildEnvBlockWindows(effectiveEnv);
}

// Owns the four anonymous-pipe handles plus the NUL stdin handle for a
// Windows child. C-ABI handles closed in the destructor; the spawn path
// releases the write ends explicitly via CloseWriteEnds once the child
// inherits them.
struct WindowsPipes {
    HANDLE rdOut = nullptr;               // C-ABI handle
    HANDLE wrOut = nullptr;               // C-ABI handle
    HANDLE rdErr = nullptr;               // C-ABI handle
    HANDLE wrErr = nullptr;               // C-ABI handle
    HANDLE nullIn = INVALID_HANDLE_VALUE; // C-ABI handle

    WindowsPipes() = default;
    WindowsPipes(const WindowsPipes&) = delete;
    WindowsPipes& operator=(const WindowsPipes&) = delete;

    void CloseWriteEnds() {
        if (wrOut) {
            CloseHandle(wrOut);
            wrOut = nullptr;
        }
        if (wrErr) {
            CloseHandle(wrErr);
            wrErr = nullptr;
        }
    }

    ~WindowsPipes() {
        CloseWriteEnds();
        if (rdOut) {
            CloseHandle(rdOut);
        }
        if (rdErr) {
            CloseHandle(rdErr);
        }
        if (nullIn != INVALID_HANDLE_VALUE) {
            CloseHandle(nullIn);
        }
    }
};

// Create the stdout/stderr pipes and the NUL stdin handle. Returns false
// (with outError set) only on CreatePipe failure; a failed NUL open is
// tolerated (the child inherits the parent stdin handle instead).
bool SetupWindowsPipes(WindowsPipes& pipes, SECURITY_ATTRIBUTES& sa, std::string& outError) {
    if (!CreatePipe(&pipes.rdOut, &pipes.wrOut, &sa, 0) || !CreatePipe(&pipes.rdErr, &pipes.wrErr, &sa, 0)) {
        outError = "CreatePipe failed";
        LOG_ERROR("SubprocessCapture: CreatePipe failed GetLastError=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    SetHandleInformation(pipes.rdOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(pipes.rdErr, HANDLE_FLAG_INHERIT, 0);
    pipes.nullIn =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    return true;
}

// Non-blocking peek-then-read of a single Windows pipe. ReadFile on a
// synchronous anonymous pipe blocks when the child has produced no
// output; PeekNamedPipe bounds each iteration to whatever bytes are
// already buffered so the timeout / cancel checks run on schedule even
// against a silent child. Returns true if any bytes were read.
bool DrainWindowsPipe(HANDLE h, char* buf, size_t bufSize, std::string& dst, size_t cap, bool& capped,
                      std::string* linePending, const std::function<void(const std::string&)>* lineCb) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
        return false;
    }
    DWORD n = 0;
    if (ReadFile(h, buf, static_cast<DWORD>(bufSize), &n, nullptr) && n > 0) {
        AppendCapped(dst, buf, static_cast<size_t>(n), cap, capped);
        if (linePending && lineCb && *lineCb) {
            DispatchLines(buf, static_cast<size_t>(n), *linePending, *lineCb);
        }
        return true;
    }
    return false;
}

// Drain a Windows pipe to exhaustion after the child exited or was
// terminated. The write end is closed (by the child's exit or by
// TerminateProcess closing the process and its handles), so each
// ReadFile returns once the pipe is fully drained.
void DrainWindowsPipeToEof(HANDLE h, char* buf, size_t bufSize, std::string& dst, size_t cap, bool& capped,
                           std::string* linePending, const std::function<void(const std::string&)>* lineCb) {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
            break;
        }
        DWORD n = 0;
        if (!ReadFile(h, buf, static_cast<DWORD>(bufSize), &n, nullptr) || n == 0) {
            break;
        }
        AppendCapped(dst, buf, static_cast<size_t>(n), cap, capped);
        if (linePending && lineCb && *lineCb) {
            DispatchLines(buf, static_cast<size_t>(n), *linePending, *lineCb);
        }
    }
}

// Main IO pump: drain both pipes non-blockingly, then poll process
// exit / cancel-token / timeout each iteration. Sets timedOut /
// cancelled by reference and returns when the child has exited or been
// terminated. `stdoutLinePending` accumulates partial stdout lines for
// the line-dispatcher; stderr stays buffered only.
void PumpWindowsLoop(const CaptureOptions& opts, CaptureResult& out, const WindowsPipes& pipes, HANDLE hProcess,
                     char* buf, size_t bufSize, std::string& stdoutLinePending,
                     std::chrono::steady_clock::time_point startTp, bool& timedOut, bool& cancelled) {
    for (;;) {
        bool sawAnyRead = false;
        sawAnyRead |= DrainWindowsPipe(pipes.rdOut, buf, bufSize, out.stdoutText, opts.stdoutByteCap, out.stdoutCapped,
                                       &stdoutLinePending, &opts.onStdoutLine);
        sawAnyRead |= DrainWindowsPipe(pipes.rdErr, buf, bufSize, out.stderrText, opts.stderrByteCap, out.stderrCapped,
                                       nullptr, nullptr);
        if (WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (opts.cancelToken && opts.cancelToken->load()) {
            cancelled = true;
            LOG_WARN("SubprocessCapture: cancel token flipped; terminating child");
            TerminateProcess(hProcess, kKillExitCode);
            WaitForSingleObject(hProcess, 5000);
            break;
        }
        if (opts.timeoutMs > 0 &&
            SubprocessCapturePure::RemainingTimeoutMs(startTp, static_cast<int64_t>(opts.timeoutMs)) <= 0) {
            timedOut = true;
            LOG_WARN("SubprocessCapture: timeout after %d ms; terminating child", opts.timeoutMs);
            TerminateProcess(hProcess, kKillExitCode);
            WaitForSingleObject(hProcess, 5000);
            break;
        }
        if (!sawAnyRead) {
            Sleep(1);
        }
    }
}

bool RunWindows(const CaptureOptions& opts, CaptureResult& out, std::string& outError) {
    const std::wstring appName = ResolveApplicationName(opts.argv0);
    if (appName.empty()) {
        outError = "empty argv0";
        return false;
    }

    std::wstring cmdLine = BuildWindowsCommandLine(opts);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    WindowsPipes pipes;
    if (!SetupWindowsPipes(pipes, sa, outError)) {
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = pipes.wrOut;
    si.hStdError = pipes.wrErr;
    si.hStdInput = (pipes.nullIn != INVALID_HANDLE_VALUE) ? pipes.nullIn : GetStdHandle(STD_INPUT_HANDLE);

    std::wstring envBlock = BuildWindowsEnvBlock(opts);
    LPVOID envPtr = envBlock.empty() ? nullptr : static_cast<LPVOID>(&envBlock[0]);

    // Working directory. Empty = inherit.
    std::wstring cwdW = Utf8ToWide(opts.cwd);
    LPCWSTR cwdPtr = opts.cwd.empty() ? nullptr : cwdW.c_str();

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');
    // CREATE_UNICODE_ENVIRONMENT — pairs with the UTF-16 env block built
    // above (envPtr points at a wchar_t buffer). Without this flag,
    // CreateProcessW interprets the buffer as ANSI and corrupts every
    // non-ASCII byte in keys or values (Unicode paths, locale-translated
    // user-dirs, GH_TOKEN containing high-bit bytes, etc).
    const BOOL ok = CreateProcessW(appName.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, envPtr, cwdPtr, &si, &pi);
    pipes.CloseWriteEnds();
    if (!ok) {
        const unsigned long err = static_cast<unsigned long>(GetLastError());
        outError = "CreateProcessW failed";
        LOG_ERROR("SubprocessCapture: CreateProcessW failed GetLastError=%lu", err);
        return false;
    }

    char buf[4096];
    bool timedOut = false;
    bool cancelled = false;
    // Per-stream line accumulator — only stdout dispatches lines. stderr stays
    // in the buffered capture for the caller to inspect on completion.
    std::string stdoutLinePending;
    const auto startTp = std::chrono::steady_clock::now();
    PumpWindowsLoop(opts, out, pipes, pi.hProcess, buf, sizeof(buf), stdoutLinePending, startTp, timedOut, cancelled);

    DrainWindowsPipeToEof(pipes.rdOut, buf, sizeof(buf), out.stdoutText, opts.stdoutByteCap, out.stdoutCapped,
                          &stdoutLinePending, &opts.onStdoutLine);
    DrainWindowsPipeToEof(pipes.rdErr, buf, sizeof(buf), out.stderrText, opts.stderrByteCap, out.stderrCapped, nullptr,
                          nullptr);
    FlushLinesEof(stdoutLinePending, opts.onStdoutLine);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    out.exitCode = static_cast<int>(code);
    if (timedOut && out.exitCode == static_cast<int>(STILL_ACTIVE)) {
        out.exitCode = kKillExitCode;
    }
    if (timedOut && out.stderrText.empty()) {
        out.stderrText = "process timed out";
    }
    out.timedOut = timedOut;
    out.cancelled = cancelled;
    const auto endTp = std::chrono::steady_clock::now();
    out.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTp - startTp).count();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else // !_WIN32

// Child-side of the POSIX fork. Redirects stdout and stderr onto the pipe
// write ends, closes inherited descriptors, optionally changes directory,
// applies the env policy, then execs. Never returns on success — it either
// execs or exits. Runs in the forked child only.
//
// CPP_CODE_AUDIT.md #26: this function does non-async-signal-safe work between
// fork() and exec() — the argv std::vector build, string parsing for the env
// scrub allow-list, and setenv/unsetenv/clearenv all potentially call malloc.
// In this multithreaded process, if another thread held the allocator's arena
// lock at the instant of fork(), the child's single surviving thread inherits
// that lock already held (and un-unlockable) — the next allocation here can
// deadlock. A fully correct fix prebuilds argv/envp in the parent (before
// fork()) so the child does only async-signal-safe calls (dup2/close/chdir/
// execve). Accepted as latent risk rather than rewritten here: the deadlock
// window is a narrow race that has not manifested in practice, and rewriting
// this file's env-scrub allow-list / PATH-search semantics carries real
// regression risk for every P4 and Git subprocess call site for a Low-severity
// finding. See docs/plans/shipped/cpp-code-audit-remediation.md § Deviations.
// The safety-net timeout in Run() (CPP_CODE_AUDIT.md #26's other half — a
// zero-timeout caller with no cancel token could hang forever) IS fixed.
//
// Env overrides apply additively in the child via setenv rather than
// rebuilding envp, because execvpe is glibc-only while setenv plus execvp
// works on every POSIX. An empty opts.env inherits the parent's environment
// unchanged, the P4Annotate contract. When replaceParentEnv is set AND env is
// non-empty, the child sees ONLY the supplied entries, the env allow-list
// path. clearenv is POSIX.1-2024 and glibc; on systems without it the additive
// path remains the documented fallback.
void PosixChildExec(const CaptureOptions& opts, int* pipeOutFds, int* pipeErrFds) {
    dup2(pipeOutFds[1], STDOUT_FILENO);
    dup2(pipeErrFds[1], STDERR_FILENO);
    close(pipeOutFds[0]);
    close(pipeOutFds[1]);
    close(pipeErrFds[0]);
    close(pipeErrFds[1]);
    if (!opts.cwd.empty()) {
        if (chdir(opts.cwd.c_str()) != 0) {
            _exit(126);
        }
    }
    std::vector<char*> argv;
    argv.reserve(opts.args.size() + 2);
    argv.push_back(const_cast<char*>(opts.argv0.c_str()));
    for (size_t i = 0; i < opts.args.size(); ++i) {
        argv.push_back(const_cast<char*>(opts.args[i].c_str()));
    }
    argv.push_back(nullptr);
    if (opts.replaceParentEnv && !opts.env.empty()) {
#if defined(__GLIBC__) || (defined(_POSIX_VERSION) && _POSIX_VERSION >= 202405L)
        clearenv();
#else
        // Best-effort manual purge: walk environ and unset every name we
        // can read. Less robust than clearenv() but the runner runs
        // Windows-first today; this path is only exercised under tests.
        // Use ::environ from <unistd.h> rather than a block-scope `extern
        // char** environ;`: under C++14 [basic.link]/6 a block-scope extern
        // inside this anonymous namespace ignores the global declaration and
        // mints a new internal-linkage entity, which then links-fails on
        // Bionic (host glibc takes the clearenv() #if side, hiding it).
        while (::environ && ::environ[0]) {
            const char* eq = std::strchr(::environ[0], '=');
            if (!eq) {
                break;
            }
            // ::environ[0] is char*, eq is const char* — the two-iterator
            // std::string(first, last) ctor needs matching pointer types, which
            // Bionic's libc++ rejects (host glibc takes the clearenv() #if side and
            // never compiles this #else, so the mismatch only surfaces on Android).
            // The (ptr, length) ctor is unambiguous on every stdlib.
            std::string name(::environ[0], static_cast<size_t>(eq - ::environ[0]));
            unsetenv(name.c_str());
        }
#endif
    } else if (opts.scrubSensitiveEnv) {
        // Drop secret-bearing parent vars before the child execs (audit #15).
        // Collect first, then unset: unsetenv() mutates ::environ in place, so
        // unsetting mid-walk would skip neighbours. PATH / HOME / locale / P4*
        // / GIT* survive (IsSensitiveEnvName returns false for them) so the p4,
        // git and file-picker children keep working.
        std::vector<std::string> toDrop;
        for (char** e = ::environ; e && *e; ++e) {
            const char* eq = std::strchr(*e, '=');
            if (!eq) {
                continue;
            }
            std::string name(*e, static_cast<size_t>(eq - *e));
            if (SubprocessCapturePure::IsSensitiveEnvName(name)) {
                toDrop.push_back(std::move(name));
            }
        }
        for (size_t i = 0; i < toDrop.size(); ++i) {
            unsetenv(toDrop[i].c_str());
        }
    }
    for (size_t i = 0; i < opts.env.size(); ++i) {
        setenv(opts.env[i].first.c_str(), opts.env[i].second.c_str(), 1);
    }
    execvp(opts.argv0.c_str(), argv.data());
    _exit(127);
}

void SetFdNonBlocking(int fd) {
    const int old = fcntl(fd, F_GETFL, 0);
    if (old >= 0) {
        fcntl(fd, F_SETFL, old | O_NONBLOCK);
    }
}

// Drain one non-blocking POSIX fd until read() reports EAGAIN /
// EWOULDBLOCK / EOF. stdout dispatches lines; stderr stays buffered.
void DrainPosixFd(int fd, const CaptureOptions& opts, char* buf, size_t bufSize, std::string& dst, size_t cap,
                  bool& capped, std::string& stdoutLinePending, bool isStdout) {
    for (;;) {
        const ssize_t n = read(fd, buf, bufSize);
        if (n <= 0) {
            // EAGAIN / EWOULDBLOCK / EOF — caller decides loop continuation.
            return;
        }
        AppendCapped(dst, buf, static_cast<size_t>(n), cap, capped);
        if (isStdout) {
            DispatchLines(buf, static_cast<size_t>(n), stdoutLinePending, opts.onStdoutLine);
        }
    }
}

// Map a waitpid()-collected status into the exit-code convention:
// normal exit → raw status, signalled → 128 + signum, else -1.
int PosixExitCodeFromStatus(int childStatus) {
    if (WIFEXITED(childStatus)) {
        return WEXITSTATUS(childStatus);
    }
    if (WIFSIGNALED(childStatus)) {
        return 128 + WTERMSIG(childStatus);
    }
    return -1;
}

// Main POSIX IO pump. Polls child exit with WNOHANG, selects both pipe read
// ends with a 200 ms slice, drains readable fds, then checks the cancel token
// and timeout. Sets timedOut, cancelled, and out.exitCode by reference, and
// returns when the child has been reaped or killed.
void PumpPosixLoop(const CaptureOptions& opts, CaptureResult& out, pid_t child, int pipeOutFd, int pipeErrFd, char* buf,
                   size_t bufSize, std::string& stdoutLinePending, std::chrono::steady_clock::time_point startTp,
                   bool& timedOut, bool& cancelled) {
    for (;;) {
        int childStatus = 0;
        const pid_t waitRes = waitpid(child, &childStatus, WNOHANG);
        if (waitRes == child) {
            out.exitCode = PosixExitCodeFromStatus(childStatus);
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(pipeOutFd, &readSet);
        FD_SET(pipeErrFd, &readSet);
        const int nfds = (pipeOutFd > pipeErrFd ? pipeOutFd : pipeErrFd) + 1;
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const int sel = select(nfds, &readSet, nullptr, nullptr, &timeout);
        if (sel > 0) {
            if (FD_ISSET(pipeOutFd, &readSet)) {
                DrainPosixFd(pipeOutFd, opts, buf, bufSize, out.stdoutText, opts.stdoutByteCap, out.stdoutCapped,
                             stdoutLinePending, true);
            }
            if (FD_ISSET(pipeErrFd, &readSet)) {
                DrainPosixFd(pipeErrFd, opts, buf, bufSize, out.stderrText, opts.stderrByteCap, out.stderrCapped,
                             stdoutLinePending, false);
            }
        }

        if (opts.cancelToken && opts.cancelToken->load()) {
            cancelled = true;
            kill(child, SIGKILL);
            int statusAfterKill = 0;
            waitpid(child, &statusAfterKill, 0);
            out.exitCode = kCancelExitCode;
            if (out.stderrText.empty()) {
                out.stderrText = "process cancelled";
            }
            break;
        }

        if (opts.timeoutMs > 0 &&
            SubprocessCapturePure::RemainingTimeoutMs(startTp, static_cast<int64_t>(opts.timeoutMs)) <= 0) {
            timedOut = true;
            kill(child, SIGKILL);
            int statusAfterKill = 0;
            waitpid(child, &statusAfterKill, 0);
            out.exitCode = kKillExitCode;
            if (out.stderrText.empty()) {
                out.stderrText = "process timed out";
            }
            break;
        }
    }
}

bool RunPosix(const CaptureOptions& opts, CaptureResult& out, std::string& outError) {
    // Two separate pipes so stdout and stderr stay distinct on POSIX,
    // mirroring the Windows path. Collapsing both onto one pipe — the prior
    // shape — left out.stderrText permanently empty and made it impossible to
    // distinguish error output from data output, which matters to the
    // gh-pr-create style stderr-on-failure flows.
    int pipeOutFds[2] = {-1, -1};
    int pipeErrFds[2] = {-1, -1};
    if (pipe(pipeOutFds) != 0) {
        outError = std::string("pipe(stdout) failed: ") + std::strerror(errno);
        LOG_ERROR("SubprocessCapture: pipe(stdout) failed errno=%d %s", errno, std::strerror(errno));
        return false;
    }
    if (pipe(pipeErrFds) != 0) {
        outError = std::string("pipe(stderr) failed: ") + std::strerror(errno);
        LOG_ERROR("SubprocessCapture: pipe(stderr) failed errno=%d %s", errno, std::strerror(errno));
        close(pipeOutFds[0]);
        close(pipeOutFds[1]);
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        outError = std::string("fork() failed: ") + std::strerror(errno);
        LOG_ERROR("SubprocessCapture: fork() failed errno=%d %s", errno, std::strerror(errno));
        close(pipeOutFds[0]);
        close(pipeOutFds[1]);
        close(pipeErrFds[0]);
        close(pipeErrFds[1]);
        return false;
    }
    if (child == 0) {
        PosixChildExec(opts, pipeOutFds, pipeErrFds);
    }

    close(pipeOutFds[1]);
    close(pipeErrFds[1]);
    SetFdNonBlocking(pipeOutFds[0]);
    SetFdNonBlocking(pipeErrFds[0]);

    const auto startTp = std::chrono::steady_clock::now();
    bool timedOut = false;
    bool cancelled = false;
    char buf[4096];
    // Line-dispatcher accumulator for opts.onStdoutLine — only stdout
    // dispatches lines; stderr stays buffered. Mirrors the Windows path
    // contract: cross-platform consumers see one shape.
    std::string stdoutLinePending;
    PumpPosixLoop(opts, out, child, pipeOutFds[0], pipeErrFds[0], buf, sizeof(buf), stdoutLinePending, startTp,
                  timedOut, cancelled);

    // Post-exit drain — read any bytes the child wrote between the last
    // select wake and termination. Both pipes are non-blocking so each
    // call returns once drained.
    DrainPosixFd(pipeOutFds[0], opts, buf, sizeof(buf), out.stdoutText, opts.stdoutByteCap, out.stdoutCapped,
                 stdoutLinePending, true);
    DrainPosixFd(pipeErrFds[0], opts, buf, sizeof(buf), out.stderrText, opts.stderrByteCap, out.stderrCapped,
                 stdoutLinePending, false);
    FlushLinesEof(stdoutLinePending, opts.onStdoutLine);

    close(pipeOutFds[0]);
    close(pipeErrFds[0]);
    out.timedOut = timedOut;
    out.cancelled = cancelled;
    const auto endTp = std::chrono::steady_clock::now();
    out.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTp - startTp).count();
    return true;
}

#endif

} // namespace

bool Run(const CaptureOptions& opts, CaptureResult& out, std::string& outError) {
    out = CaptureResult();
    outError.clear();
    if (opts.argv0.empty()) {
        outError = "empty argv0";
        return false;
    }
    // CPP_CODE_AUDIT.md #26: a zero timeout budget with no cancel token can never
    // be interrupted; apply the safety-net fallback for that one combination only
    // (see the constant's doc comment).
    if (opts.timeoutMs == 0 && !opts.cancelToken) {
        CaptureOptions guarded = opts;
        guarded.timeoutMs = kFallbackTimeoutMsWhenUnguarded;
#ifdef _WIN32
        return RunWindows(guarded, out, outError);
#else
        return RunPosix(guarded, out, outError);
#endif
    }
#ifdef _WIN32
    return RunWindows(opts, out, outError);
#else
    return RunPosix(opts, out, outError);
#endif
}

} // namespace SubprocessCapture
