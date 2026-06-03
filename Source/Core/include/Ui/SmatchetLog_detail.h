#pragma once

// Pure, ImGui-free helpers for the Log window. Extracted from SmatchetUI::drawLogWindow so the
// log-entry → display-line formatting and clipboard join are bucket-A testable and the draw
// body stays under the function-size branch cap. No ImGui, no session state.

#include "Logger.h"

#include <string>
#include <vector>

namespace SmatchetLog {
namespace detail {

/** Append one log entry's lines to `out`: the first physical line carries the 8-char level tag,
 *  every wrapped continuation line is indented by 8 spaces so multi-line messages align. */
void AppendLogEntryAsLines(std::vector<std::string>& out, const char* levelTag8, const std::string& message);

/** Rebuild the flat display-line vector from a log-entry snapshot. Clears `out` first. Each entry
 *  contributes one tagged line plus one per embedded newline. */
void FillLogViewLinesFromEntries(const std::vector<LogEntry>& entries, std::vector<std::string>& out);

/** Join display lines with '\n' separators (no trailing newline) for the clipboard. */
std::string JoinLogLines(const std::vector<std::string>& lines);

} // namespace detail
} // namespace SmatchetLog
