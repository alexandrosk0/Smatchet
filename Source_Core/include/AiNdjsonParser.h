#ifndef SMATCHET_AI_NDJSON_PARSER_H
#define SMATCHET_AI_NDJSON_PARSER_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <string>

/// Stateful newline-delimited JSON (NDJSON) byte-stream parser used by `OllamaClient`.
///
/// Sibling to `AiSseParser`. Ollama's native `/api/chat` endpoint streams one
/// complete JSON object per `\n`-terminated line (no `data:` prefix, no blank-line
/// frame separator). libcurl chunks the byte stream on TCP boundaries, never on
/// line boundaries, so Feed() may be called with arbitrarily-split bytes. The
/// parser buffers until a complete line is available, parses it as JSON, then
/// invokes onLine with the parsed object.
///
/// Invalid-JSON lines surface via onError with the raw line so callers can
/// `LOG_WARN` without breaking the stream — subsequent valid lines still parse.
/// Blank lines (lone `\n` or `\r\n`) are silently skipped (neither callback).
class AiNdjsonParser {
  public:
    using LineCallback = std::function<void(const nlohmann::json&)>;
    using ErrorCallback = std::function<void(const std::string& /*raw line*/)>;

    AiNdjsonParser() = default;

    /// Append `len` bytes of stream data, emitting completed lines.
    void Feed(const char* data, std::size_t len, const LineCallback& onLine, const ErrorCallback& onError);

    /// Flush any trailing in-progress line as a final line (treat EOF as a line terminator).
    /// Blank trailing data is silently dropped.
    void Flush(const LineCallback& onLine, const ErrorCallback& onError);

    /// Clear the partial-frame buffer. Use between independent streams.
    void Reset();

  private:
    std::string buffer_;

    // Parse + dispatch one buffered line in range [begin, end) (newline excluded).
    // Strips trailing \r; drops blank lines silently.
    void emitOneLine(std::size_t begin, std::size_t end, const LineCallback& onLine, const ErrorCallback& onError);
};

#endif
