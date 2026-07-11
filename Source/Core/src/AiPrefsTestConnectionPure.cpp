#if defined(SMATCHET_WITH_AI)

#include "AiPrefsTestConnectionPure.h"

#include "Logger.h"

#include <algorithm>
#include <iterator>

namespace AiPrefsTestConnectionPure {

ProbeTarget ResolveProbeTarget(const TrackerConfig& cfg, AiProvider provider) {
    ProbeTarget t;
    switch (provider) {
    case AiProvider::Anthropic:
        t.ApiKey = cfg.AiAnthropicApiKey;
        t.BaseUrl = cfg.AiBaseUrl;
        t.ModelId = cfg.AiModelAnthropic;
        break;
    case AiProvider::OllamaNative:
        t.ApiKey.clear();
        t.BaseUrl = cfg.AiOllamaBaseUrl;
        t.ModelId = cfg.AiModelOllama;
        break;
    case AiProvider::OllamaOpenAiCompat:
        t.ApiKey = cfg.AiApiKey;
        t.BaseUrl = cfg.AiBaseUrl.empty() ? cfg.AiOllamaBaseUrl : cfg.AiBaseUrl;
        t.ModelId = cfg.AiModelOpenAi;
        break;
    case AiProvider::DeepSeek:
        t.ApiKey = cfg.AiDeepSeekApiKey;
        t.BaseUrl = cfg.AiDeepSeekBaseUrl;
        t.ModelId = cfg.AiModelDeepSeek;
        break;
    case AiProvider::OpenAi:
    default:
        t.ApiKey = cfg.AiApiKey;
        t.BaseUrl = cfg.AiBaseUrl;
        t.ModelId = cfg.AiModelOpenAi;
        break;
    }
    return t;
}

std::string DefaultBaseUrlFor(AiProvider provider) {
    if (provider == AiProvider::OllamaOpenAiCompat) {
        return "http://127.0.0.1:1234";
    }
    if (provider == AiProvider::OllamaNative) {
        return "http://localhost:11434";
    }
    if (provider == AiProvider::DeepSeek) {
        return "https://api.deepseek.com";
    }
    return std::string();
}

AiClientConfig BuildProbeClientConfig(const std::string& apiKey, const std::string& baseUrl,
                                      const smatchet::ai::pure::EndpointPolicy& policy) {
    std::string sanitisedKey;
    sanitisedKey.reserve(apiKey.size());
    std::copy_if(apiKey.begin(), apiKey.end(), std::back_inserter(sanitisedKey),
                 [](char c) { return c != '\r' && c != '\n' && c != '\0'; });
    std::string sanitisedBase;
    if (!baseUrl.empty()) {
        std::string normalised;
        const smatchet::ai::pure::EndpointVerdict v =
            smatchet::ai::pure::SanitizeAiEndpointUrl(baseUrl, policy, normalised);
        if (v == smatchet::ai::pure::EndpointVerdict::Allowed) {
            sanitisedBase = normalised;
        } else {
            LOG_WARN("AiPrefsTestConnection: endpoint URL %s; falling back to provider default.",
                     smatchet::ai::pure::EndpointVerdictDescription(v));
        }
    }
    AiClientConfig clientCfg;
    clientCfg.ApiKey = sanitisedKey;
    clientCfg.BaseUrl = sanitisedBase;
    clientCfg.ConnectTimeoutMs = 5000;
    clientCfg.TotalTimeoutMs = 15000;
    return clientCfg;
}

ProbePlan PlanProbe(const TrackerConfig& cfg, AiProvider provider, const smatchet::ai::pure::EndpointPolicy& policy) {
    ProbeTarget target = ResolveProbeTarget(cfg, provider);

    // When the configured base URL is empty for a local provider, fall back to the
    // canonical default so the user can click Test connection right after picking
    // the provider — no manual URL entry needed. The default is also recorded so
    // the success callback can persist it back into cfg.
    ProbePlan plan;
    if (target.BaseUrl.empty()) {
        plan.DefaultedBaseUrl = DefaultBaseUrlFor(provider);
        target.BaseUrl = plan.DefaultedBaseUrl;
    }
    plan.ClientCfg = BuildProbeClientConfig(target.ApiKey, target.BaseUrl, policy);
    plan.ModelId = target.ModelId;
    return plan;
}

} // namespace AiPrefsTestConnectionPure

#endif // SMATCHET_WITH_AI
