#pragma once

#include "AiTypes.h"

#include <nlohmann/json_fwd.hpp>
#include <string>

// Single-source wire introspection: thin public wrappers over each AI client's
// internal request-body builder + chat-URL resolver. `ai.dump-request` (and its
// tests) call these so the reported request reflects the EXACT wire body/URL the
// live client would send — there is no separate debug "mirror" to drift out of
// sync (the pre-existing BuildAnthropicBody/BuildOpenAiBody/... mirrors, which
// dropped History and duplicated the /v1-suffix stripping, are retired). Each
// wrapper delegates to the same anonymous-namespace builder the client's own
// dispatch path uses, so introspection can never diverge from production.
namespace smatchet {
namespace ai {

// Request-body JSON for each wire dialect. `req.History` carries the full turn
// list; Temperature < 0 / MaxTokens == 0 are "unset" sentinels (omitted / default).
nlohmann::json OpenAiBuildChatBodyJson(const AiChatRequest& req);
nlohmann::json AnthropicBuildChatBodyJson(const AiChatRequest& req);
nlohmann::json OllamaNativeBuildChatBodyJson(const AiChatRequest& req);

// Fully-resolved chat endpoint URL for each provider (base-URL fallback + suffix
// handling included, via the client's own ResolveBaseUrl/JoinUrl).
std::string OpenAiResolveChatUrl(const AiClientConfig& cfg);
std::string AnthropicResolveChatUrl(const AiClientConfig& cfg);
std::string OllamaNativeResolveChatUrl(const AiClientConfig& cfg);

} // namespace ai
} // namespace smatchet
