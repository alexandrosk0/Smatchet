// Pure-logic cover for the debug.log_tail selection (slice 2 of
// docs/plans/shipped/autonomous-debug-live-evidence.md). Lives in the fast
// SmatchetTests rig — the command itself can only be dispatched from the Linux-only
// SmatchetCommandsTests target, but the selection rules are what actually carry the
// behaviour, and they must be covered on the required Windows lane too.

#include <doctest/doctest.h>

#include "Diagnostics/LogTailSelect.h"
#include "Logger.h"
#include "Privacy/TextRedaction.h"

#include <cstddef>
#include <string>
#include <vector>

using smatchet::diagnostics::ClampLogTailLines;
using smatchet::diagnostics::kLogTailMaxLines;
using smatchet::diagnostics::kLogTailMinLines;
using smatchet::diagnostics::SelectLogTail;

namespace {

LogEntry Entry(LogLevel level, const std::string& message, double ts = 0.0) {
    LogEntry e;
    e.timestampSeconds = ts;
    e.level = level;
    e.message = message;
    return e;
}

std::vector<LogEntry> Numbered(std::size_t count, LogLevel level = LogLevel::Info) {
    std::vector<LogEntry> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(Entry(level, "line-" + std::to_string(i), static_cast<double>(i)));
    }
    return out;
}

} // namespace

TEST_CASE("log_tail — clamp bounds a caller-supplied line count") {
    CHECK(ClampLogTailLines(0) == kLogTailMinLines);
    CHECK(ClampLogTailLines(-17) == kLogTailMinLines);
    CHECK(ClampLogTailLines(1) == kLogTailMinLines);
    CHECK(ClampLogTailLines(50) == 50u);
    CHECK(ClampLogTailLines(static_cast<long long>(kLogTailMaxLines)) == kLogTailMaxLines);
    CHECK(ClampLogTailLines(static_cast<long long>(kLogTailMaxLines) + 1) == kLogTailMaxLines);
    // A caller passing a value past int range must still land on the ceiling, not wrap.
    CHECK(ClampLogTailLines(9223372036854775807LL) == kLogTailMaxLines);
}

TEST_CASE("log_tail — returns the NEWEST entries, oldest-first, and reports elision") {
    const std::vector<LogEntry> entries = Numbered(10);
    std::size_t total = 0;
    const std::vector<LogEntry> got = SelectLogTail(entries, LogLevel::Trace, "", 3, &total);

    REQUIRE(got.size() == 3u);
    CHECK(total == 10u); // pre-tail match count, so a caller can see what was dropped
    // Newest three, still in chronological order.
    CHECK(got[0].message == "line-7");
    CHECK(got[1].message == "line-8");
    CHECK(got[2].message == "line-9");
}

TEST_CASE("log_tail — asking for more than exists returns everything, untruncated") {
    const std::vector<LogEntry> entries = Numbered(4);
    std::size_t total = 0;
    const std::vector<LogEntry> got = SelectLogTail(entries, LogLevel::Trace, "", 100, &total);
    CHECK(got.size() == 4u);
    CHECK(total == 4u);
}

TEST_CASE("log_tail — empty input is empty output, not a crash") {
    std::size_t total = 7; // pre-seeded: the function must overwrite, not leave stale
    const std::vector<LogEntry> got = SelectLogTail(std::vector<LogEntry>(), LogLevel::Trace, "", 10, &total);
    CHECK(got.empty());
    CHECK(total == 0u);
}

TEST_CASE("log_tail — minLevel drops everything below it") {
    std::vector<LogEntry> entries;
    entries.push_back(Entry(LogLevel::Trace, "t"));
    entries.push_back(Entry(LogLevel::Debug, "d"));
    entries.push_back(Entry(LogLevel::Info, "i"));
    entries.push_back(Entry(LogLevel::Warn, "w"));
    entries.push_back(Entry(LogLevel::Error, "e"));

    std::size_t total = 0;
    const std::vector<LogEntry> warnUp = SelectLogTail(entries, LogLevel::Warn, "", 100, &total);
    REQUIRE(warnUp.size() == 2u);
    CHECK(warnUp[0].message == "w");
    CHECK(warnUp[1].message == "e");
    CHECK(total == 2u);

    const std::vector<LogEntry> all = SelectLogTail(entries, LogLevel::Trace, "", 100, nullptr);
    CHECK(all.size() == 5u);
}

TEST_CASE("log_tail — substring filter is applied BEFORE the tail is taken") {
    // The load-bearing ordering rule: lines=2 with contains=sync must yield the two
    // most recent SYNC lines, not "the last two lines, of which some mention sync".
    std::vector<LogEntry> entries;
    entries.push_back(Entry(LogLevel::Info, "sync started"));
    entries.push_back(Entry(LogLevel::Info, "unrelated chatter"));
    entries.push_back(Entry(LogLevel::Info, "sync progress"));
    entries.push_back(Entry(LogLevel::Info, "more chatter"));
    entries.push_back(Entry(LogLevel::Info, "sync finished"));
    entries.push_back(Entry(LogLevel::Info, "trailing noise"));

    std::size_t total = 0;
    const std::vector<LogEntry> got = SelectLogTail(entries, LogLevel::Trace, "sync", 2, &total);
    REQUIRE(got.size() == 2u);
    CHECK(got[0].message == "sync progress");
    CHECK(got[1].message == "sync finished");
    CHECK(total == 3u); // three matched; the oldest was elided by the tail
}

TEST_CASE("log_tail — level and substring filters compose") {
    std::vector<LogEntry> entries;
    entries.push_back(Entry(LogLevel::Info, "tracker ok"));
    entries.push_back(Entry(LogLevel::Warn, "tracker slow"));
    entries.push_back(Entry(LogLevel::Warn, "disk slow"));
    entries.push_back(Entry(LogLevel::Error, "tracker failed"));

    std::size_t total = 0;
    const std::vector<LogEntry> got = SelectLogTail(entries, LogLevel::Warn, "tracker", 100, &total);
    REQUIRE(got.size() == 2u);
    CHECK(got[0].message == "tracker slow");
    CHECK(got[1].message == "tracker failed");
    CHECK(total == 2u);
}

TEST_CASE("log_tail — an empty substring filter matches everything, not nothing") {
    const std::vector<LogEntry> entries = Numbered(3);
    const std::vector<LogEntry> got = SelectLogTail(entries, LogLevel::Trace, "", 100, nullptr);
    CHECK(got.size() == 3u);
}

TEST_CASE("log_tail — entries arrive already redacted, so the tail never leaks a token") {
    // debug.log_tail hands ring entries straight to an MCP client and performs NO
    // redaction of its own. That is only safe because Logger::Log redacts at ingest.
    // This pins the property the command depends on: if RedactLogLine ever stopped
    // covering a secret shape, the selection would happily forward it.
    const std::string secret = "ghp_0123456789abcdefghijklmnopqrstuvwxyzAB";
    const std::string redacted = smatchet::privacy::RedactLogLine("auth header token=" + secret);
    CHECK(redacted.find(secret) == std::string::npos);

    std::vector<LogEntry> entries;
    entries.push_back(Entry(LogLevel::Info, redacted));
    const std::vector<LogEntry> got = SelectLogTail(entries, LogLevel::Trace, "", 10, nullptr);
    REQUIRE(got.size() == 1u);
    CHECK(got[0].message.find(secret) == std::string::npos);
}
