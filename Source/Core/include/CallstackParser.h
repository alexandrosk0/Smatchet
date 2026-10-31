#ifndef CALLSTACK_PARSER_H
#define CALLSTACK_PARSER_H

#include "ConfigManager.h"

#include <string>
#include <vector>

struct ParsedCallstackFrame {
    std::string RawLine;
    std::string Function;
    std::string FilePath;
    int LineNumber = 0;
};

/** Apply longest matching PathRemaps prefix replacement (case-sensitive on Windows paths as given). */
std::string ApplyPathRemaps(std::string path, const std::vector<PathRemapRule>& remaps);

bool FrameMatchesIgnoreKeywords(const ParsedCallstackFrame& frame, const std::vector<std::string>& keywords);

/**
 * Parse a raw callstack into frames with file + line + best-effort function name.
 * Multiple line formats are tried (MSVC, Unreal Editor `Module!Func() [path:line]`, GDB/Clang, generic path:line).
 * Each format is matched by a linear scan (no std::regex), so the result does not depend on the standard
 * library and a long line costs time proportional to its length. Lines over 16 KiB are skipped.
 */
std::vector<ParsedCallstackFrame> ParseCallstackText(const std::string& text);

#endif
