// Pure, ImGui-free helpers for the Log window. Extracted from SmatchetUI::drawLogWindow so the
// formatting + clipboard join are bucket-A testable in the doctest rig (no ImGui link) and the
// draw body stays under the branch cap.

#include "Ui/SmatchetLog_detail.h"

#include <string>
#include <vector>

namespace SmatchetLog {
namespace detail {

void AppendLogEntryAsLines(std::vector<std::string>& out, const char* levelTag8, const std::string& message) {
    const char* kPad = "        ";
    size_t start = 0;
    bool first = true;
    for (;;) {
        const size_t nl = message.find('\n', start);
        const std::string part = (nl == std::string::npos) ? message.substr(start) : message.substr(start, nl - start);
        out.push_back(std::string(first ? levelTag8 : kPad) + part);
        first = false;
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
}

void FillLogViewLinesFromEntries(const std::vector<LogEntry>& entries, std::vector<std::string>& out) {
    out.clear();
    out.reserve(entries.size() * 2);
    for (const auto& e : entries) {
        const char* levelLabel;
        switch (e.level) {
        case LogLevel::Trace:
            levelLabel = "[TRACE] ";
            break;
        case LogLevel::Debug:
            levelLabel = "[DEBUG] ";
            break;
        case LogLevel::Info:
            levelLabel = "[INFO ] ";
            break;
        case LogLevel::Warn:
            levelLabel = "[WARN ] ";
            break;
        case LogLevel::Error:
            levelLabel = "[ERROR] ";
            break;
        default:
            levelLabel = "";
            break;
        }
        AppendLogEntryAsLines(out, levelLabel, e.message);
    }
}

std::string JoinLogLines(const std::vector<std::string>& lines) {
    size_t n = 0;
    for (const auto& s : lines) {
        n += s.size() + 1;
    }
    if (!lines.empty() && n > 0) {
        --n;
    }
    std::string clip;
    clip.reserve(n);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            clip.push_back('\n');
        }
        clip.append(lines[i]);
    }
    return clip;
}

} // namespace detail
} // namespace SmatchetLog
