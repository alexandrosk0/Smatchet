#include "P4Blame.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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
#include <sys/wait.h>
#endif

namespace {

constexpr size_t kP4LogMaxStderr = 2048;
constexpr size_t kP4LogMaxStdoutTrace = 8192;

std::string TruncateForLog(std::string s, size_t maxLen) {
    if (s.size() <= maxLen) {
        return s;
    }
    s.resize(maxLen);
    s += "... [truncated]";
    return s;
}

std::string JoinArgsForLog(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            out += ' ';
        }
        out += args[i];
    }
    return out;
}


std::string Trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.erase(0, 1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
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

bool RunProcessCapture(const std::wstring& applicationName,
                       const std::wstring& commandLine,
                       int& outExit,
                       std::string& outStdout,
                       std::string& outStderr) {
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
        LOG_ERROR("P4 RunProcessCapture: CreatePipe failed GetLastError=%lu", static_cast<unsigned long>(GetLastError()));
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
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    const BOOL ok = CreateProcessW(
        applicationName.empty() ? nullptr : applicationName.c_str(),
        mutableCmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);
    CloseHandle(wrOut);
    CloseHandle(wrErr);
    if (!ok) {
        LOG_ERROR("P4 RunProcessCapture: CreateProcessW failed GetLastError=%lu", static_cast<unsigned long>(GetLastError()));
        CloseHandle(rdOut);
        CloseHandle(rdErr);
        return false;
    }

    char buf[4096];
    DWORD n = 0;
    for (;;) {
        n = 0;
        if (ReadFile(rdOut, buf, sizeof(buf), &n, nullptr) && n > 0) {
            outStdout.append(buf, n);
        }
        n = 0;
        if (ReadFile(rdErr, buf, sizeof(buf), &n, nullptr) && n > 0) {
            outStderr.append(buf, n);
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (n == 0) {
            Sleep(1);
        }
    }
    while (ReadFile(rdOut, buf, sizeof(buf), &n, nullptr) && n > 0) {
        outStdout.append(buf, n);
    }
    while (ReadFile(rdErr, buf, sizeof(buf), &n, nullptr) && n > 0) {
        outStderr.append(buf, n);
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    outExit = static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rdOut);
    CloseHandle(rdErr);
    return true;
}
#endif

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) {
            lines.push_back(Trim(s.substr(i)));
            break;
        }
        lines.push_back(Trim(s.substr(i, j - i)));
        i = j + 1;
    }
    return lines;
}

void StripP4UserDomain(std::string& user) {
    const size_t at = user.find('@');
    if (at != std::string::npos) {
        user = user.substr(0, at);
    }
}

/**
 * Parse `p4 annotate` text lines. Observed formats:
 * - rev user: code
 * - rev: user YYYY/MM/DD code   (common with -c -u on some servers)
 * - rev user YYYY/MM/DD code
 * - rev: code
 */
bool ParseAnnotateTextLine(const std::string& line,
                           std::string& outCl,
                           std::string& outUser,
                           std::string& outAnnotDate,
                           std::string& outCode) {
    outCl.clear();
    outUser.clear();
    outAnnotDate.clear();
    outCode = line;
    static const std::regex reUserColonCode(R"(^\s*(\d+)\s+(\S+)\s*:\s*(.*)$)");
    // Code may be empty (blank source line): "rev: user YYYY/MM/DD" with nothing after the date.
    static const std::regex reRevUserDateCode(R"(^\s*(\d+)\s*:\s*(\S+)\s+(\S+)(?:\s+(.*))?$)");
    static const std::regex reRevUserDateCodeNoColon(R"(^\s*(\d+)\s+(\S+)\s+(\S+)(?:\s+(.*))?$)");
    static const std::regex revOnlyColonCode(R"(^\s*(\d+)\s*:\s*(.*)$)");
    std::smatch m;
    if (std::regex_match(line, m, reUserColonCode)) {
        outCl = m[1].str();
        outUser = m[2].str();
        outCode = m[3].str();
        StripP4UserDomain(outUser);
        return true;
    }
    if (std::regex_match(line, m, reRevUserDateCode)) {
        outCl = m[1].str();
        outUser = m[2].str();
        outAnnotDate = m[3].str();
        outCode = m[4].str();
        StripP4UserDomain(outUser);
        return true;
    }
    if (std::regex_match(line, m, reRevUserDateCodeNoColon)) {
        outCl = m[1].str();
        outUser = m[2].str();
        outAnnotDate = m[3].str();
        outCode = m[4].str();
        StripP4UserDomain(outUser);
        return true;
    }
    if (std::regex_match(line, m, revOnlyColonCode)) {
        outCl = m[1].str();
        outCode = m[2].str();
        return true;
    }
    return false;
}

P4LineBlame ParseLatestChangeFromChangesOutput(const std::string& stdoutText, std::string& stderrText) {
    P4LineBlame b;
    b.Approximate = true;
    static const std::regex re(R"(Change\s+(\d+)\s+on\s+[^\s]+\s+by\s+(\S+))");
    std::smatch m;
    if (std::regex_search(stdoutText, m, re)) {
        b.Changelist = m[1].str();
        std::string who = m[2].str();
        const size_t at = who.find('@');
        if (at != std::string::npos) {
            who = who.substr(0, at);
        }
        b.User = who;
        return b;
    }
    if (!stderrText.empty()) {
        b.Error = stderrText;
    } else {
        b.Error = "p4 changes produced no match";
    }
    return b;
}

} // namespace

bool P4RunCommand(const BlameAnalysisConfig& cfg,
                  const std::vector<std::string>& args,
                  int& outExitCode,
                  std::string& outStdout,
                  std::string& outStderr) {
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();
#ifdef _WIN32
    const std::string& exe = cfg.P4Executable.empty() ? std::string("p4") : cfg.P4Executable;
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
    LOG_INFO("P4: spawn exe=\"%s\" args: %s", exeLogged.c_str(), JoinArgsForLog(args).c_str());

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
    const std::string& exe = cfg.P4Executable.empty() ? std::string("p4") : cfg.P4Executable;
    std::string cmd = exe;
    for (const std::string& a : args) {
        cmd += ' ';
        if (a.find_first_of(" \t\n\"'\\") != std::string::npos) {
            cmd += '"';
            for (char c : a) {
                if (c == '"' || c == '\\') {
                    cmd += '\\';
                }
                cmd += c;
            }
            cmd += '"';
        } else {
            cmd += a;
        }
    }
    LOG_INFO("P4: spawn cmd: %s", cmd.c_str());
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        LOG_ERROR("P4: popen failed errno=%d %s", errno, std::strerror(errno));
        return false;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        outStdout += buf;
    }
    const int st = pclose(pipe);
    outExitCode = (st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
    (void)outStderr;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    LOG_INFO("P4: completed exit=%d duration=%lld ms (stderr not captured on this platform; stdout only)", outExitCode,
             static_cast<long long>(ms));
    if (outExitCode != 0) {
        LOG_WARN("P4: non-zero exit=%d (stderr unavailable via popen)", outExitCode);
    }
    if (Logger::Instance().GetLogP4Io() && Logger::Instance().ShouldLog(LogLevel::Trace) && !outStdout.empty()) {
        LOG_TRACE("P4: stdout: %s", TruncateForLog(outStdout, kP4LogMaxStdoutTrace).c_str());
    }
    return true;
#endif
}

P4LineBlame P4BlameLine(const BlameAnalysisConfig& cfg,
                        const std::string& depotOrPath,
                        int oneBasedLine,
                        const std::string& atChangelist) {
    P4LineBlame result;
    if (depotOrPath.empty() || oneBasedLine <= 0) {
        result.Error = "invalid path or line";
        LOG_WARN("P4BlameLine: invalid input (empty path or line<=0) line=%d", oneBasedLine);
        return result;
    }

    std::string pathArg = depotOrPath;
    if (!atChangelist.empty() && pathArg.find('@') == std::string::npos &&
        pathArg.find('#') == std::string::npos) {
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
        result.Error = err.empty() ? ("p4 annotate exit " + std::to_string(code)) : err;
        LOG_DEBUG(
            "P4BlameLine: annotate non-zero exit=%d, trying changes fallback pathArg=%s err=%s",
            code,
            pathArg.c_str(),
            TruncateForLog(result.Error, 512).c_str());
        // Fallback: latest change on file
        std::vector<std::string> chArgs = {"changes", "-m", "1", pathArg};
        int c2 = 0;
        std::string o2;
        std::string e2;
        if (P4RunCommand(cfg, chArgs, c2, o2, e2) && c2 == 0) {
            LOG_DEBUG("P4BlameLine: changes fallback success pathArg=%s", pathArg.c_str());
            return ParseLatestChangeFromChangesOutput(o2, e2);
        }
        LOG_WARN("P4BlameLine: annotate failed and changes fallback failed pathArg=%s line=%d", pathArg.c_str(), oneBasedLine);
        return result;
    }

    const std::vector<std::string> lines = SplitLines(out);
    if (oneBasedLine <= 0 || static_cast<size_t>(oneBasedLine) > lines.size()) {
        result.Approximate = true;
        LOG_DEBUG(
            "P4BlameLine: line out of annotate range (line=%zu annotateLines=%zu) pathArg=%s",
            static_cast<size_t>(oneBasedLine),
            lines.size(),
            pathArg.c_str());
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
        LOG_DEBUG(
            "P4BlameLine: unrecognized annotate line, trying changes pathArg=%s raw=%s",
            pathArg.c_str(),
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

std::vector<P4AnnotatedLine> P4AnnotateFile(const BlameAnalysisConfig& cfg,
                                             const std::string& depotOrPath,
                                             const std::string& atChangelist,
                                             std::string& outError) {
    std::vector<P4AnnotatedLine> rows;
    outError.clear();
    if (depotOrPath.empty()) {
        outError = "empty path";
        LOG_WARN("P4AnnotateFile: empty depot path");
        return rows;
    }
    std::string pathArg = depotOrPath;
    if (!atChangelist.empty() && pathArg.find('@') == std::string::npos &&
        pathArg.find('#') == std::string::npos) {
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
        outError = err.empty() ? ("p4 annotate exit " + std::to_string(code)) : err;
        LOG_WARN(
            "P4AnnotateFile: annotate failed exit=%d pathArg=%s err=%s",
            code,
            pathArg.c_str(),
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

P4ChangelistDetails P4ChangelistDescribeCache::GetOrFetch(const BlameAnalysisConfig& cfg, const std::string& changelist) {
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
        d.Error = err.empty() ? ("p4 describe failed: " + std::to_string(code)) : err;
        LOG_WARN(
            "P4ChangelistDescribeCache: describe failed cl=%s err=%s",
            changelist.c_str(),
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
        d.Date = Trim(m[2].str());
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
        d.Description = Trim(out.substr(descStart));
        if (d.Description.size() > 2000) {
            d.Description.resize(2000);
            d.Description += "...";
        }
    }
    Store(changelist, d);
    return d;
}
