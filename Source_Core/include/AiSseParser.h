#ifndef SMATCHET_AI_SSE_PARSER_H
#define SMATCHET_AI_SSE_PARSER_H

#include <cstddef>
#include <functional>
#include <string>

/// Stateful Server-Sent-Events byte-stream parser shared by OpenAi/Anthropic clients.
///
/// SSE frames are terminated by a blank line (`\n\n` or `\r\n\r\n`). libcurl
/// chunks the byte stream on TCP boundaries, never on SSE frame boundaries, so
/// Feed() may be called with arbitrarily-split bytes. The parser buffers until
/// a complete frame is available, then invokes onEvent with the event name
/// (empty if absent — OpenAI uses unnamed events) and the concatenated `data:`
/// payload (multiple data lines joined by `\n` per the SSE spec).
class AiSseParser {
  public:
    struct Event {
        std::string Name;
        std::string Data;
    };

    using EventCallback = std::function<void(const Event&)>;

    AiSseParser() = default;

    /// Append `len` bytes of stream data, emitting completed frames via onEvent.
    void Feed(const char* data, std::size_t len, const EventCallback& onEvent);

    /// Flush any in-progress frame as a final event. Call once after EOF/abort.
    void Flush(const EventCallback& onEvent);

    void Reset();

  private:
    std::string buffer_;
    Event partial_;

    void emitIfReady(const EventCallback& onEvent);
};

#endif
