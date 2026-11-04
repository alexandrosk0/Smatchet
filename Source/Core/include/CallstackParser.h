#ifndef CALLSTACK_PARSER_H
#define CALLSTACK_PARSER_H

#include "ConfigManager.h"

#include <string>
#include <vector>

/**
 * Callstack parsing for annotate analysis. Rules:
 *
 * - PathRemaps: ApplyPathRemaps replaces the LONGEST matching FromPrefix (a later rule wins a
 *   tie), compared byte for byte, so Windows paths match case-sensitively as the user typed them.
 *   Before changing the matching, check every ApplyPathRemaps caller and
 *   tests/Core/CallstackParser.test.cpp: Windows path semantics break easily.
 * - Ignore keywords: FrameMatchesIgnoreKeywords is a case-insensitive substring test over RawLine,
 *   Function and FilePath. Frames are filtered with it in the analysis worker, never in draw code.
 * - A new callstack format is a new Match*Format matcher in CallstackParser.cpp, tried from
 *   TryParsePathLinePair on the raw line text (kept as RawLine); keep it a linear scan (no
 *   std::regex), with a case in tests/Core/CallstackParser.test.cpp or CallstackParserScan.test.cpp.
 *
 * How the annotate views use these: Source/Core/src/Ui/AGENTS.md § Annotate views and syntax highlight
 */

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
