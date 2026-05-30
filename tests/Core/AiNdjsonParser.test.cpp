// AiNdjsonParser pure-logic tests — exercises Source/Core/include/AiNdjsonParser.h.
//
// AiNdjsonParser is the line-buffered sibling of AiSseParser, consumed by
// OllamaClient to parse the native `/api/chat` NDJSON stream (one complete
// JSON object per `\n`-terminated line). Network chunking can split any line
// mid-byte, so the parser MUST buffer partial frames across Feed() calls and
// emit each complete line exactly once. Invalid-JSON lines surface to onError
// without breaking the stream (subsequent valid lines still parse).
//
// Coverage matrix (6 cases / 31 assertions, 1 `[high-risk]`):
//   1. Single complete line — one onLine, no onError.
//   2. Multiple complete lines in one Feed — onLine fires per line, in order.
//   3. Line split across two Feeds — partial frame buffered, full delivery on second Feed.
//   4. Blank lines are silently skipped (neither callback fires).
//   5. Invalid-JSON line → onError fires with raw line; subsequent valid lines still parse [high-risk].
//   6. Reset() clears partial-frame buffer mid-stream.

#include "AiNdjsonParser.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace {

struct CollectingSink {
    std::vector<nlohmann::json> lines;
    std::vector<std::string> errors;

    AiNdjsonParser::LineCallback onLine() {
        return [this](const nlohmann::json& j) { this->lines.push_back(j); };
    }

    AiNdjsonParser::ErrorCallback onError() {
        return [this](const std::string& raw) { this->errors.push_back(raw); };
    }
};

void FeedString(AiNdjsonParser& parser, const std::string& bytes, CollectingSink& sink) {
    parser.Feed(bytes.data(), bytes.size(), sink.onLine(), sink.onError());
}

} // namespace

TEST_CASE("AiNdjsonParser: single complete line emits exactly one parsed JSON object") {
    AiNdjsonParser parser;
    CollectingSink sink;

    FeedString(parser, "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello\"},\"done\":false}\n", sink);

    REQUIRE(sink.lines.size() == 1);
    CHECK(sink.errors.empty());
    CHECK(sink.lines[0].is_object());
    REQUIRE(sink.lines[0].contains("message"));
    REQUIRE(sink.lines[0]["message"].is_object());
    CHECK(sink.lines[0]["message"]["content"].get<std::string>() == "Hello");
    CHECK(sink.lines[0]["done"].get<bool>() == false);
}

TEST_CASE("AiNdjsonParser: multiple lines in one Feed are emitted in order") {
    AiNdjsonParser parser;
    CollectingSink sink;

    const std::string blob = "{\"message\":{\"content\":\"Hello\"},\"done\":false}\n"
                             "{\"message\":{\"content\":\" world\"},\"done\":false}\n"
                             "{\"message\":{\"content\":\"\"},\"done\":true}\n";
    FeedString(parser, blob, sink);

    REQUIRE(sink.lines.size() == 3);
    CHECK(sink.errors.empty());
    CHECK(sink.lines[0]["message"]["content"].get<std::string>() == "Hello");
    CHECK(sink.lines[1]["message"]["content"].get<std::string>() == " world");
    CHECK(sink.lines[2]["done"].get<bool>() == true);
    CHECK(sink.lines[2]["message"]["content"].get<std::string>() == "");
}

TEST_CASE("AiNdjsonParser: line split across two Feed calls buffers correctly") {
    AiNdjsonParser parser;
    CollectingSink sink;

    // First Feed: the line has no terminating newline yet — parser must buffer.
    FeedString(parser, "{\"message\":{\"content\":\"Hel", sink);
    CHECK(sink.lines.empty());
    CHECK(sink.errors.empty());

    // Second Feed: completes the first line + starts a second incomplete one.
    FeedString(parser, "lo\"},\"done\":false}\n{\"done\":tr", sink);
    REQUIRE(sink.lines.size() == 1);
    CHECK(sink.errors.empty());
    CHECK(sink.lines[0]["message"]["content"].get<std::string>() == "Hello");

    // Third Feed: completes the second line.
    FeedString(parser, "ue}\n", sink);
    REQUIRE(sink.lines.size() == 2);
    CHECK(sink.errors.empty());
    CHECK(sink.lines[1]["done"].get<bool>() == true);
}

TEST_CASE("AiNdjsonParser: blank lines are silently skipped") {
    AiNdjsonParser parser;
    CollectingSink sink;

    // Three blank lines (LF + CRLF mix) interleaved with one valid line.
    FeedString(parser, "\n\r\n{\"done\":true}\n\n", sink);

    REQUIRE(sink.lines.size() == 1);
    CHECK(sink.errors.empty());
    CHECK(sink.lines[0]["done"].get<bool>() == true);
}

TEST_CASE("AiNdjsonParser: invalid JSON line surfaces to onError, subsequent valid lines still parse [high-risk]") {
    AiNdjsonParser parser;
    CollectingSink sink;

    // Three lines: garbage, valid, then another garbage. Parser must not get stuck.
    const std::string blob = "{not json at all\n"
                             "{\"message\":{\"content\":\"recovered\"},\"done\":false}\n"
                             "</html>\n"
                             "{\"done\":true}\n";
    FeedString(parser, blob, sink);

    REQUIRE(sink.lines.size() == 2);
    REQUIRE(sink.errors.size() == 2);
    CHECK(sink.errors[0] == "{not json at all");
    CHECK(sink.errors[1] == "</html>");
    CHECK(sink.lines[0]["message"]["content"].get<std::string>() == "recovered");
    CHECK(sink.lines[1]["done"].get<bool>() == true);
}

TEST_CASE("AiNdjsonParser: aborts cleanly past 4 MiB cap without newline") {
    AiNdjsonParser parser;
    CollectingSink sink;
    // Feed 5 MiB with no newline at all — cap engages, stream poisoned until
    // Reset, no OOM, no callbacks.
    const std::string oneMb(1024u * 1024u, 'X');
    for (int i = 0; i < 5; ++i) {
        parser.Feed(oneMb.data(), oneMb.size(), sink.onLine(), sink.onError());
    }
    CHECK(sink.lines.empty());
    CHECK(sink.errors.empty());
    // Post-cap feeds are no-ops.
    FeedString(parser, "{\"x\":1}\n", sink);
    CHECK(sink.lines.empty());
    // Reset restores the parser to a usable state.
    parser.Reset();
    FeedString(parser, "{\"x\":1}\n", sink);
    REQUIRE(sink.lines.size() == 1);
    CHECK(sink.lines[0]["x"].get<int>() == 1);
}

TEST_CASE("AiNdjsonParser: Reset() clears partial-frame buffer mid-stream") {
    AiNdjsonParser parser;
    CollectingSink sink;

    // Prime with half a line — no callback should fire yet.
    FeedString(parser, "{\"junk\":\"abandoned mid-line", sink);
    CHECK(sink.lines.empty());
    CHECK(sink.errors.empty());

    // Reset discards the half-line. Now feed a fresh, valid line on its own.
    parser.Reset();
    FeedString(parser, "{\"done\":true}\n", sink);

    REQUIRE(sink.lines.size() == 1);
    CHECK(sink.errors.empty()); // discarded half-line never surfaced as a parse error
    CHECK(sink.lines[0]["done"].get<bool>() == true);
}
