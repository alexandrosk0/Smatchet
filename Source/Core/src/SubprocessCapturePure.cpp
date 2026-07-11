#include "SubprocessCapturePure.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SubprocessCapturePure {

namespace {

#if defined(_WIN32)
// UTF-8 → UTF-16 conversion for env-block entries. We embed this helper
// inside the pure TU rather than reusing the SubprocessCapture.cpp
// helper because the env block is built entirely from KEY=VALUE pairs
// the caller supplies in UTF-8 (the cross-platform CaptureOptions::env
// shape), and CreateProcessW reads the block as UTF-16 when
// CREATE_UNICODE_ENVIRONMENT is set. A failed/empty conversion
// degrades to an empty wide string so the resulting env block omits
// the corrupt entry rather than silently injecting partial bytes.
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
#else
// Non-Windows builds (dual-target test runs on POSIX) still need the
// function to compile. Widen each UTF-8 byte one-to-one; the helper is
// never called on POSIX (the env block is only consumed by
// CreateProcessW) but the unit tests instantiate it on every host.
std::wstring Utf8ToWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(s[i])));
    }
    return w;
}
#endif

} // namespace

std::string QuoteArgvWindows(const std::string& arg) {
    // Empty argv element must still produce "" so the parser sees a
    // zero-length token rather than dropping it.
    if (arg.empty()) {
        return std::string("\"\"");
    }
    // Bare token without whitespace or quotes: no quoting needed.
    if (arg.find_first_of(" \t\v\"") == std::string::npos) {
        return arg;
    }
    std::string out;
    out.reserve(arg.size() + 4);
    out.push_back('"');
    // Backslashes only need doubling when they immediately precede a
    // quote (literal or the closing wrap quote). Track the run length.
    size_t bsRun = 0;
    for (size_t i = 0; i < arg.size(); ++i) {
        const char c = arg[i];
        if (c == '\\') {
            ++bsRun;
            continue;
        }
        if (c == '"') {
            // Double every preceding backslash, then escape the quote.
            out.append(bsRun * 2 + 1, '\\');
            out.push_back('"');
            bsRun = 0;
            continue;
        }
        if (bsRun != 0) {
            out.append(bsRun, '\\');
            bsRun = 0;
        }
        out.push_back(c);
    }
    // Backslashes immediately before the closing wrap quote must also
    // be doubled — otherwise the parser pairs them with the wrap.
    out.append(bsRun * 2, '\\');
    out.push_back('"');
    return out;
}

std::string QuoteArgvPosix(const std::string& arg) {
    if (arg.empty()) {
        return std::string("''");
    }
    // Bare token with only POSIX-safe characters: no quoting needed.
    static const auto isSafe = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == '/' || c == ':' || c == ',' || c == '+' || c == '=' || c == '@';
    };
    bool allSafe = true;
    for (size_t i = 0; i < arg.size(); ++i) {
        if (!isSafe(arg[i])) {
            allSafe = false;
            break;
        }
    }
    if (allSafe) {
        return arg;
    }
    std::string out;
    out.reserve(arg.size() + 4);
    out.push_back('\'');
    for (size_t i = 0; i < arg.size(); ++i) {
        if (arg[i] == '\'') {
            out.append("'\\''");
        } else {
            out.push_back(arg[i]);
        }
    }
    out.push_back('\'');
    return out;
}

std::wstring BuildEnvBlockWindows(const std::vector<std::pair<std::string, std::string>>& env) {
    std::wstring out;
    if (env.empty()) {
        // Windows requires the double-null even for an empty block.
        out.push_back(L'\0');
        out.push_back(L'\0');
        return out;
    }
    for (size_t i = 0; i < env.size(); ++i) {
        // Build the KEY=VALUE entry in UTF-8, convert in one shot so a
        // multi-byte rune split across the name and value boundary is
        // never re-encoded twice. The intermediate string is bounded by
        // the longest individual entry; allocating it inline keeps the
        // hot path simple.
        std::string entry;
        entry.reserve(env[i].first.size() + 1 + env[i].second.size());
        entry.append(env[i].first);
        entry.push_back('=');
        entry.append(env[i].second);
        const std::wstring wide = Utf8ToWide(entry);
        out.append(wide);
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

bool IsSensitiveEnvName(const std::string& name) {
    // Uppercase a copy for case-insensitive substring matching. Env names
    // are conventionally ASCII; non-ASCII bytes pass through untouched and
    // simply won't match a token.
    std::string upper;
    upper.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
        }
        upper.push_back(c);
    }
    static const char* const kTokens[] = {"TOKEN", "SECRET",     "PASSWORD", "PASSWD", "APIKEY",  "API_KEY",   "KEY",
                                          "AUTH",  "CREDENTIAL", "SESSION",  "COOKIE", "PRIVATE", "PASSPHRASE"};
    for (size_t i = 0; i < sizeof(kTokens) / sizeof(kTokens[0]); ++i) {
        if (upper.find(kTokens[i]) != std::string::npos) {
            return true;
        }
    }
    // "_PAT" (Personal-Access-Token vars: *_PAT) is a SUFFIX token, matched as
    // ends-with — NOT a substring — so it does not false-match *_PATH vars
    // (LD_LIBRARY_PATH / DYLD_LIBRARY_PATH / GIT_EXEC_PATH / PKG_CONFIG_PATH ...),
    // which must survive the scrub so p4/git children keep loading shared libs on
    // Linux/macOS (#1711). "..._PATH" ends with "PATH", not "_PAT", so it is not
    // flagged; "GITHUB_PAT" ends with "_PAT" and is.
    static const char kPatSuffix[] = "_PAT";
    const size_t patLen = sizeof(kPatSuffix) - 1;
    if (upper.size() >= patLen && upper.compare(upper.size() - patLen, patLen, kPatSuffix) == 0) {
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>>
ScrubSensitiveEnv(const std::vector<std::pair<std::string, std::string>>& parentEnv) {
    std::vector<std::pair<std::string, std::string>> kept;
    kept.reserve(parentEnv.size());
    for (size_t i = 0; i < parentEnv.size(); ++i) {
        if (!IsSensitiveEnvName(parentEnv[i].first)) {
            kept.push_back(parentEnv[i]);
        }
    }
    return kept;
}

int64_t RemainingTimeoutMs(std::chrono::steady_clock::time_point start, int64_t totalTimeoutMs) {
    if (totalTimeoutMs <= 0) {
        return totalTimeoutMs;
    }
    const auto now = std::chrono::steady_clock::now();
    const int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    if (elapsed >= totalTimeoutMs) {
        return 0;
    }
    return totalTimeoutMs - elapsed;
}

} // namespace SubprocessCapturePure
