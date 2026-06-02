#include "AiEndpointPolicy.h"

#include "ConfigManager.h"

namespace smatchet {
namespace ai {

pure::EndpointPolicy EndpointPolicyForProvider(const TrackerConfig& cfg, AiProvider provider) {
    pure::EndpointPolicy p;
    switch (provider) {
    case AiProvider::OpenAi:
        p.CanonicalHost = "api.openai.com";
        p.AllowCustomHost = cfg.AiAllowCustomEndpointOpenAi;
        p.AllowInsecureHttp = cfg.AiAllowCustomEndpointOpenAi;
        break;
    case AiProvider::Anthropic:
        p.CanonicalHost = "api.anthropic.com";
        p.AllowCustomHost = cfg.AiAllowCustomEndpointAnthropic;
        p.AllowInsecureHttp = cfg.AiAllowCustomEndpointAnthropic;
        break;
    default:
        break; // Ollama (native + compat) + DeepSeek: unpinned, permissive defaults
    }
    return p;
}

} // namespace ai
} // namespace smatchet
