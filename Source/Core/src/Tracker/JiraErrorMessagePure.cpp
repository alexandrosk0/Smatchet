#include "Tracker/JiraErrorMessagePure.h"

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

namespace smatchet {
namespace jira {

namespace {

// Cap on the joined message so a server that stuffs hundreds of field errors into one
// response cannot flood a toast. Generous enough for several real validation messages.
constexpr std::size_t kMaxJoinedErrorLen = 400;

void AppendCapped(std::string& out, const std::string& piece) {
    if (piece.empty() || out.size() >= kMaxJoinedErrorLen) {
        return;
    }
    if (!out.empty()) {
        out += "; ";
    }
    if (out.size() + piece.size() > kMaxJoinedErrorLen) {
        out.append(piece, 0, kMaxJoinedErrorLen - out.size());
        out += "…";
        return;
    }
    out += piece;
}

} // namespace

std::string ExtractJiraErrorMessage(int httpStatus, const std::string& body) {
    const std::string fallback = "HTTP " + std::to_string(httpStatus);
    if (body.empty()) {
        return fallback;
    }
    // `body` is the untrusted HTTP error response — bounded parse (discarded on failure) so a
    // depth bomb in an error body can't crash the process. Mirrors ExtractLinearErrorMessage.
    const nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(body);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return fallback;
    }

    std::string joined;
    const auto messagesIt = parsed.find("errorMessages");
    if (messagesIt != parsed.end() && messagesIt->is_array()) {
        for (const nlohmann::json& msg : *messagesIt) {
            if (msg.is_string()) {
                AppendCapped(joined, msg.get<std::string>());
            }
        }
    }
    const auto errorsIt = parsed.find("errors");
    if (errorsIt != parsed.end() && errorsIt->is_object()) {
        for (auto it = errorsIt->begin(); it != errorsIt->end(); ++it) {
            if (it.value().is_string()) {
                AppendCapped(joined, it.key() + ": " + it.value().get<std::string>());
            }
        }
    }

    if (joined.empty()) {
        return fallback;
    }
    return fallback + ": " + joined;
}

} // namespace jira
} // namespace smatchet
