#include "P4Blame.h"
#include "Logger.h"
#include "P4BlameParse.h"
#include "P4ErrorUtil.h"
#include "StringUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <regex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#endif

namespace {

constexpr size_t kP4LogMaxStderr = 2048;
constexpr size_t kP4LogMaxStdoutTrace = 8192;
constexpr DWORD kP4ProcessTimeoutMs = 120000;
constexpr size_t kP4CaptureBytesMax = 4u * 1024u * 1024u;

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

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return std::string();
    }
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

/** Wrap for CreateProcessW command line: escape embedded quotes. */
std::wstring QuoteArg(const std::wstring& arg) {
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') {
            out += L"\\\"";
        } else {
            out += c;
        }
    }
    out += L'"';
    return out;
}

bool RunProcessCapture(const std::wstring& applicationName, const std::wstring& commandLine, int& outExit,
                       std::string& outStdout, std::string& outStderr) {
    outExit = -1;
    outStdout.clear();
    outStderr.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rdOut = nullptr;
    HANDLE wrOut = nullptr;
    HANDLE rdErr = nullptr;
    HANDLE wrErr = nullptr;
    if (!CreatePipe(&rdOut, &wrOut, &sa, 0) || !CreatePipe(&rdErr, &wrErr, &sa, 0)) {
        LOG_ERROR("P4 RunProcessCapture: CreatePipe failed GetLastError=%lu",
                  static_cast<unsigned long>(GetLastError()));
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

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    const BOOL ok = CreateProcessW(applicationName.empty() ? nullptr : applicationName.c_str(), mutableCmd.data(),
                                   nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wrOut);
    CloseHandle(wrErr);
    if (!ok) {
        LOG_ERROR("P4 RunProcessCapture: CreateProcessW failed GetLastError=%lu",
                  static_cast<unsigned long>(GetLastError()));
        if (hNullInput != INVALID_HANDLE_VALUE) {
            CloseHandle(hNullInput);
        }
        CloseHandle(rdOut);
        CloseHandle(rdErr);
        return false;
    }

    char buf[4096];
    DWORD n = 0;
    bool outCapped = false;
    bool errCapped = false;
    bool timedOut = false;
    auto appendCapped = [](std::string& dst, const char* src, size_t count, bool& capped) {
        if (capped || count == 0) {
            return;
        }
        const size_t remaining = (dst.size() < kP4CaptureBytesMax) ? (kP4CaptureBytesMax - dst.size()) : 0;
        const size_t toAppend = std::min(remaining, count);
        if (toAppend > 0) {
            dst.append(src, toAppend);
        }
        if (toAppend < count || dst.size() >= kP4CaptureBytesMax) {
            capped = true;
            static const char* kSuffix = "\n... [capture capped]";
            dst.append(kSuffix);
        }
    };
    const DWORD startedAt = GetTickCount();
    for (;;) {
        n = 0;
        if (ReadFile(rdOut, buf, sizeof(buf), &n, nullptr) && n > 0) {
            appendCapped(outStdout, buf, static_cast<size_t>(n), outCapped);
        }
        n = 0;
        if (ReadFile(rdErr, buf, sizeof(buf), &n, nullptr) && n > 0) {
            appendCapped(outStderr, buf, static_cast<size_t>(n), errCapped);
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        const DWORD elapsed = GetTickCount() - startedAt;
        if (elapsed > kP4ProcessTimeoutMs) {
            timedOut = true;
            LOG_WARN("P4 RunProcessCapture: process timeout after %lu ms; terminating child",
                     static_cast<unsigned long>(elapsed));
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        if (n == 0) {
            Sleep(1);
        }
    }
    while (ReadFile(rdOut, buf, sizeof(buf), &n, nullptr) && n > 0) {
        appendCapped(outStdout, buf, static_cast<size_t>(n), outCapped);
    }
    while (ReadFile(rdErr, buf, sizeof(buf), &n, nullptr) && n > 0) {
        appendCapped(outStderr, buf, static_cast<size_t>(n), errCapped);
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    outExit = static_cast<int>(code);
    if (timedOut && outExit == STILL_ACTIVE) {
        outExit = 124;
    }
    if (timedOut && outStderr.empty()) {
        outStderr = "p4 process timed out";
    }
    if (outCapped || errCapped) {
        LOG_WARN("P4 RunProcessCapture: output capture capped at %zu bytes per stream", kP4CaptureBytesMax);
    }
    if (hNullInput != INVALID_HANDLE_VALUE) {
        CloseHandle(hNullInput);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rdOut);
    CloseHandle(rdErr);
    return true;
}
#endif

#if !defined(_WIN32)
bool RunProcessCapturePosix(const std::string& exe, const std::vector<std::string>& args, int& outExit,
                            std::string& outStdout, std::string& outStderr) {
    outExit = -1;
    outStdout.clear();
    outStderr.clear();
    int pipeFds[2] = {-1, -1};
    if (pipe(pipeFds) != 0) {
        LOG_ERROR("P4: pipe() failed errno=%d %s", errno, std::strerror(errno));
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        LOG_ERROR("P4: fork() failed errno=%d %s", errno, std::strerror(errno));
        close(pipeFds[0]);
        close(pipeFds[1]);
        return false;
    }
    if (child == 0) {
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[0]);
        close(pipeFds[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(exe.c_str()));
        std::transform(args.begin(), args.end(), std::back_inserter(argv), [](const std::string& a) {
            return const_cast<char*>(a.c_str());
        });
        argv.push_back(nullptr);
        execvp(exe.c_str(), argv.data());
        _exit(127);
    }

    close(pipeFds[1]);
    const int oldFlags = fcntl(pipeFds[0], F_GETFL, 0);
    if (oldFlags >= 0) {
        fcntl(pipeFds[0], F_SETFL, oldFlags | O_NONBLOCK);
    }

    const auto start = std::chrono::steady_clock::now();
    bool timedOut = false;
    bool outCapped = false;
    char buf[4096];
    for (;;) {
        int childStatus = 0;
        const pid_t waitRes = waitpid(child, &childStatus, WNOHANG);
        if (waitRes == child) {
            if (WIFEXITED(childStatus)) {
                outExit = WEXITSTATUS(childStatus);
            } else if (WIFSIGNALED(childStatus)) {
                outExit = 128 + WTERMSIG(childStatus);
            } else {
                outExit = -1;
            }
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(pipeFds[0], &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const int sel = select(pipeFds[0] + 1, &readSet, nullptr, nullptr, &timeout);
        if (sel > 0 && FD_ISSET(pipeFds[0], &readSet)) {
            const ssize_t n = read(pipeFds[0], buf, sizeof(buf));
            if (n > 0) {
                if (!outCapped) {
                    const size_t remaining =
                        (outStdout.size() < kP4CaptureBytesMax) ? (kP4CaptureBytesMax - outStdout.size()) : 0;
                    const size_t toAppend = std::min(remaining, static_cast<size_t>(n));
                    if (toAppend > 0) {
                        outStdout.append(buf, toAppend);
                    }
                    if (toAppend < static_cast<size_t>(n) || outStdout.size() >= kP4CaptureBytesMax) {
                        outCapped = true;
                        outStdout += "\n... [capture capped]";
                    }
                }
            }
        }

        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsedMs > static_cast<long long>(kP4ProcessTimeoutMs)) {
            timedOut = true;
            kill(child, SIGKILL);
            int childStatusAfterKill = 0;
            waitpid(child, &childStatusAfterKill, 0);
            outExit = 124;
            outStderr = "p4 process timed out";
            break;
        }
    }

    for (;;) {
        const ssize_t n = read(pipeFds[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (!outCapped) {
            const size_t remaining =
                (outStdout.size() < kP4CaptureBytesMax) ? (kP4CaptureBytesMax - outStdout.size()) : 0;
            const size_t toAppend = std::min(remaining, static_cast<size_t>(n));
            if (toAppend > 0) {
                outStdout.append(buf, toAppend);
            }
            if (toAppend < static_cast<size_t>(n) || outStdout.size() >= kP4CaptureBytesMax) {
                outCapped = true;
                outStdout += "\n... [capture capped]";
            }
        }
    }

    close(pipeFds[0]);
    if (timedOut && outStderr.empty()) {
        outStderr = "p4 process timed out";
    }
    if (outCapped) {
        LOG_WARN("P4: stdout capture capped at %zu bytes on posix path", kP4CaptureBytesMax);
    }
    return true;
}
#endif

} // namespace

using P4BlameParse::ParseAnnotateTextLine;
using P4BlameParse::ParseLatestChangeFromChangesOutput;
using P4BlameParse::SplitLines;
using P4BlameParse::StripP4UserDomain;

bool P4RunCommand(const BlameAnalysisConfig& cfg, const std::vector<std::string>& args, int& outExitCode,
                  std::string& outStdout, std::string& outStderr) {
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();
#ifdef _WIN32
    const std::string exe = cfg.P4Executable.empty() ? "p4" : cfg.P4Executable;
    std::wstring wexe = Utf8ToWide(exe);
    wchar_t found[MAX_PATH];
    wchar_t* fname = nullptr;
    std::wstring appName;
    if (wexe.find(L'\\') != std::wstring::npos || wexe.find(L'/') != std::wstring::npos) {
        appName = wexe;
    } else if (SearchPathW(nullptr, wexe.c_str(), L".exe", MAX_PATH, found, &fname) > 0) {
        appName.assign(found);
    } else {
        appName = wexe;
    }

    const std::string exeLogged = WideToUtf8(appName);
    LOG_INFO("P4: spawn exe=\"%s\" args: %s", exeLogged.c_str(), JoinStrings(args, " ").c_str());

    std::wstring cmd;
    cmd += QuoteArg(appName);
    for (const std::string& a : args) {
        cmd += L' ';
        cmd += QuoteArg(Utf8ToWide(a));
    }
    const bool ran = RunProcessCapture(appName, cmd, outExitCode, outStdout, outStderr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    if (!ran) {
        LOG_ERROR("P4: spawn failed after %lld ms (see RunProcessCapture errors).", static_cast<long long>(ms));
        return false;
    }
    LOG_INFO("P4: completed exit=%d duration=%lld ms", outExitCode, static_cast<long long>(ms));
    if (outExitCode != 0 && !outStderr.empty()) {
        LOG_WARN("P4: stderr: %s", TruncateForLog(outStderr, kP4LogMaxStderr).c_str());
    } else if (outExitCode != 0) {
        LOG_WARN("P4: non-zero exit=%d with empty stderr", outExitCode);
    }
    if (Logger::Instance().GetLogP4Io() && Logger::Instance().ShouldLog(LogLevel::Trace) && !outStdout.empty()) {
        LOG_TRACE("P4: stdout: %s", TruncateForLog(outStdout, kP4LogMaxStdoutTrace).c_str());
    }
    return true;
#else
    const std::string exe = cfg.P4Executable.empty() ? "p4" : cfg.P4Executable;
    LOG_INFO("P4: spawn exe=\"%s\" args: %s", exe.c_str(), JoinStrings(args, " ").c_str());
    if (!RunProcessCapturePosix(exe, args, outExitCode, outStdout, outStderr)) {
        LOG_ERROR("P4: posix spawn failed");
        return false;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    LOG_INFO("P4: completed exit=%d duration=%lld ms", outExitCode, static_cast<long long>(ms));
    if (outExitCode != 0) {
        LOG_WARN("P4: non-zero exit=%d stderr=%s", outExitCode, TruncateForLog(outStderr, kP4LogMaxStderr).c_str());
    }
    if (Logger::Instance().GetLogP4Io() && Logger::Instance().ShouldLog(LogLevel::Trace) && !outStdout.empty()) {
        LOG_TRACE("P4: stdout: %s", TruncateForLog(outStdout, kP4LogMaxStdoutTrace).c_str());
    }
    return true;
#endif
}

P4LineBlame P4BlameLine(const BlameAnalysisConfig& cfg, const std::string& depotOrPath, int oneBasedLine,
                        const std::string& atChangelist) {
    P4LineBlame result;
    if (depotOrPath.empty() || oneBasedLine <= 0) {
        result.Error = "invalid path or line";
        LOG_WARN("P4BlameLine: invalid input (empty path or line<=0) line=%d", oneBasedLine);
        return result;
    }

    std::string pathArg = depotOrPath;
    if (!atChangelist.empty() && pathArg.find('@') == std::string::npos && pathArg.find('#') == std::string::npos) {
        pathArg += "@" + atChangelist;
    }
    LOG_DEBUG("P4BlameLine: pathArg=%s line=%d atCl=%s", pathArg.c_str(), oneBasedLine, atChangelist.c_str());
    const std::vector<std::string> args = {"annotate", "-u", "-c", "-q", pathArg};

    int code = 0;
    std::string out;
    std::string err;
    if (!P4RunCommand(cfg, args, code, out, err)) {
        result.Error = "failed to run p4";
        LOG_WARN("P4BlameLine: failed to spawn p4 pathArg=%s line=%d", pathArg.c_str(), oneBasedLine);
        return result;
    }
    if (code != 0) {
        result.Error = FormatP4CommandError("p4 annotate failed", code, err);
        LOG_DEBUG("P4BlameLine: annotate non-zero exit=%d, trying changes fallback pathArg=%s err=%s", code,
                  pathArg.c_str(), TruncateForLog(result.Error, 512).c_str());
        // Fallback: latest change on file
        std::vector<std::string> chArgs = {"changes", "-m", "1", pathArg};
        int c2 = 0;
        std::string o2;
        std::string e2;
        if (P4RunCommand(cfg, chArgs, c2, o2, e2) && c2 == 0) {
            LOG_DEBUG("P4BlameLine: changes fallback success pathArg=%s", pathArg.c_str());
            return ParseLatestChangeFromChangesOutput(o2, e2);
        }
        LOG_WARN("P4BlameLine: annotate failed and changes fallback failed pathArg=%s line=%d", pathArg.c_str(),
                 oneBasedLine);
        return result;
    }

    const std::vector<std::string> lines = SplitLines(out);
    if (oneBasedLine <= 0 || static_cast<size_t>(oneBasedLine) > lines.size()) {
        result.Approximate = true;
        LOG_DEBUG("P4BlameLine: line out of annotate range (line=%zu annotateLines=%zu) pathArg=%s",
                  static_cast<size_t>(oneBasedLine), lines.size(), pathArg.c_str());
        std::vector<std::string> chArgs = {"changes", "-m", "1", pathArg};
        int c2 = 0;
        std::string o2;
        std::string e2;
        if (P4RunCommand(cfg, chArgs, c2, o2, e2) && c2 == 0) {
            return ParseLatestChangeFromChangesOutput(o2, e2);
        }
        result.Error = "line out of range";
        LOG_WARN("P4BlameLine: %s pathArg=%s", result.Error.c_str(), pathArg.c_str());
        return result;
    }

    const std::string& L = lines[static_cast<size_t>(oneBasedLine - 1)];
    std::string cl;
    std::string user;
    std::string annDate;
    std::string codeLine;
    if (!ParseAnnotateTextLine(L, cl, user, annDate, codeLine)) {
        result.Approximate = true;
        LOG_DEBUG("P4BlameLine: unrecognized annotate line, trying changes pathArg=%s raw=%s", pathArg.c_str(),
                  TruncateForLog(L, 200).c_str());
        std::vector<std::string> chArgs = {"changes", "-m", "1", pathArg};
        int c2 = 0;
        std::string o2;
        std::string e2;
        if (P4RunCommand(cfg, chArgs, c2, o2, e2) && c2 == 0) {
            P4LineBlame fb = ParseLatestChangeFromChangesOutput(o2, e2);
            return fb;
        }
        result.Error = "unrecognized annotate line";
        LOG_WARN("P4BlameLine: %s pathArg=%s", result.Error.c_str(), pathArg.c_str());
        return result;
    }

    result.Changelist = cl;
    result.User = user;
    if (!annDate.empty()) {
        result.Date = annDate;
    }
    constexpr size_t kMaxSnippet = 400;
    if (codeLine.size() > kMaxSnippet) {
        result.LineSnippet = codeLine.substr(0, kMaxSnippet);
    } else {
        result.LineSnippet = codeLine;
    }
    LOG_DEBUG("P4BlameLine: success cl=%s user=%s pathArg=%s", cl.c_str(), user.c_str(), pathArg.c_str());
    return result;
}

std::vector<P4AnnotatedLine> P4AnnotateFile(const BlameAnalysisConfig& cfg, const std::string& depotOrPath,
                                            const std::string& atChangelist, std::string& outError) {
    std::vector<P4AnnotatedLine> rows;
    outError.clear();
    if (depotOrPath.empty()) {
        outError = "empty path";
        LOG_WARN("P4AnnotateFile: empty depot path");
        return rows;
    }
    std::string pathArg = depotOrPath;
    if (!atChangelist.empty() && pathArg.find('@') == std::string::npos && pathArg.find('#') == std::string::npos) {
        pathArg += "@" + atChangelist;
    }
    LOG_DEBUG("P4AnnotateFile: pathArg=%s atCl=%s", pathArg.c_str(), atChangelist.c_str());
    const std::vector<std::string> args = {"annotate", "-u", "-c", "-q", pathArg};

    int code = 0;
    std::string out;
    std::string err;
    if (!P4RunCommand(cfg, args, code, out, err)) {
        outError = "failed to run p4";
        LOG_WARN("P4AnnotateFile: failed to spawn p4 pathArg=%s", pathArg.c_str());
        return rows;
    }
    if (code != 0) {
        outError = FormatP4CommandError("p4 annotate failed", code, err);
        LOG_WARN("P4AnnotateFile: annotate failed exit=%d pathArg=%s err=%s", code, pathArg.c_str(),
                 TruncateForLog(outError, kP4LogMaxStderr).c_str());
        return rows;
    }

    const std::vector<std::string> lines = SplitLines(out);
    LOG_DEBUG("P4AnnotateFile: parsed %zu lines pathArg=%s", lines.size(), pathArg.c_str());
    for (size_t i = 0; i < lines.size(); ++i) {
        P4AnnotatedLine row;
        row.SourceLine = static_cast<int>(i + 1);
        std::string cl;
        std::string user;
        std::string annDate;
        std::string codeLine;
        if (ParseAnnotateTextLine(lines[i], cl, user, annDate, codeLine)) {
            row.Changelist = cl;
            row.User = user;
            row.Date = annDate;
            row.Code = codeLine;
        } else {
            row.Code = lines[i];
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/** Advance (y,m,d) by one day in UTC (exclusive end date for Perforce `//...@start,end` ranges). */
static bool SmatchetAddOneUtcCalendarDay(int y, int m, int d, int& oy, int& om, int& od) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = 0;
#if defined(_WIN32)
    const time_t sec0 = _mkgmtime(&t);
#else
    const time_t sec0 = timegm(&t);
#endif
    if (sec0 == static_cast<time_t>(-1)) {
        return false;
    }
    const time_t sec1 = sec0 + 86400;
    std::tm out{};
#if defined(_WIN32)
    if (gmtime_s(&out, &sec1) != 0) {
        return false;
    }
#else
    if (gmtime_r(&sec1, &out) == nullptr) {
        return false;
    }
#endif
    oy = out.tm_year + 1900;
    om = out.tm_mon + 1;
    od = out.tm_mday;
    return true;
}

bool P4FirstSubmittedChangelistOnCalendarDay(const BlameAnalysisConfig& cfg, int year, int month, int day,
                                            std::string& outChangelist, std::string& outError) {
    outChangelist.clear();
    outError.clear();
    if (year < 1970 || year > 3000 || month < 1 || month > 12 || day < 1 || day > 31) {
        outError = "invalid calendar date";
        return false;
    }
    int ey = 0;
    int em = 0;
    int ed = 0;
    if (!SmatchetAddOneUtcCalendarDay(year, month, day, ey, em, ed)) {
        outError = "date range arithmetic failed";
        return false;
    }
    char start[32];
    char endEx[32];
    std::snprintf(start, sizeof(start), "%04d/%02d/%02d", year, month, day);
    std::snprintf(endEx, sizeof(endEx), "%04d/%02d/%02d", ey, em, ed);
    const std::string filespec = std::string("//...@") + start + "," + endEx;
    const std::vector<std::string> args = {"changes", "-r", "-m", "1", "-s", "submitted", filespec};

    int code = 0;
    std::string out;
    std::string err;
    if (!P4RunCommand(cfg, args, code, out, err)) {
        outError = "failed to run p4";
        return false;
    }
    if (code != 0) {
        outError = FormatP4CommandError("p4 changes failed", code, err);
        return false;
    }
    if (out.find_first_not_of(" \t\r\n") == std::string::npos) {
        outError = "no submitted changelists on that calendar day (or no visibility to //...@)";
        return false;
    }
    const P4LineBlame parsed = ParseLatestChangeFromChangesOutput(out, err);
    if (parsed.Changelist.empty()) {
        outError = parsed.Error.empty() ? "could not parse p4 changes output" : parsed.Error;
        return false;
    }
    outChangelist = parsed.Changelist;
    return true;
}

P4ChangelistDescribeCache::P4ChangelistDescribeCache(int maxEntries) : maxEntries_(maxEntries > 0 ? maxEntries : 16) {}

P4ChangelistDetails P4ChangelistDescribeCache::Get(const std::string& changelist) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(changelist);
    if (it == map_.end()) {
        return P4ChangelistDetails();
    }
    return it->second;
}

void P4ChangelistDescribeCache::Touch(const std::string& cl) {
    auto it = std::find(lru_.begin(), lru_.end(), cl);
    if (it != lru_.end()) {
        lru_.erase(it);
    }
    lru_.push_back(cl);
}

void P4ChangelistDescribeCache::EvictIfNeeded() {
    while (static_cast<int>(map_.size()) > maxEntries_ && !lru_.empty()) {
        const std::string victim = lru_.front();
        lru_.erase(lru_.begin());
        map_.erase(victim);
    }
}

void P4ChangelistDescribeCache::Store(const std::string& changelist, P4ChangelistDetails d) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[changelist] = std::move(d);
    Touch(changelist);
    EvictIfNeeded();
}

P4ChangelistDetails P4ChangelistDescribeCache::GetOrFetch(const BlameAnalysisConfig& cfg,
                                                          const std::string& changelist) {
    if (changelist.empty()) {
        return P4ChangelistDetails();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = map_.find(changelist);
        if (it != map_.end() && it->second.Loaded) {
            Touch(changelist);
            LOG_DEBUG("P4ChangelistDescribeCache: hit cl=%s", changelist.c_str());
            return it->second;
        }
    }

    LOG_DEBUG("P4ChangelistDescribeCache: miss cl=%s fetching", changelist.c_str());
    std::vector<std::string> args = {"describe", "-s", changelist};
    int code = 0;
    std::string out;
    std::string err;
    P4ChangelistDetails d;
    d.Loaded = true;
    if (!P4RunCommand(cfg, args, code, out, err) || code != 0) {
        d.Error = FormatP4CommandError("p4 describe failed", code, err);
        LOG_WARN("P4ChangelistDescribeCache: describe failed cl=%s err=%s", changelist.c_str(),
                 TruncateForLog(d.Error, kP4LogMaxStderr).c_str());
        Store(changelist, d);
        return d;
    }

    // Common: "Change N by user@client on 2026/04/09 ..."  OR  "Change N on date by user ..."
    std::smatch m;
    static const std::regex headByOn(R"(Change\s+\d+\s+by\s+(\S+)\s+on\s+([^\r\n]+))");
    static const std::regex headOnBy(R"(Change\s+\d+\s+on\s+(\S+)\s+by\s+(\S+))");
    if (std::regex_search(out, m, headByOn)) {
        std::string who = m[1].str();
        StripP4UserDomain(who);
        d.Author = who;
        d.Date = TrimCopy(m[2].str());
    } else if (std::regex_search(out, m, headOnBy)) {
        d.Date = m[1].str();
        std::string who = m[2].str();
        StripP4UserDomain(who);
        d.Author = who;
    }
    size_t descStart = out.find('\t');
    if (descStart == std::string::npos) {
        descStart = out.find('\n');
    }
    if (descStart != std::string::npos) {
        d.Description = TrimCopy(out.substr(descStart));
        if (d.Description.size() > 2000) {
            d.Description.resize(2000);
            d.Description += "...";
        }
    }
    if (d.Author.empty() && d.Date.empty()) {
        LOG_DEBUG("P4ChangelistDescribeCache: header parse miss cl=%s stdout=%s", changelist.c_str(),
                  TruncateForLog(out, 300).c_str());
    }
    Store(changelist, d);
    return d;
}






