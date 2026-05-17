#include "AiPrefsValidator.h"

#include "AiClientFactory.h"
#include "AiModelCatalog.h"
#include "AiTypes.h"

namespace smatchet {
namespace ai {

namespace {

AiProvider ClampProvider(int kind) {
    switch (kind) {
    case 0:
        return AiProvider::OpenAi;
    case 1:
        return AiProvider::Anthropic;
    case 2:
        return AiProvider::OllamaOpenAiCompat;
    case 3:
        return AiProvider::OllamaNative;
    default:
        return AiProvider::OpenAi;
    }
}

bool StartsWith(const std::string& s, const char* prefix) {
    const std::size_t plen = std::char_traits<char>::length(prefix);
    if (s.size() < plen) {
        return false;
    }
    return s.compare(0, plen, prefix) == 0;
}

bool LooksLikeHttpUrl(const std::string& s) {
    return StartsWith(s, "http://") || StartsWith(s, "https://");
}

} // namespace

PrefsValidation ValidateAiPrefs(const TrackerConfig& cfg) {
    PrefsValidation v;
    const AiProvider provider = ClampProvider(cfg.AiProviderKind);

    // --- Key presence (per active provider) ---
    if (provider == AiProvider::OpenAi || provider == AiProvider::OllamaOpenAiCompat) {
        // OllamaOpenAiCompat reuses the same key slot. Most local-only setups
        // leave it blank — but if the chosen provider is OpenAI proper, the
        // key is required.
        if (provider == AiProvider::OpenAi && cfg.AiApiKey.empty()) {
            v.Errors.push_back("OpenAI: API key required");
        }
    } else if (provider == AiProvider::Anthropic) {
        if (cfg.AiAnthropicApiKey.empty()) {
            v.Errors.push_back("Anthropic: API key required");
        }
    }
    // OllamaNative needs no API key.

    // --- Model presence (per active provider) ---
    std::string activeModelId;
    const char* activeProviderLabel = "";
    switch (provider) {
    case AiProvider::OpenAi:
        activeModelId = cfg.AiModelOpenAi;
        activeProviderLabel = "OpenAI";
        break;
    case AiProvider::Anthropic:
        activeModelId = cfg.AiModelAnthropic;
        activeProviderLabel = "Anthropic";
        break;
    case AiProvider::OllamaNative:
        activeModelId = cfg.AiModelOllama;
        activeProviderLabel = "Ollama (native)";
        break;
    case AiProvider::OllamaOpenAiCompat:
        activeModelId = cfg.AiModelOpenAi;
        activeProviderLabel = "Ollama (OpenAI-compat)";
        break;
    }
    if (activeModelId.empty()) {
        v.Errors.push_back(std::string(activeProviderLabel) + ": model ID required");
    }

    // --- Base URL well-formedness ---
    if (!cfg.AiBaseUrl.empty() && !LooksLikeHttpUrl(cfg.AiBaseUrl)) {
        v.Errors.push_back("Base URL must start with http:// or https://");
    }
    if (provider == AiProvider::OllamaNative) {
        if (!cfg.AiOllamaBaseUrl.empty() && !LooksLikeHttpUrl(cfg.AiOllamaBaseUrl)) {
            v.Errors.push_back("Ollama base URL must start with http:// or https://");
        }
    }

    // --- Key format sniff (warnings) ---
    if (provider == AiProvider::OpenAi && !cfg.AiApiKey.empty() && !StartsWith(cfg.AiApiKey, "sk-")) {
        v.Warnings.push_back("OpenAI: API key doesn't start with 'sk-' - likely malformed");
    }
    if (provider == AiProvider::Anthropic && !cfg.AiAnthropicApiKey.empty() &&
        !StartsWith(cfg.AiAnthropicApiKey, "sk-ant-")) {
        v.Warnings.push_back("Anthropic: API key doesn't start with 'sk-ant-' - likely malformed");
    }

    // --- Unknown model warning (only when catalog non-empty) ---
    if (!activeModelId.empty()) {
        const std::vector<ModelOption> catalog = KnownModels(provider);
        if (!catalog.empty() && !IsKnownModel(provider, activeModelId)) {
            v.Warnings.push_back(std::string(activeProviderLabel) + ": model '" + activeModelId +
                                 "' not in known catalog - may not exist");
        }
    }

    // --- Ollama default-URL hint ---
    if (provider == AiProvider::OllamaNative && cfg.AiOllamaBaseUrl.empty()) {
        v.Warnings.push_back("Ollama: base URL empty; will default to http://localhost:11434");
    }

    return v;
}

} // namespace ai
} // namespace smatchet
