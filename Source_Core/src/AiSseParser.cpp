#include "AiSseParser.h"

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
    if (len > 0)
        buffer_.append(data, len);

    std::size_t scanFrom = 0;
    while (true) {
        std::size_t boundaryLen = 0;
        const std::size_t end = FindFrameBoundary(buffer_, scanFrom, boundaryLen);
        if (end == std::string::npos)
            break;

        Event ev;
        std::size_t lineStart = 0;
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
                    std::size_t p = 6;
                    while (p < line.size() && line[p] == ' ')
                        ++p;
                    ev.Name.assign(line, p, line.size() - p);
                } else if (line.compare(0, 5, "data:") == 0) {
                    std::size_t p = 5;
                    while (p < line.size() && line[p] == ' ')
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

        buffer_.erase(0, end + boundaryLen);
        scanFrom = 0;
    }
}

void AiSseParser::Flush(const EventCallback& onEvent) {
    if (buffer_.empty())
        return;
    // Treat any trailing buffered content as a final partial frame by appending the boundary.
    buffer_.append("\n\n");
    Feed(nullptr, 0, onEvent);
    buffer_.clear();
}

void AiSseParser::Reset() {
    buffer_.clear();
    partial_ = Event();
}

void AiSseParser::emitIfReady(const EventCallback&) {}
