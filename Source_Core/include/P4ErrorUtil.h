#pragma once

#include <string>

inline std::string FormatP4CommandError(const char* context, int exitCode, const std::string& stderrText) {
    if (!stderrText.empty()) {
        return stderrText;
    }
    std::string prefix = context ? std::string(context) : std::string("p4 command failed");
    return prefix + " (exit " + std::to_string(exitCode) + ")";
}
