#include "AiNdjsonParser.h"

#include "Json/BoundedJsonParse.h"
#include "Logger.h"

#include <cstddef>
#include <exception>
#include <string>

void AiNdjsonParser::emitOneLine(std::size_t begin, std::size_t end, const LineCallback& onLine,
                                 const ErrorCallback& onError) {
    std::size_t actualEnd = end;
    if (actualEnd > begin && buffer_[actualEnd - 1] == '\r')
        --actualEnd;
    if (actualEnd <= begin)
        return; // blank line — silently skipped
    const std::string line(buffer_, begin, actualEnd - begin);

    // Each NDJSON line is server-supplied and attacker-influenced; route it through the
    // depth/node-bounded helper so a hostile / oversized line can't crash the process.
    // ParseBounded never throws and signals failure via a non-empty parseErr.
    std::string parseErr;
    nlohmann::json j = smatchet::json_safe::ParseBounded(line, parseErr);
    if (!parseErr.empty()) {
        if (onError)
            onError(line);
        return;
    }
    if (onLine)
        onLine(j);
}

void AiNdjsonParser::Feed(const char* data, std::size_t len, const LineCallback& onLine, const ErrorCallback& onError) {
    if (overflowed_)
        return; // Stream poisoned; ignore further bytes until Reset.
    if (len > 0 && data != nullptr) {
        if (buffer_.size() + len > kAiNdjsonParserMaxBufferBytes) {
            overflowed_ = true;
            LOG_ERROR("AiNdjsonParser: stream exceeded %zu-byte cap without a newline; aborting parse.",
                      kAiNdjsonParserMaxBufferBytes);
            buffer_.clear();
            return;
        }
        buffer_.append(data, len);
    }

    std::size_t scanFrom = 0;
    while (true) {
        const std::size_t nl = buffer_.find('\n', scanFrom);
        if (nl == std::string::npos)
            break;
        emitOneLine(scanFrom, nl, onLine, onError);
        scanFrom = nl + 1;
    }
    if (scanFrom > 0)
        buffer_.erase(0, scanFrom);
}

void AiNdjsonParser::Flush(const LineCallback& onLine, const ErrorCallback& onError) {
    if (buffer_.empty())
        return;
    emitOneLine(0, buffer_.size(), onLine, onError);
    buffer_.clear();
}

void AiNdjsonParser::Reset() {
    buffer_.clear();
    overflowed_ = false;
}
