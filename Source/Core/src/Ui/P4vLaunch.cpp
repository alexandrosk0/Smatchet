// Extracted verbatim from AnnotateAnalysisUi_Launch.cpp (user-info-window Slice 1).
// Logic, log lines and the annotate_allow_custom_commands gate are unchanged --
// only the namespace moved (AnnotateInternal -> P4vLaunch).

#include "Ui/P4vLaunch.h"

#include "Logger.h"
#include "StringUtil.h"

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
    if (arg.find_first_of(L" \t\n\"") == std::wstring::npos) {
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
    out += L"\"";
    return out;
}

std::wstring ResolveP4VcExecutableWide(const AnnotateAnalysisConfig& cfg) {
    const std::string& exeUtf8 = cfg.P4VcExecutable.empty() ? std::string("p4vc") : cfg.P4VcExecutable;
    std::wstring wexe = Utf8ToWide(exeUtf8);
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
            params = L"change " + Utf8ToWide(cl);
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
