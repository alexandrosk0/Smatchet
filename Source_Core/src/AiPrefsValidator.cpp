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

void EmitError(PrefsValidation& v, const char* fieldKey, std::string message) {
    PrefsValidationIssue issue;
    issue.FieldKey = (fieldKey != nullptr) ? std::string(fieldKey) : std::string();
    issue.Severity = PrefsSeverity::Error;
    issue.Message = message;
    v.Errors.push_back(message);
    v.Issues.push_back(std::move(issue));
}

void EmitWarning(PrefsValidation& v, const char* fieldKey, std::string message) {
    PrefsValidationIssue issue;
    issue.FieldKey = (fieldKey != nullptr) ? std::string(fieldKey) : std::string();
    issue.Severity = PrefsSeverity::Warning;
    issue.Message = message;
    v.Warnings.push_back(message);
    v.Issues.push_back(std::move(issue));
}

} // namespace

PrefsValidation ValidateAiPrefs(const TrackerConfig& cfg) {
    PrefsValidation v;
    const AiProvider provider = ClampProvider(cfg.AiProviderKind);

    // --- Key presence (per active provider) ---
    // Only the hosted providers (OpenAi proper, Anthropic) require a key. Local
    // OpenAI-compatible servers (LM Studio / Ollama / LocalAI / vLLM) and Ollama
    // native ship without auth by default; even when the server is configured
    // for token gating, users frequently leave the slot blank and provide auth
    // out-of-band (header injection via reverse proxy, OS keychain helper, etc).
    if (provider == AiProvider::OpenAi) {
        if (cfg.AiApiKey.empty()) {
            EmitError(v, PrefsFieldKey::kAiApiKey, "OpenAI: API key required");
        }
    } else if (provider == AiProvider::Anthropic) {
        if (cfg.AiAnthropicApiKey.empty()) {
            EmitError(v, PrefsFieldKey::kAiAnthropicApiKey, "Anthropic: API key required");
        }
    }
    // OllamaOpenAiCompat + OllamaNative: API key optional (no error when empty).

    // --- Model presence (per active provider) ---
    std::string activeModelId;
    const char* activeProviderLabel = "";
    const char* activeModelFieldKey = nullptr;
    switch (provider) {
    case AiProvider::OpenAi:
        activeModelId = cfg.AiModelOpenAi;
        activeProviderLabel = "OpenAI";
        activeModelFieldKey = PrefsFieldKey::kAiModelOpenAi;
        break;
    case AiProvider::Anthropic:
        activeModelId = cfg.AiModelAnthropic;
        activeProviderLabel = "Anthropic";
        activeModelFieldKey = PrefsFieldKey::kAiModelAnthropic;
        break;
    case AiProvider::OllamaNative:
        activeModelId = cfg.AiModelOllama;
        activeProviderLabel = "Ollama (native)";
        activeModelFieldKey = PrefsFieldKey::kAiModelOllama;
        break;
    case AiProvider::OllamaOpenAiCompat:
        activeModelId = cfg.AiModelOpenAi;
        activeProviderLabel = "OpenAI-compatible local";
        activeModelFieldKey = PrefsFieldKey::kAiModelOpenAi;
        break;
    }
    if (activeModelId.empty()) {
        EmitError(v, activeModelFieldKey, std::string(activeProviderLabel) + ": model ID required");
    }

    // --- Base URL well-formedness ---
    if (!cfg.AiBaseUrl.empty() && !LooksLikeHttpUrl(cfg.AiBaseUrl)) {
        EmitError(v, PrefsFieldKey::kAiBaseUrl, "Base URL must start with http:// or https://");
    }
    if (provider == AiProvider::OllamaNative) {
        if (!cfg.AiOllamaBaseUrl.empty() && !LooksLikeHttpUrl(cfg.AiOllamaBaseUrl)) {
            EmitError(v, PrefsFieldKey::kAiOllamaBaseUrl,
                      "Ollama base URL must start with http:// or https://");
        }
    }

    // --- Key format sniff (warnings) ---
    // Only nag for the hosted-provider key formats. Local-server keys (rare; sometimes used
    // as a literal "sk-no-key-required" placeholder) don't follow a stable prefix.
    if (provider == AiProvider::OpenAi && !cfg.AiApiKey.empty() && !StartsWith(cfg.AiApiKey, "sk-")) {
        EmitWarning(v, PrefsFieldKey::kAiApiKey,
                    "OpenAI: API key doesn't start with 'sk-' - likely malformed");
    }
    if (provider == AiProvider::Anthropic && !cfg.AiAnthropicApiKey.empty() &&
        !StartsWith(cfg.AiAnthropicApiKey, "sk-ant-")) {
        EmitWarning(v, PrefsFieldKey::kAiAnthropicApiKey,
                    "Anthropic: API key doesn't start with 'sk-ant-' - likely malformed");
    }

    // --- Unknown model warning (only when catalog non-empty) ---
    if (!activeModelId.empty()) {
        const std::vector<ModelOption> catalog = KnownModels(provider);
        if (!catalog.empty() && !IsKnownModel(provider, activeModelId)) {
            EmitWarning(v, activeModelFieldKey,
                        std::string(activeProviderLabel) + ": model '" + activeModelId +
                            "' not in known catalog - may not exist");
        }
    }

    // --- Ollama default-URL hint ---
    if (provider == AiProvider::OllamaNative && cfg.AiOllamaBaseUrl.empty()) {
        EmitWarning(v, PrefsFieldKey::kAiOllamaBaseUrl,
                    "Ollama: base URL empty; will default to http://localhost:11434");
    }

    return v;
}

} // namespace ai
} // namespace smatchet
