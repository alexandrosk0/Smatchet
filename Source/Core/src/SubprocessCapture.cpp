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
constexpr int kCancelExitCode = 130; // 128 + SIGINT — conventional cancel exit on POSIX.

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
    wchar_t found[MAX_PATH];
    wchar_t* fname = nullptr;
    if (SearchPathW(nullptr, wexe.c_str(), L".exe", MAX_PATH, found, &fname) > 0) {
        return std::wstring(found);
    }
    return wexe;
}

bool RunWindows(const CaptureOptions& opts, CaptureResult& out, std::string& outError) {
    const std::wstring appName = ResolveApplicationName(opts.argv0);
    if (appName.empty()) {
        outError = "empty argv0";
        return false;
    }

    // Build command line: quoted argv0 followed by quoted args. The
    // first token must round-trip through CommandLineToArgvW for
    // argv[0] inside the child to look right.
    std::string cmdUtf8;
    cmdUtf8 = SubprocessCapturePure::QuoteArgvWindows(opts.argv0);
    for (size_t i = 0; i < opts.args.size(); ++i) {
        cmdUtf8.push_back(' ');
        cmdUtf8.append(SubprocessCapturePure::QuoteArgvWindows(opts.args[i]));
    }
    std::wstring cmdLine = Utf8ToWide(cmdUtf8);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rdOut = nullptr;
    HANDLE wrOut = nullptr;
    HANDLE rdErr = nullptr;
    HANDLE wrErr = nullptr;
    if (!CreatePipe(&rdOut, &wrOut, &sa, 0) || !CreatePipe(&rdErr, &wrErr, &sa, 0)) {
        outError = "CreatePipe failed";
        LOG_ERROR("SubprocessCapture: CreatePipe failed GetLastError=%lu", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    SetHandleInformation(rdOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(rdErr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wrOut;
    si.hStdError = wrErr;
    HANDLE hNullInput =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = (hNullInput != INVALID_HANDLE_VALUE) ? hNullInput : GetStdHandle(STD_INPUT_HANDLE);

    // Env block. Empty options.env → inherit parent (envPtr=nullptr). Non-empty
    // env with replaceParentEnv=true → child sees ONLY the supplied entries
    // (the agentic-flow allow-list shape). Non-empty env with
    // replaceParentEnv=false → merge supplied entries on top of the parent's
    // env (mimic POSIX additive `setenv` semantic so the H1 contract holds
    // cross-platform for P4Annotate-style overrides).
    // UTF-16 env block — CREATE_UNICODE_ENVIRONMENT below tells CreateProcessW
    // to read this as wide characters. The pure helper handles UTF-8 → UTF-16
    // conversion so non-ASCII values (Unicode paths, locale-translated user
    // dirs) round-trip cleanly. Without UTF-16, ANSI interpretation corrupts
    // any byte > 0x7F.
    std::wstring envBlock;
    LPVOID envPtr = nullptr;
    if (!opts.env.empty()) {
        std::vector<std::pair<std::string, std::string>> effectiveEnv = opts.env;
        if (!opts.replaceParentEnv) {
            // Pull parent env, drop entries shadowed by opts.env, then prepend.
            // The Windows env block is case-insensitive at name lookup so
            // dedupe on a lowercased key.
            auto toLower = [](std::string s) {
                for (size_t i = 0; i < s.size(); ++i) {
                    if (s[i] >= 'A' && s[i] <= 'Z') {
                        s[i] = static_cast<char>(s[i] + ('a' - 'A'));
                    }
                }
                return s;
            };
            std::vector<std::string> overrideKeys;
            overrideKeys.reserve(opts.env.size());
            for (size_t i = 0; i < opts.env.size(); ++i) {
                overrideKeys.push_back(toLower(opts.env[i].first));
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
                        WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(entryLen), &entry[0], u8len, nullptr,
                                            nullptr);
                        const size_t eq = entry.find('=');
                        if (eq != std::string::npos && eq > 0) {
                            std::string name = entry.substr(0, eq);
                            std::string value = entry.substr(eq + 1);
                            const std::string lname = toLower(name);
                            const bool shadowed = std::any_of(overrideKeys.begin(), overrideKeys.end(),
                                                              [&lname](const std::string& k) { return k == lname; });
                            if (!shadowed) {
                                merged.emplace_back(std::move(name), std::move(value));
                            }
                        }
                    }
                    p += entryLen + 1;
                }
                FreeEnvironmentStringsW(parentBlock);
            }
            for (size_t i = 0; i < opts.env.size(); ++i) {
                merged.push_back(opts.env[i]);
            }
            effectiveEnv = std::move(merged);
        }
        envBlock = SubprocessCapturePure::BuildEnvBlockWindows(effectiveEnv);
        envPtr = static_cast<LPVOID>(&envBlock[0]);
    }

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
    CloseHandle(wrOut);
    CloseHandle(wrErr);
    if (!ok) {
        const unsigned long err = static_cast<unsigned long>(GetLastError());
        outError = "CreateProcessW failed";
        LOG_ERROR("SubprocessCapture: CreateProcessW failed GetLastError=%lu", err);
        if (hNullInput != INVALID_HANDLE_VALUE) {
            CloseHandle(hNullInput);
        }
        CloseHandle(rdOut);
        CloseHandle(rdErr);
        return false;
    }

    // Drain both pipes non-blockingly. ReadFile on a synchronous
    // anonymous pipe blocks when the child has produced no output;
    // PeekNamedPipe lets us bound each iteration to whatever bytes
    // are already buffered so the timeout / cancel checks run on
    // schedule even against a silent child. The original P4-only
    // implementation got away with the blocking read because p4
    // always writes intermediate banner output, but a generic
    // runner must not depend on that.
    char buf[4096];
    DWORD n = 0;
    bool timedOut = false;
    bool cancelled = false;
    bool sawAnyRead = false;
    // Per-stream line accumulator — only stdout dispatches lines. stderr stays
    // in the buffered capture for the caller to inspect on completion.
    std::string stdoutLinePending;
    const auto startTp = std::chrono::steady_clock::now();
    auto drain = [&buf, &n, &sawAnyRead](HANDLE h, std::string& dst, size_t cap, bool& capped, std::string* linePending,
                                         const std::function<void(const std::string&)>* lineCb) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
            return;
        }
        if (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            AppendCapped(dst, buf, static_cast<size_t>(n), cap, capped);
            if (linePending && lineCb && *lineCb) {
                DispatchLines(buf, static_cast<size_t>(n), *linePending, *lineCb);
            }
            sawAnyRead = true;
        }
    };
    for (;;) {
        sawAnyRead = false;
        drain(rdOut, out.stdoutText, opts.stdoutByteCap, out.stdoutCapped, &stdoutLinePending, &opts.onStdoutLine);
        drain(rdErr, out.stderrText, opts.stderrByteCap, out.stderrCapped, nullptr, nullptr);
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (opts.cancelToken && opts.cancelToken->load()) {
            cancelled = true;
            LOG_WARN("SubprocessCapture: cancel token flipped; terminating child");
            TerminateProcess(pi.hProcess, kKillExitCode);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        if (opts.timeoutMs > 0) {
            const int64_t remaining =
                SubprocessCapturePure::RemainingTimeoutMs(startTp, static_cast<int64_t>(opts.timeoutMs));
            if (remaining <= 0) {
                timedOut = true;
                LOG_WARN("SubprocessCapture: timeout after %d ms; terminating child", opts.timeoutMs);
                TerminateProcess(pi.hProcess, kKillExitCode);
                WaitForSingleObject(pi.hProcess, 5000);
                break;
            }
        }
        if (!sawAnyRead) {
            Sleep(1);
        }
    }
    // Drain remaining buffered output after the child exited or was
    // terminated. ReadFile here is bounded — the write end is closed
    // (either by the child's exit or by TerminateProcess closing the
    // process and its handles), so each ReadFile returns once the
    // pipe is fully drained.
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rdOut, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
            break;
        }
        if (!ReadFile(rdOut, buf, sizeof(buf), &n, nullptr) || n == 0) {
            break;
        }
        AppendCapped(out.stdoutText, buf, static_cast<size_t>(n), opts.stdoutByteCap, out.stdoutCapped);
        DispatchLines(buf, static_cast<size_t>(n), stdoutLinePending, opts.onStdoutLine);
    }
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rdErr, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
            break;
        }
        if (!ReadFile(rdErr, buf, sizeof(buf), &n, nullptr) || n == 0) {
            break;
        }
        AppendCapped(out.stderrText, buf, static_cast<size_t>(n), opts.stderrByteCap, out.stderrCapped);
    }
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
    if (hNullInput != INVALID_HANDLE_VALUE) {
        CloseHandle(hNullInput);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rdOut);
    CloseHandle(rdErr);
    return true;
}

#else // !_WIN32

bool RunPosix(const CaptureOptions& opts, CaptureResult& out, std::string& outError) {
    // Two separate pipes so stdout and stderr stay distinct on POSIX —
    // mirrors the Windows path. Collapsing both onto one pipe (the prior
    // shape) left out.stderrText permanently empty and made it impossible
    // for callers to distinguish error output from data output (matters
    // for `gh pr create`-style stderr-on-failure flows).
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
        // Apply env overrides additively in the child via setenv() rather
        // than rebuilding envp — execvpe() is glibc-only, while setenv() +
        // execvp() works on every POSIX. Empty opts.env means "inherit
        // parent's environment unchanged" (the P4Annotate contract).
        // When replaceParentEnv is set AND env is non-empty, the child sees
        // ONLY the supplied entries — the agentic-flow allow-list path
        // (decision #7). clearenv() is POSIX.1-2024 / glibc; on systems
        // without it the additive path remains the documented fallback.
        if (opts.replaceParentEnv && !opts.env.empty()) {
#if defined(__GLIBC__) || (defined(_POSIX_VERSION) && _POSIX_VERSION >= 202405L)
            clearenv();
#else
            // Best-effort manual purge: walk environ and unset every name we
            // can read. Less robust than clearenv() but the runner runs
            // Windows-first today; this path is only exercised under tests.
            extern char** environ;
            while (environ && environ[0]) {
                const char* eq = std::strchr(environ[0], '=');
                if (!eq) {
                    break;
                }
                std::string name(environ[0], eq);
                unsetenv(name.c_str());
            }
#endif
        }
        for (size_t i = 0; i < opts.env.size(); ++i) {
            setenv(opts.env[i].first.c_str(), opts.env[i].second.c_str(), 1);
        }
        execvp(opts.argv0.c_str(), argv.data());
        _exit(127);
    }

    close(pipeOutFds[1]);
    close(pipeErrFds[1]);
    {
        const int oldOut = fcntl(pipeOutFds[0], F_GETFL, 0);
        if (oldOut >= 0) {
            fcntl(pipeOutFds[0], F_SETFL, oldOut | O_NONBLOCK);
        }
        const int oldErr = fcntl(pipeErrFds[0], F_GETFL, 0);
        if (oldErr >= 0) {
            fcntl(pipeErrFds[0], F_SETFL, oldErr | O_NONBLOCK);
        }
    }

    const auto startTp = std::chrono::steady_clock::now();
    bool timedOut = false;
    bool cancelled = false;
    char buf[4096];
    // Line-dispatcher accumulator for opts.onStdoutLine — only stdout
    // dispatches lines; stderr stays buffered. Mirrors the Windows path
    // contract: cross-platform consumers see one shape.
    std::string stdoutLinePending;
    auto drainFd = [&buf, &out, &opts, &stdoutLinePending](int fd, std::string& dst, size_t cap, bool& capped,
                                                           bool isStdout) {
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                // EAGAIN / EWOULDBLOCK / EOF — caller decides loop continuation.
                return;
            }
            AppendCapped(dst, buf, static_cast<size_t>(n), cap, capped);
            if (isStdout) {
                DispatchLines(buf, static_cast<size_t>(n), stdoutLinePending, opts.onStdoutLine);
            }
        }
    };
    for (;;) {
        int childStatus = 0;
        const pid_t waitRes = waitpid(child, &childStatus, WNOHANG);
        if (waitRes == child) {
            if (WIFEXITED(childStatus)) {
                out.exitCode = WEXITSTATUS(childStatus);
            } else if (WIFSIGNALED(childStatus)) {
                out.exitCode = 128 + WTERMSIG(childStatus);
            } else {
                out.exitCode = -1;
            }
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(pipeOutFds[0], &readSet);
        FD_SET(pipeErrFds[0], &readSet);
        const int nfds = (pipeOutFds[0] > pipeErrFds[0] ? pipeOutFds[0] : pipeErrFds[0]) + 1;
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const int sel = select(nfds, &readSet, nullptr, nullptr, &timeout);
        if (sel > 0) {
            if (FD_ISSET(pipeOutFds[0], &readSet)) {
                drainFd(pipeOutFds[0], out.stdoutText, opts.stdoutByteCap, out.stdoutCapped, true);
            }
            if (FD_ISSET(pipeErrFds[0], &readSet)) {
                drainFd(pipeErrFds[0], out.stderrText, opts.stderrByteCap, out.stderrCapped, false);
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

        if (opts.timeoutMs > 0) {
            const int64_t remaining =
                SubprocessCapturePure::RemainingTimeoutMs(startTp, static_cast<int64_t>(opts.timeoutMs));
            if (remaining <= 0) {
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

    // Post-exit drain — read any bytes the child wrote between the last
    // select wake and termination. Both pipes are non-blocking so each
    // call returns once drained.
    drainFd(pipeOutFds[0], out.stdoutText, opts.stdoutByteCap, out.stdoutCapped, true);
    drainFd(pipeErrFds[0], out.stderrText, opts.stderrByteCap, out.stderrCapped, false);
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
#ifdef _WIN32
    return RunWindows(opts, out, outError);
#else
    return RunPosix(opts, out, outError);
#endif
}

} // namespace SubprocessCapture
