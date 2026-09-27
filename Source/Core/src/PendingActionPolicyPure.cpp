#include "PendingActionPolicyPure.h"

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <cstddef>

namespace smatchet {
namespace pendingaction {

namespace {

// A comment payload is one short object; anything larger was not written by this queue.
constexpr std::size_t kMaxPayloadBytes = 1024u * 1024u;

bool IsAsciiAlnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// CRLF → LF and surrounding whitespace trimmed: the fallback comparison for a body with no
// letters or digits (e.g. "+1" is covered by the digit; ":)" is not).
std::string TrimmedLf(const std::string& text) {
    std::string lf;
    lf.reserve(text.size());
    for (const char c : text) {
        if (c != '\r') {
            lf.push_back(c);
        }
    }
    const std::size_t first = lf.find_first_not_of(" \t\n");
    if (first == std::string::npos) {
        return std::string();
    }
    return lf.substr(first, lf.find_last_not_of(" \t\n") - first + 1);
}

} // namespace

const char* StateAfterFailedSend(PendingActionKind kind, const TrackerError& error) {
    if (!error.IsRetryable()) {
        return "";
    }
    if (kind == PendingActionKind::WatchAdd || error.Kind == TrackerErrorKind::RateLimited) {
        return PendingActionState::kPending;
    }
    return PendingActionState::kAmbiguous;
}

std::string NormalizeCommentForDedupe(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x80) {
            out.push_back(ch);
        } else if (IsAsciiAlnum(c)) {
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : ch);
        }
    }
    return out;
}

bool CommentAlreadyPosted(const std::vector<TrackerIssueComment>& fetched, const std::string& body,
                          std::int64_t queuedAtSec) {
    const std::string wanted = NormalizeCommentForDedupe(body);
    const std::string wantedRaw = wanted.empty() ? TrimmedLf(body) : std::string();
    if (wanted.empty() && wantedRaw.empty()) {
        return false;
    }
    for (const TrackerIssueComment& c : fetched) {
        if (c.CreatedAtSec < queuedAtSec - kCommentDedupeWindowSec) {
            continue;
        }
        if (wanted.empty() ? TrimmedLf(c.Body) == wantedRaw : NormalizeCommentForDedupe(c.Body) == wanted) {
            return true;
        }
    }
    return false;
}

std::string BuildCommentActionPayload(const std::string& body, std::int64_t createdAtSec) {
    nlohmann::json payload = nlohmann::json::object();
    payload["body"] = body;
    payload["created"] = createdAtSec;
    // User-typed text; replace invalid UTF-8 instead of throwing.
    return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool ParseCommentActionPayload(const std::string& json, std::string& outBody, std::int64_t& outCreatedAtSec) {
    std::string err;
    const nlohmann::json parsed = smatchet::json_safe::ParseBounded(json, err, kMaxPayloadBytes);
    if (!err.empty() || !parsed.is_object()) {
        return false;
    }
    const auto body = parsed.find("body");
    if (body == parsed.end() || !body->is_string() || body->get_ref<const std::string&>().empty()) {
        return false;
    }
    const auto created = parsed.find("created");
    outBody = body->get<std::string>();
    outCreatedAtSec = created != parsed.end() && created->is_number_integer() ? created->get<std::int64_t>() : 0;
    return true;
}

} // namespace pendingaction
} // namespace smatchet
