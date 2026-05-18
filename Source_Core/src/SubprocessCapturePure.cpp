#include "SubprocessCapturePure.h"

#include <algorithm>

namespace SubprocessCapturePure {

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

std::string BuildEnvBlockWindows(const std::vector<std::pair<std::string, std::string>>& env) {
    std::string out;
    if (env.empty()) {
        // Windows requires the double-null even for an empty block.
        out.push_back('\0');
        out.push_back('\0');
        return out;
    }
    for (size_t i = 0; i < env.size(); ++i) {
        out.append(env[i].first);
        out.push_back('=');
        out.append(env[i].second);
        out.push_back('\0');
    }
    out.push_back('\0');
    return out;
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
