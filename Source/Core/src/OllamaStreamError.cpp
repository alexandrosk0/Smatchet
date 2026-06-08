#include "OllamaStreamError.h"

#include <string>

namespace smatchet {
namespace ai {
namespace pure {

std::string FormatOllamaTransportError(const std::string& cprMessage) {
    return std::string("transport: ") + cprMessage;
}

std::string FormatOllamaHttpError(long statusCode, const std::string& sanitisedBody) {
    std::string msg = std::string("HTTP ") + std::to_string(statusCode);
    if (!sanitisedBody.empty()) {
        msg.append(": ");
        msg.append(sanitisedBody);
    }
    return msg;
}

} // namespace pure
} // namespace ai
} // namespace smatchet
