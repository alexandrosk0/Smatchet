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
    // Separator is part of the budgeted candidate — appending it before the cap math let
    // out.size() exceed the cap and underflow the subtraction below (review finding).
    const std::string candidate = out.empty() ? piece : "; " + piece;
    if (out.size() + candidate.size() <= kMaxJoinedErrorLen) {
        out += candidate;
        return;
    }
    // Entry guard keeps out.size() < kMaxJoinedErrorLen, so cut >= 1, and this branch
    // implies candidate.size() > cut, so candidate[cut] is in range.
    std::size_t cut = kMaxJoinedErrorLen - out.size();
    // Back up to a UTF-8 lead byte so the cap never splits a multi-byte sequence — Jira
    // instances localize errorMessages[], and an invalid-UTF-8 tail would both render as
    // garbage in the toast and make BackendAuditTrail's json dump() throw.
    while (cut > 0 && (static_cast<unsigned char>(candidate[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    out.append(candidate, 0, cut);
    out += "…";
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
