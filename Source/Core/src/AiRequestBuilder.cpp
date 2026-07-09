#include "AiRequestBuilder.h"

#include "AiEndpointPolicy.h"
#include "AiEndpointSanitize.h"
#include "ConfigManager.h"
#include "Logger.h"

#include <algorithm>
#include <iterator>

namespace smatchet {
namespace ai {

namespace {

// Validate + (best-effort) normalise the user-configured endpoint URL against
// `policy`. Returns the sanitised URL on success; empty + a structured warning on
// rejection (the empty string causes the provider client to fall back to its
// built-in default, which is the safe choice).
std::string SanitizeBaseUrlOrLog(const std::string& raw, const char* providerLabel,
                                 const smatchet::ai::pure::EndpointPolicy& policy) {
    std::string normalised;
    const smatchet::ai::pure::EndpointVerdict v = smatchet::ai::pure::SanitizeAiEndpointUrl(raw, policy, normalised);
    if (v == smatchet::ai::pure::EndpointVerdict::Allowed)
        return normalised;
    LOG_ERROR("AiRequestBuilder: %s endpoint URL %s — falling back to provider default. Raw URL withheld.",
              providerLabel, smatchet::ai::pure::EndpointVerdictDescription(v));
    return std::string();
}

} // namespace

std::string SanitizeHeaderValue(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    std::copy_if(value.begin(), value.end(), std::back_inserter(out),
                 [](char c) { return c != '\r' && c != '\n' && c != '\0'; });
    return out;
}

AiClientConfig BuildClientConfig(const TrackerConfig& cfg, AiProvider provider) {
    AiClientConfig out;
    const smatchet::ai::pure::EndpointPolicy policy = EndpointPolicyForProvider(cfg, provider);
    switch (provider) {
    case AiProvider::Anthropic:
        out.ApiKey = SanitizeHeaderValue(cfg.AiAnthropicApiKey);
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiBaseUrl, "anthropic", policy);
        break;
    case AiProvider::OllamaNative:
        out.ApiKey.clear(); // Ollama-native has no API key
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiOllamaBaseUrl, "ollama", policy);
        break;
    case AiProvider::OllamaOpenAiCompat:
        out.ApiKey = SanitizeHeaderValue(cfg.AiApiKey);
        // OllamaOpenAi-compat uses the user's base URL (typically http://localhost:11434/v1)
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiBaseUrl.empty() ? cfg.AiOllamaBaseUrl : cfg.AiBaseUrl,
                                           "ollama-openai-compat", policy);
        break;
    case AiProvider::DeepSeek:
        out.ApiKey = SanitizeHeaderValue(cfg.AiDeepSeekApiKey);
        // DeepSeek default endpoint when the user leaves the URL blank. Pass
        // the literal default through the sanitiser too — never bypass the
        // SanitizeAiEndpointUrl gate so a future redirect-block / scheme-pin
        // rule applies uniformly.
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiDeepSeekBaseUrl.empty() ? std::string("https://api.deepseek.com")
                                                                         : cfg.AiDeepSeekBaseUrl,
                                           "deepseek", policy);
        break;
    case AiProvider::OpenAi:
    default:
        out.ApiKey = SanitizeHeaderValue(cfg.AiApiKey);
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiBaseUrl, "openai", policy);
        break;
    }
    // Streaming chat replies from reasoning-tuned models routinely exceed the
    // AiClientConfig default of 120s. cpr's `cpr::Timeout` is a total-envelope cap
    // and fires regardless of stream progress, so a long reply gets killed
    // mid-stream. 10 minutes for the chat path; the user-driven Cancel atom is the
    // right abort mechanism for a genuinely stuck stream.
    out.TotalTimeoutMs = 600000;
    return out;
}

} // namespace ai
} // namespace smatchet
