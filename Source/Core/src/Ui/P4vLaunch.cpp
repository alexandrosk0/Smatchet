// Extracted verbatim from AnnotateAnalysisUi_Launch.cpp (user-info-window Slice 1).
// Logic, log lines and the annotate_allow_custom_commands gate are unchanged --
// only the namespace moved (AnnotateInternal -> P4vLaunch).

#include "Ui/P4vLaunch.h"

#include "Logger.h"
#include "StringUtil.h"
#include "Ui/P4vLaunchArgQuotePure.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <string>

namespace P4vLaunch {

#ifdef _WIN32

namespace {

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

void ReplacePlaceholder(std::string& s, const char* key, const std::string& val) {
    const std::string k = key;
    size_t pos = 0;
    while ((pos = s.find(k, pos)) != std::string::npos) {
        s.replace(pos, k.size(), val);
        pos += val.size();
    }
}

bool SplitCommandExecutableAndArgs(const std::string& command, std::string& outExe, std::string& outArgs) {
    const std::string trimmed = TrimCopy(command);
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed[0] == '"') {
        const size_t quoteEnd = trimmed.find('"', 1);
        if (quoteEnd == std::string::npos || quoteEnd <= 1) {
            return false;
        }
        outExe = trimmed.substr(1, quoteEnd - 1);
        outArgs = TrimCopy(trimmed.substr(quoteEnd + 1));
        return !outExe.empty();
    }
    size_t split = 0;
    while (split < trimmed.size() && trimmed[split] != ' ' && trimmed[split] != '\t' && trimmed[split] != '\r' &&
           trimmed[split] != '\n') {
        ++split;
    }
    outExe = trimmed.substr(0, split);
    outArgs = split < trimmed.size() ? TrimCopy(trimmed.substr(split + 1)) : std::string();
    return !outExe.empty();
}

std::wstring QuoteWinArgWide(const std::wstring& arg) {
    // Delegate to the pure, unit-tested helper. The canonical CommandLineToArgvW
    // quoting algorithm (backslash-run doubling before any quote AND before the
    // closing wrap quote) lives in P4vLaunchArgQuotePure.h so the trailing-
    // backslash + embedded-quote corner cases are covered by doctest without a
    // Windows toolchain. The prior in-place version doubled neither, letting a
    // file/changelist field ending in '\' break out of its quoted argument.
    return P4vLaunch::QuoteWinArgWidePure(arg);
}

std::wstring ResolveP4VcExecutableWide(const AnnotateAnalysisConfig& cfg) {
    const std::string& exeUtf8 = cfg.P4VcExecutable.empty() ? std::string("p4vc") : cfg.P4VcExecutable;
    std::wstring wexe = Utf8ToWide(exeUtf8);
    if (wexe.find(L'\\') != std::wstring::npos || wexe.find(L'/') != std::wstring::npos) {
        return wexe;
    }
    // Resolve a bare p4vc name to its absolute path so ShellExecuteW launches a
    // fully-qualified trusted binary rather than re-searching PATH (audit #16 —
    // binary-planting surface). A resolution miss falls back to the bare name
    // and warns so the residual PATH-launch is observable.
    wchar_t found[MAX_PATH];
    wchar_t* fname = nullptr;
    if (SearchPathW(nullptr, wexe.c_str(), L".exe", MAX_PATH, found, &fname) > 0) {
        return std::wstring(found);
    }
    LOG_WARN("P4vLaunch: could not resolve \"%s\" to an absolute path via SearchPathW; "
             "falling back to PATH-based launch (binary-planting surface, audit #16)",
             exeUtf8.c_str());
    return wexe;
}

std::wstring WorkingDirWideForFile(const std::string& fileUtf8) {
    const size_t slash = fileUtf8.find_last_of("\\/");
    if (slash == std::string::npos || slash == 0) {
        return std::wstring();
    }
    return Utf8ToWide(fileUtf8.substr(0, slash));
}

std::string WideToUtf8ForLog(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return std::string();
    }
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

} // namespace

bool LaunchP4VcLike(const AnnotateAnalysisConfig& cfg, const std::string& timelapseTemplate,
                    const std::string& changeTemplate, bool isTimelapse, const std::string& file, int line,
                    const std::string& cl) {
    const std::wstring workDir = WorkingDirWideForFile(file);
    const wchar_t* workDirPtr = workDir.empty() ? nullptr : workDir.c_str();

    bool customCmd = (isTimelapse && !timelapseTemplate.empty()) || (!isTimelapse && !changeTemplate.empty());

    // Custom templates invoke arbitrary programs from config-controlled strings.
    // Require explicit opt-in so a stolen/edited config file cannot auto-exec.
    if (customCmd) {
        const TrackerConfig jiraCfg = ConfigManager::Load();
        if (!jiraCfg.AnnotateAllowCustomCommands) {
            LOG_WARN(
                "LaunchP4VcLike: custom command template present but annotate_allow_custom_commands=false; falling "
                "back to p4vc.");
            customCmd = false;
        }
    }

    if (!customCmd) {
        const std::wstring app = ResolveP4VcExecutableWide(cfg);
        std::wstring params;
        if (isTimelapse) {
            params = L"timelapse -l " + std::to_wstring(line) + L" " + QuoteWinArgWide(Utf8ToWide(file));
        } else {
            // Quote the changelist field too: it is a user/server-supplied value
            // and an embedded space / quote / trailing backslash would otherwise
            // shift the argument boundary on the spawned p4vc command line.
            params = L"change " + QuoteWinArgWide(Utf8ToWide(cl));
        }
        LOG_INFO("LaunchP4VcLike (direct p4vc): %s | CWD=%s",
                 (WideToUtf8ForLog(app) + " " + WideToUtf8ForLog(params)).c_str(),
                 workDirPtr ? WideToUtf8ForLog(workDir).c_str() : "(inherit)");
        SetLastError(0);
        const INT_PTR r = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, nullptr, app.c_str(), params.c_str(), workDirPtr, SW_SHOW));
        if (r <= 32) {
            LOG_WARN("LaunchP4VcLike: ShellExecuteW failed, result=%lld GetLastError=%lu", static_cast<long long>(r),
                     static_cast<unsigned long>(GetLastError()));
        }
        return r > 32;
    }

    std::string cmd;
    if (isTimelapse) {
        cmd = timelapseTemplate;
    } else {
        cmd = changeTemplate;
    }
    // Placeholder values are substituted raw into a command line whose quoting
    // structure the template author controls — we cannot re-quote per-arg here.
    // A {file}/{cl} value containing a double-quote could close the template's
    // wrap quote and inject arguments; a value ENDING IN A BACKSLASH escapes the
    // template's closing wrap quote ("foo\") and shifts the argument boundary
    // (#1712 — same trailing-backslash injection class QuoteWinArgWidePure guards
    // on the helper path, but here the raw field is the vector). Reject both.
    if (P4vLaunch::P4vCustomCommandFieldRejected(file) || P4vLaunch::P4vCustomCommandFieldRejected(cl)) {
        LOG_WARN("LaunchP4VcLike: custom command rejected because a {file}/{cl} value contains a double-quote "
                 "or ends with a backslash");
        return false;
    }
    ReplacePlaceholder(cmd, "{file}", file);
    ReplacePlaceholder(cmd, "{line}", std::to_string(line));
    ReplacePlaceholder(cmd, "{cl}", cl);
    if (cmd.find('\r') != std::string::npos || cmd.find('\n') != std::string::npos) {
        LOG_WARN("LaunchP4VcLike: custom command rejected because it contains newline characters");
        return false;
    }
    std::string exeUtf8;
    std::string argsUtf8;
    if (!SplitCommandExecutableAndArgs(cmd, exeUtf8, argsUtf8)) {
        LOG_WARN("LaunchP4VcLike: custom command parse failed. Expected: <exe> [args]");
        return false;
    }
    LOG_INFO("LaunchP4VcLike (custom direct launch): exe=\"%s\" args=\"%s\"", exeUtf8.c_str(), argsUtf8.c_str());
    const std::wstring wexe = Utf8ToWide(exeUtf8);
    const std::wstring wargs = Utf8ToWide(argsUtf8);
    SetLastError(0);
    const INT_PTR r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, nullptr, wexe.c_str(), wargs.empty() ? nullptr : wargs.c_str(), workDirPtr, SW_SHOW));
    if (r <= 32) {
        LOG_WARN("LaunchP4VcLike: ShellExecuteW(custom) failed, result=%lld GetLastError=%lu",
                 static_cast<long long>(r), static_cast<unsigned long>(GetLastError()));
    }
    return r > 32;
}

#else

bool LaunchP4VcLike(const AnnotateAnalysisConfig&, const std::string&, const std::string&, bool, const std::string&,
                    int, const std::string&) {
    return false;
}

#endif

} // namespace P4vLaunch
