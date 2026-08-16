#ifndef SMATCHET_DIAGNOSTICS_LOG_TAIL_SELECT_H
#define SMATCHET_DIAGNOSTICS_LOG_TAIL_SELECT_H

// LogTailSelect.h — pure selection over a Logger entry snapshot, backing the
// debug.log_tail command (docs/plans/shipped/autonomous-debug-live-evidence.md
// slice 2). Header-only and dependency-free beyond Logger.h + std, so the logic is
// testable in the fast SmatchetTests rig rather than only through the Linux-only
// SmatchetCommandsTests dispatch harness.
//
// Entries reaching here are ALREADY redacted: Logger::Log applies
// privacy::RedactLogLine at ingest, so nothing here redacts. That property is what
// makes the ring safe to hand to an MCP client, so a test pins it end-to-end rather
// than leaving it as a comment.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Logger.h"

namespace smatchet {
namespace diagnostics {

// Bounds for one log-tail request. The ceiling mirrors kMaxLogTailLines in
// EngineContextFormat.cpp so the two agent-facing log tails agree on how much a
// caller may pull in a single response.
const std::size_t kLogTailMinLines = 1;
const std::size_t kLogTailMaxLines = 300;
const std::size_t kLogTailDefaultLines = 100;

// Clamp a caller-supplied line count into [kLogTailMinLines, kLogTailMaxLines].
// Takes long long because the command layer resolves the arg as a JSON integer.
inline std::size_t ClampLogTailLines(long long requested) {
    if (requested < static_cast<long long>(kLogTailMinLines)) {
        return kLogTailMinLines;
    }
    if (requested > static_cast<long long>(kLogTailMaxLines)) {
        return kLogTailMaxLines;
    }
    return static_cast<std::size_t>(requested);
}

// Filter `entries` by minimum level and by substring, then return the LAST
// `maxLines` matches in oldest-first order.
//
// Filtering happens BEFORE the tail is taken, so `maxLines` bounds the MATCHED
// entries rather than the scanned ones — asking for 10 lines containing "sync"
// yields the 10 most recent sync lines, not "the last 10 lines, of which some
// mention sync". An empty `contains` disables the substring filter.
//
// `outTotalMatched`, when non-null, receives the pre-tail match count so a caller
// can report how much was elided.
inline std::vector<LogEntry> SelectLogTail(const std::vector<LogEntry>& entries, LogLevel minLevel,
                                           const std::string& contains, std::size_t maxLines,
                                           std::size_t* outTotalMatched = nullptr) {
    std::vector<LogEntry> matched;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const LogEntry& e = entries[i];
        if (static_cast<int>(e.level) < static_cast<int>(minLevel)) {
            continue;
        }
        if (!contains.empty() && e.message.find(contains) == std::string::npos) {
            continue;
        }
        matched.push_back(e);
    }
    if (outTotalMatched != nullptr) {
        *outTotalMatched = matched.size();
    }
    if (matched.size() <= maxLines) {
        return matched;
    }
    const std::ptrdiff_t drop = static_cast<std::ptrdiff_t>(matched.size() - maxLines);
    return std::vector<LogEntry>(matched.begin() + drop, matched.end());
}

} // namespace diagnostics
} // namespace smatchet

#endif // SMATCHET_DIAGNOSTICS_LOG_TAIL_SELECT_H
