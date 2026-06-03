// Bucket-A coverage for the pure log-line formatting / clipboard-join helpers extracted from
// SmatchetUI::drawLogWindow (Source/Core/src/Ui/SmatchetLog_detail.cpp). No ImGui — pure
// functions of their arguments. Goldens built from real LogEntry records, not hand-computed.

#include <doctest/doctest.h>

#include "Ui/SmatchetLog_detail.h"

using SmatchetLog::detail::AppendLogEntryAsLines;
using SmatchetLog::detail::FillLogViewLinesFromEntries;
using SmatchetLog::detail::JoinLogLines;

namespace {

LogEntry MakeEntry(LogLevel level, const std::string& message) {
    LogEntry e;
    e.level = level;
    e.message = message;
    return e;
}

} // namespace

TEST_CASE("AppendLogEntryAsLines tags first line, pads continuations") {
    std::vector<std::string> out;
    AppendLogEntryAsLines(out, "[INFO ] ", "single line");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "[INFO ] single line");

    out.clear();
    AppendLogEntryAsLines(out, "[WARN ] ", "first\nsecond\nthird");
    REQUIRE(out.size() == 3);
    CHECK(out[0] == "[WARN ] first");
    CHECK(out[1] == "        second"); // 8-space pad
    CHECK(out[2] == "        third");
}

TEST_CASE("AppendLogEntryAsLines preserves a trailing newline as an empty padded line") {
    std::vector<std::string> out;
    AppendLogEntryAsLines(out, "[ERROR] ", "boom\n");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "[ERROR] boom");
    CHECK(out[1] == "        ");
}

TEST_CASE("FillLogViewLinesFromEntries maps each level to its tag and clears first") {
    std::vector<LogEntry> entries;
    entries.push_back(MakeEntry(LogLevel::Trace, "t"));
    entries.push_back(MakeEntry(LogLevel::Debug, "d"));
    entries.push_back(MakeEntry(LogLevel::Info, "i"));
    entries.push_back(MakeEntry(LogLevel::Warn, "w"));
    entries.push_back(MakeEntry(LogLevel::Error, "e"));

    std::vector<std::string> out;
    out.push_back("stale"); // must be cleared
    FillLogViewLinesFromEntries(entries, out);
    REQUIRE(out.size() == 5);
    CHECK(out[0] == "[TRACE] t");
    CHECK(out[1] == "[DEBUG] d");
    CHECK(out[2] == "[INFO ] i");
    CHECK(out[3] == "[WARN ] w");
    CHECK(out[4] == "[ERROR] e");
}

TEST_CASE("JoinLogLines joins with newlines and no trailing separator") {
    std::vector<std::string> empty;
    CHECK(JoinLogLines(empty).empty());

    std::vector<std::string> one;
    one.push_back("only");
    CHECK(JoinLogLines(one) == "only");

    std::vector<std::string> three;
    three.push_back("a");
    three.push_back("b");
    three.push_back("c");
    CHECK(JoinLogLines(three) == "a\nb\nc");
}
