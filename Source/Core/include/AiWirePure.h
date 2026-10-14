// AiWirePure.h — the provider-neutral request plumbing every AI client shares:
// endpoint URL assembly and the chat `messages` array build. Header-only and
// cpr / httplib / SQLite-free, so the doctest rig tests it without the HTTP
// layer. Provider policy (default base URLs, OpenAI's `/v1` trim, where the
// system prompt goes) stays in each client.

#ifndef SMATCHET_AI_WIRE_PURE_H
#define SMATCHET_AI_WIRE_PURE_H

#include "AiTypes.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace ai {
namespace pure {
namespace detail {

// Length of `s` once its trailing run of '/' is dropped.
inline std::size_t LengthWithoutTrailingSlashes(const std::string& s) {
    std::size_t n = s.size();
    while (n > 0 && s[n - 1] == '/')
        --n;
    return n;
}

// Appends one {role, content} entry per History turn. Only Role and Content
// reach the wire; AiMessage's local-only members (timestamp, pin) never do.
inline void AppendHistoryMessages(nlohmann::json& messages, const std::vector<AiMessage>& history) {
    for (const auto& h : history) {
        nlohmann::json m;
        m["role"] = h.Role;
        m["content"] = h.Content;
        messages.push_back(std::move(m));
    }
}

} // namespace detail

// Joins a base URL and an endpoint path with exactly one '/' between them:
// every trailing '/' of `base` and every leading '/' of `path` is dropped, then
// one '/' is inserted. An empty `path` (or nullptr) yields the trimmed base plus
// a single '/'; an empty (or all-'/') base yields "/" + path.
inline std::string JoinUrl(const std::string& base, const char* path) {
    const std::size_t baseLen = detail::LengthWithoutTrailingSlashes(base);
    const char* tail = (path != nullptr) ? path : "";
    while (*tail == '/')
        ++tail;
    const std::size_t tailLen = std::strlen(tail);

    std::string url;
    url.reserve(baseLen + 1 + tailLen);
    url.append(base, 0, baseLen);
    url.push_back('/');
    url.append(tail, tailLen);
    return url;
}

// The user-configured base URL when set, else the provider's default, with
// every trailing '/' dropped so a provider's suffix rule (OpenAI's "/v1" trim)
// sees the bare base. A set but malformed base (say "///") is kept, never
// swapped for the default: that would send the prompt to a host the user did
// not choose.
inline std::string ResolveBaseUrlOr(const std::string& configured, const char* defaultBase) {
    std::string base = !configured.empty() ? configured : std::string((defaultBase != nullptr) ? defaultBase : "");
    base.resize(detail::LengthWithoutTrailingSlashes(base));
    return base;
}

// The `messages` array holding only the History turns (Anthropic: the system
// prompt is a top-level body field there, never a message).
inline nlohmann::json BuildHistoryMessages(const std::vector<AiMessage>& history) {
    nlohmann::json messages = nlohmann::json::array();
    detail::AppendHistoryMessages(messages, history);
    return messages;
}

// The OpenAI-chat `messages` array (OpenAI-compatible endpoints and Ollama's
// /api/chat): a leading {role: "system"} entry when `systemPrompt` is
// non-empty, then the History turns.
inline nlohmann::json BuildChatMessages(const std::string& systemPrompt, const std::vector<AiMessage>& history) {
    nlohmann::json messages = nlohmann::json::array();
    if (!systemPrompt.empty()) {
        nlohmann::json sys;
        sys["role"] = "system";
        sys["content"] = systemPrompt;
        messages.push_back(std::move(sys));
    }
    detail::AppendHistoryMessages(messages, history);
    return messages;
}

} // namespace pure
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_WIRE_PURE_H
