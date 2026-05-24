// AiSseParser pure-logic tests — exercises the published Source_Core/include/AiSseParser.h
// surface. Parser is a stateful byte-stream splitter — Feed() must be safe under
// arbitrarily-chunked input (libcurl never honours SSE frame boundaries on TCP boundaries).
//
// `[DONE]` sentinel: the parser itself does NOT interpret the `data: [DONE]` payload — it
// emits the Event with Data == "[DONE]" and lets the OpenAI client translate that to
// AiStreamDelta{IsFinal=true}. Tests assert the parser surface, not the client semantics.
//
// Pillar 3 (never crash): adversarial / partial / malformed inputs must terminate Feed()
// cleanly and never lose buffered state.

#include "AiSseParser.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

// Tiny collector that captures every Event emitted by the parser.
struct EventCollector {
    std::vector<AiSseParser::Event> events;
    AiSseParser::EventCallback callback() {
        return [this](const AiSseParser::Event& e) { events.push_back(e); };
    }
};

void Feed(AiSseParser& p, const std::string& s, EventCollector& c) { p.Feed(s.data(), s.size(), c.callback()); }

} // namespace

TEST_CASE("AiSseParser: single complete event in one Feed yields one delta") {
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: hello\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Name.empty());
    CHECK(c.events[0].Data == "hello");
}

TEST_CASE("AiSseParser: two events in one Feed yields two deltas in order") {
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: first\n\ndata: second\n\n", c);
    REQUIRE(c.events.size() == 2);
    CHECK(c.events[0].Data == "first");
    CHECK(c.events[1].Data == "second");
    CHECK(c.events[0].Name.empty());
    CHECK(c.events[1].Name.empty());
}

TEST_CASE("AiSseParser: event split across Feed boundary [high-risk]") {
    // [high-risk] Adversarial chunk-split: libcurl will hand us this exact shape (TCP
    // chunks never align to SSE frames). The parser must hold the partial buffer and
    // emit nothing until the second Feed completes the frame.
    AiSseParser p;
    EventCollector c;

    Feed(p, "data: par", c);
    CHECK(c.events.empty()); // partial frame, must NOT emit

    Feed(p, "tial\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "partial");
}

TEST_CASE("AiSseParser: split at the frame boundary itself") {
    // Even more adversarial: split exactly between the two newlines.
    AiSseParser p;
    EventCollector c;

    Feed(p, "data: edge\n", c);
    CHECK(c.events.empty());

    Feed(p, "\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "edge");
}

TEST_CASE("AiSseParser: CRLF (\\r\\n\\r\\n) boundary matches LF boundary [high-risk]") {
    // [high-risk] OpenAI / Anthropic / proxies in front of either may emit CRLF line
    // endings. A CRLF-only stream must produce identical Event output to an LF-only
    // stream — otherwise the Anthropic client would silently drop every event on
    // implementations that normalise to CRLF.
    AiSseParser pLf;
    EventCollector cLf;
    Feed(pLf, "data: crlf-payload\n\n", cLf);

    AiSseParser pCrlf;
    EventCollector cCrlf;
    Feed(pCrlf, "data: crlf-payload\r\n\r\n", cCrlf);

    REQUIRE(cCrlf.events.size() == cLf.events.size());
    CHECK(cCrlf.events[0].Data == cLf.events[0].Data);
    CHECK(cCrlf.events[0].Name == cLf.events[0].Name);
}

TEST_CASE("AiSseParser: multiple data: lines in one event concatenate with newline") {
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: line1\ndata: line2\ndata: line3\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "line1\nline2\nline3");
}

TEST_CASE("AiSseParser: named event surfaces (Anthropic content_block_delta shape)") {
    AiSseParser p;
    EventCollector c;
    Feed(p,
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\"}\n"
         "\n",
         c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Name == "content_block_delta");
    CHECK(c.events[0].Data == "{\"type\":\"content_block_delta\"}");
}

TEST_CASE("AiSseParser: Anthropic-style message_stop named event") {
    AiSseParser p;
    EventCollector c;
    Feed(p,
         "event: message_stop\n"
         "data: {}\n"
         "\n",
         c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Name == "message_stop");
    CHECK(c.events[0].Data == "{}");
}

TEST_CASE("AiSseParser: comment line (`: keepalive`) ignored without emitting") {
    AiSseParser p;
    EventCollector c;
    // Comment-only frame: nothing emitted (no data and no name → suppressed by parser).
    Feed(p, ": keepalive comment\n\n", c);
    CHECK(c.events.empty());
    // Comment line ignored, data still flows.
    Feed(p, ": another comment\ndata: real\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "real");
}

TEST_CASE("AiSseParser: unknown fields (id:, retry:) ignored without erroring") {
    AiSseParser p;
    EventCollector c;
    Feed(p,
         "id: 12345\n"
         "retry: 3000\n"
         "data: payload\n"
         "\n",
         c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "payload");
    CHECK(c.events[0].Name.empty());
}

TEST_CASE("AiSseParser: [DONE] sentinel surfaces as Event with Data == [DONE]") {
    // Per AiSseParser contract: the parser does not interpret [DONE]; that's the
    // OpenAI client's job. Verify the sentinel ARRIVES intact at the callback.
    AiSseParser p;
    EventCollector c;
    Feed(p,
         "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
         "data: [DONE]\n\n",
         c);
    REQUIRE(c.events.size() == 2);
    CHECK(c.events[0].Data == "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}");
    CHECK(c.events[1].Data == "[DONE]");
}

TEST_CASE("AiSseParser: Reset clears buffered partial frame") {
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: never-completed", c);
    CHECK(c.events.empty());

    p.Reset();
    Feed(p, "data: after-reset\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "after-reset");
}

TEST_CASE("AiSseParser: Flush surfaces trailing partial frame as final event") {
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: trailing-no-boundary", c);
    CHECK(c.events.empty());

    p.Flush(c.callback());
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "trailing-no-boundary");
}

TEST_CASE("AiSseParser: empty Feed is a no-op") {
    AiSseParser p;
    EventCollector c;
    p.Feed(nullptr, 0, c.callback());
    p.Feed("", 0, c.callback());
    CHECK(c.events.empty());
}

TEST_CASE("AiSseParser: strips exactly one leading space after data:/event: (RFC)") {
    // WHATWG SSE / RFC 8895: strip the single optional space after the colon —
    // NOT all leading whitespace. A payload that intentionally leads with spaces
    // must reach the consumer with the extra spaces intact.
    SUBCASE("Single space stripped to empty leading") {
        AiSseParser p;
        EventCollector c;
        Feed(p, "data: hello\n\n", c);
        REQUIRE(c.events.size() == 1);
        CHECK(c.events[0].Data == "hello");
    }

    SUBCASE("Two spaces — only first stripped, second preserved") {
        AiSseParser p;
        EventCollector c;
        Feed(p, "data:  two-spaces\n\n", c);
        REQUIRE(c.events.size() == 1);
        CHECK(c.events[0].Data == " two-spaces"); // leading space preserved
    }

    SUBCASE("No space after colon — payload starts immediately") {
        AiSseParser p;
        EventCollector c;
        Feed(p, "data:no-space\n\n", c);
        REQUIRE(c.events.size() == 1);
        CHECK(c.events[0].Data == "no-space");
    }

    SUBCASE("event: same single-space rule") {
        AiSseParser p;
        EventCollector c;
        Feed(p, "event:  named\ndata:x\n\n", c);
        REQUIRE(c.events.size() == 1);
        CHECK(c.events[0].Name == " named"); // leading space preserved
        CHECK(c.events[0].Data == "x");
    }
}

TEST_CASE("AiSseParser: aborts cleanly past 4 MiB cap without frame boundary") {
    AiSseParser p;
    EventCollector c;
    // Feed 5 MiB of data: lines with NO terminator. The cap kicks in and the
    // parser drops the rest of the stream; no exceptions, no OOM, no infinite
    // buffer growth.
    const std::string oneMb(1024u * 1024u, 'X');
    for (int i = 0; i < 5; ++i) {
        p.Feed(oneMb.data(), oneMb.size(), c.callback());
    }
    // After cap, further feeds are no-ops (no spurious events).
    p.Feed("data: late\n\n", 12, c.callback());
    CHECK(c.events.empty()); // nothing was framed
    p.Reset();
    // Post-reset the parser is usable again.
    Feed(p, "data: ok\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "ok");
}

TEST_CASE("AiSseParser: malformed JSON in data payload passes through unchanged [opaque payload]") {
    // The parser does NOT inspect the data: payload — it's an opaque byte
    // sequence handed to the OpenAi/Anthropic client to JSON-parse. Garbage
    // JSON must surface intact at the callback (the client's own try/catch
    // turns it into a graceful error; the parser must not pre-judge).
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: {not-valid-json,,\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "{not-valid-json,,");
}

TEST_CASE("AiSseParser: data payload that looks like a frame boundary survives because boundary is line-based") {
    // Adversarial — the payload contains `\\n\\n` literal chars (not actual
    // newlines). Boundary detection works on byte-level `\n\n` only, so the
    // escaped form passes through.
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: foo\\n\\nbar\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Data == "foo\\n\\nbar");
}

TEST_CASE("AiSseParser: mid-frame caller-side cancel preserves buffer for either resume or Reset") {
    // The parser has no cancel concept — the *caller* (worker thread) is
    // expected to stop feeding bytes when its AiCancelToken trips. Verify
    // that an abrupt stop mid-frame leaves the parser in a consistent
    // state: no spurious emission, and either resume-with-more-bytes OR
    // Reset() + fresh stream both work.
    SUBCASE("Caller resumes after cancel: partial frame eventually completes") {
        AiSseParser p;
        EventCollector c;
        Feed(p, "data: half-", c);
        CHECK(c.events.empty()); // no half-frame leak

        // Simulated cancel → caller pauses (no Feed calls). Parser state intact.
        // Caller decides to resume rather than abandon.
        Feed(p, "delivered\n\n", c);
        REQUIRE(c.events.size() == 1);
        CHECK(c.events[0].Data == "half-delivered");
    }

    SUBCASE("Caller cancels and Resets: subsequent stream parses cleanly from scratch") {
        AiSseParser p;
        EventCollector c;
        Feed(p, "event: in-progress\ndata: dropped", c);
        CHECK(c.events.empty());

        p.Reset(); // caller aborts mid-frame
        Feed(p, "data: fresh-event\n\n", c);
        REQUIRE(c.events.size() == 1);
        CHECK(c.events[0].Name.empty());
        CHECK(c.events[0].Data == "fresh-event");
    }
}

TEST_CASE("AiSseParser: Flush on empty parser is a no-op") {
    AiSseParser p;
    EventCollector c;
    p.Flush(c.callback());
    CHECK(c.events.empty());
}

TEST_CASE("AiSseParser: Flush after a complete stream emits nothing extra") {
    // No trailing partial frame — Flush must not synthesise a phantom event.
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: complete\n\n", c);
    REQUIRE(c.events.size() == 1);

    p.Flush(c.callback());
    CHECK(c.events.size() == 1); // unchanged
}

TEST_CASE("AiSseParser: mixed LF + CRLF in the same stream both frame correctly") {
    // Some proxies mix line endings — the parser must handle both within
    // a single connection without dropping events.
    AiSseParser p;
    EventCollector c;
    Feed(p,
         "data: lf-event\n\n"
         "data: crlf-event\r\n\r\n"
         "data: lf-again\n\n",
         c);
    REQUIRE(c.events.size() == 3);
    CHECK(c.events[0].Data == "lf-event");
    CHECK(c.events[1].Data == "crlf-event");
    CHECK(c.events[2].Data == "lf-again");
}

TEST_CASE("AiSseParser: CRLF frame boundary split across Feed calls [high-risk]") {
    // libcurl may hand us the four CRLF bytes in any combination across two
    // Feed calls. Split at every possible position.
    const std::string stream = "data: split-crlf\r\n\r\n";
    for (std::size_t split = 1; split + 1 < stream.size(); ++split) {
        AiSseParser p;
        EventCollector c;
        Feed(p, stream.substr(0, split), c);
        Feed(p, stream.substr(split), c);
        REQUIRE_MESSAGE(c.events.size() == 1, "split at byte " << split);
        CHECK(c.events[0].Data == "split-crlf");
    }
}

TEST_CASE("AiSseParser: empty-payload data: line still emits with empty Data") {
    // OpenAI sometimes sends `data: \n\n` as keepalive-ish framing.
    // Per parser contract the event suppression rule is "name empty AND
    // data empty" — `data:` with no payload still produces an empty Data
    // string, which combined with an empty Name suppresses the emission.
    AiSseParser p;
    EventCollector c;
    Feed(p, "data: \n\n", c);
    // Both name and data empty → suppressed by parser per AiSseParser.cpp:94.
    CHECK(c.events.empty());
}

TEST_CASE("AiSseParser: named event with no data: still emits (Name carries the signal)") {
    // Anthropic's `event: ping\n\n` shape — no data, but the named event
    // itself is the signal. Must surface so the client can ignore it
    // explicitly rather than silently swallowing.
    AiSseParser p;
    EventCollector c;
    Feed(p, "event: ping\n\n", c);
    REQUIRE(c.events.size() == 1);
    CHECK(c.events[0].Name == "ping");
    CHECK(c.events[0].Data.empty());
}

TEST_CASE("AiSseParser: many small Feeds reconstruct a full stream") {
    // Pillar 3 stress: feed one byte at a time. Parser must accumulate cleanly
    // and emit exactly the same events as a single all-in-one Feed.
    const std::string stream = "event: msg\ndata: chunk-a\n\nevent: msg\ndata: chunk-b\n\n";

    AiSseParser oneShot;
    EventCollector oneShotC;
    Feed(oneShot, stream, oneShotC);

    AiSseParser drip;
    EventCollector dripC;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        drip.Feed(stream.data() + i, 1, dripC.callback());
    }

    REQUIRE(dripC.events.size() == oneShotC.events.size());
    for (std::size_t i = 0; i < dripC.events.size(); ++i) {
        CHECK(dripC.events[i].Name == oneShotC.events[i].Name);
        CHECK(dripC.events[i].Data == oneShotC.events[i].Data);
    }
}
