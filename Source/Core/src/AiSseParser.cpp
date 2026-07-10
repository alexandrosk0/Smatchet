#include "AiSseParser.h"

#include "Logger.h"

#include <cstddef>
#include <string>

namespace {

std::size_t FindFrameBoundary(const std::string& buf, std::size_t from, std::size_t& boundaryLen) {
    const std::size_t a = buf.find("\n\n", from);
    const std::size_t b = buf.find("\r\n\r\n", from);
    if (a == std::string::npos && b == std::string::npos)
        return std::string::npos;
    if (a == std::string::npos) {
        boundaryLen = 4;
        return b;
    }
    if (b == std::string::npos) {
        boundaryLen = 2;
        return a;
    }
    if (a < b) {
        boundaryLen = 2;
        return a;
    }
    boundaryLen = 4;
    return b;
}

void AppendDataLine(std::string& acc, const std::string& payload) {
    if (!acc.empty())
        acc.push_back('\n');
    acc.append(payload);
}

} // namespace

void AiSseParser::Feed(const char* data, std::size_t len, const EventCallback& onEvent) {
    if (overflowed_)
        return; // Stream poisoned; ignore further bytes until Reset.
    if (len > 0) {
        if (buffer_.size() + len > kAiSseParserMaxBufferBytes) {
            overflowed_ = true;
            LOG_ERROR("AiSseParser: stream exceeded %zu-byte cap without a frame boundary; aborting parse.",
                      kAiSseParserMaxBufferBytes);
            buffer_.clear();
            return;
        }
        buffer_.append(data, len);
    }

    // Issue #1596: this used to `buffer_.erase(0, end + boundaryLen)` after EVERY
    // extracted frame, then rescan from offset 0 — an O(n) erase/rescan per frame,
    // O(n^2) total on a single Feed() call packed with many small frames (e.g. a
    // fuzzer-grown "\n\n\n\n..." input), which fits the fuzz-smoke report's "crash,
    // leak, or timeout" category (a libFuzzer per-input hang). Fixed: track a
    // monotonically-advancing `consumed` offset instead — every find() scans only
    // the not-yet-consumed tail, and the whole consumed prefix is erased ONCE after
    // the loop, making one Feed() call O(buffer size) instead of O(buffer size^2).
    std::size_t consumed = 0;
    while (true) {
        std::size_t boundaryLen = 0;
        const std::size_t end = FindFrameBoundary(buffer_, consumed, boundaryLen);
        if (end == std::string::npos)
            break;

        Event ev;
        std::size_t lineStart = consumed;
        while (lineStart <= end) {
            std::size_t lineEnd = buffer_.find('\n', lineStart);
            if (lineEnd == std::string::npos || lineEnd > end)
                lineEnd = end;
            std::size_t actualEnd = lineEnd;
            if (actualEnd > lineStart && buffer_[actualEnd - 1] == '\r')
                --actualEnd;
            if (actualEnd > lineStart) {
                const std::string line(buffer_, lineStart, actualEnd - lineStart);
                if (line[0] == ':') {
                    // Comment line — ignore.
                } else if (line.compare(0, 6, "event:") == 0) {
                    // RFC 8895 / WHATWG SSE: strip exactly one optional leading
                    // space after the colon — not all of them. Stripping multiple
                    // would corrupt payloads that intentionally lead with spaces.
                    std::size_t p = 6;
                    if (p < line.size() && line[p] == ' ')
                        ++p;
                    ev.Name.assign(line, p, line.size() - p);
                } else if (line.compare(0, 5, "data:") == 0) {
                    std::size_t p = 5;
                    if (p < line.size() && line[p] == ' ')
                        ++p;
                    AppendDataLine(ev.Data, line.substr(p));
                }
                // Ignore id:, retry:, and unknown fields.
            }
            if (lineEnd == end)
                break;
            lineStart = lineEnd + 1;
        }

        if (!ev.Name.empty() || !ev.Data.empty())
            onEvent(ev);

        consumed = end + boundaryLen;
    }

    if (consumed > 0)
        buffer_.erase(0, consumed);
}

void AiSseParser::Flush(const EventCallback& onEvent) {
    (void)onEvent;
    // A residual buffer at Flush time is an incomplete trailing SSE frame: the
    // server never emitted its terminating blank line. Synthesizing a `\n\n`
    // boundary here would deliver a half-received frame as a real token, which
    // for streaming LLM responses means emitting a truncated/garbled final
    // chunk. Drop the partial frame instead — a well-formed stream always ends
    // on a frame boundary, so there is nothing legitimate to flush.
    buffer_.clear();
}

void AiSseParser::Reset() {
    buffer_.clear();
    overflowed_ = false;
}
