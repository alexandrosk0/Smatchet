#include "AiNdjsonParser.h"

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

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const std::exception&) {
        if (onError)
            onError(line);
        return;
    }
    if (onLine)
        onLine(j);
}

void AiNdjsonParser::Feed(const char* data, std::size_t len, const LineCallback& onLine,
                          const ErrorCallback& onError) {
    if (len > 0 && data != nullptr)
        buffer_.append(data, len);

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

void AiNdjsonParser::Reset() { buffer_.clear(); }
