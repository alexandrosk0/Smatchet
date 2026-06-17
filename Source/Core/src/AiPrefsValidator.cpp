#include "AiPrefsValidator.h"

#include "AiClientFactory.h"
#include "AiEndpointPolicy.h"
#include "AiEndpointSanitize.h"
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
    case 4:
        return AiProvider::DeepSeek;
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

// Resolved per-provider view of the model field — the model ID plus the human
// label and the structured field key used for both the missing-model error and
// the unknown-catalog warning.
struct ActiveModelView {
    std::string Id;
    const char* Label;
    const char* FieldKey;
};

ActiveModelView ResolveActiveModel(const TrackerConfig& cfg, AiProvider provider) {
    ActiveModelView m;
    m.Label = "";
    m.FieldKey = nullptr;
    switch (provider) {
    case AiProvider::OpenAi:
        m.Id = cfg.AiModelOpenAi;
        m.Label = "OpenAI";
        m.FieldKey = PrefsFieldKey::kAiModelOpenAi;
        break;
    case AiProvider::Anthropic:
        m.Id = cfg.AiModelAnthropic;
        m.Label = "Anthropic";
        m.FieldKey = PrefsFieldKey::kAiModelAnthropic;
        break;
    case AiProvider::OllamaNative:
        m.Id = cfg.AiModelOllama;
        m.Label = "Ollama (native)";
        m.FieldKey = PrefsFieldKey::kAiModelOllama;
        break;
    case AiProvider::OllamaOpenAiCompat:
        m.Id = cfg.AiModelOpenAi;
        m.Label = "OpenAI-compatible local";
        m.FieldKey = PrefsFieldKey::kAiModelOpenAi;
        break;
    case AiProvider::DeepSeek:
        m.Id = cfg.AiModelDeepSeek;
        m.Label = "DeepSeek";
        m.FieldKey = PrefsFieldKey::kAiModelDeepSeek;
        break;
    }
    return m;
}

// --- Key presence (per active provider) ---
// Only the hosted providers (OpenAi proper, Anthropic, DeepSeek) require a key.
// Local OpenAI-compatible servers (LM Studio / Ollama / LocalAI / vLLM) and Ollama
// native ship without auth by default; even when the server is configured for token
// gating, users frequently leave the slot blank and provide auth out-of-band (header
// injection via reverse proxy, OS keychain helper, etc).
void CheckKeyPresence(PrefsValidation& v, const TrackerConfig& cfg, AiProvider provider) {
    if (provider == AiProvider::OpenAi) {
        if (cfg.AiApiKey.empty()) {
            EmitError(v, PrefsFieldKey::kAiApiKey, "OpenAI: API key required");
        }
    } else if (provider == AiProvider::Anthropic) {
        if (cfg.AiAnthropicApiKey.empty()) {
            EmitError(v, PrefsFieldKey::kAiAnthropicApiKey, "Anthropic: API key required");
        }
    } else if (provider == AiProvider::DeepSeek) {
        if (cfg.AiDeepSeekApiKey.empty()) {
            EmitError(v, PrefsFieldKey::kAiDeepSeekApiKey, "DeepSeek: API key required");
        }
    }
    // OllamaOpenAiCompat + OllamaNative: API key optional (no error when empty).
}

// --- Base URL safety (scheme + SSRF denylist + per-provider host pin) ---
// Route each configured base URL through the SAME pure validator the live
// request (AiAssistantController) and the Test-connection probe use — see
// AiEndpointPolicy.h, the single source of truth — so Preferences rejects
// exactly what a real request would and no looser. The previous scheme-only
// check let a cloud-metadata / link-local / private-network / alternate-encoding
// host (e.g. http://2852039166 == 169.254.169.254) save clean here only for the
// request path to refuse it. The SSRF denylist always fires; loopback (the local
// http://localhost:11434 Ollama case) stays allowed via the unpinned-provider
// policy, so the common local setups are unaffected.
void CheckOneBaseUrl(PrefsValidation& v, const std::string& url, const char* fieldKey, const char* label,
                     const pure::EndpointPolicy& policy) {
    if (url.empty()) {
        return;
    }
    std::string normalized;
    const pure::EndpointVerdict verdict = pure::SanitizeAiEndpointUrl(url, policy, normalized);
    if (verdict != pure::EndpointVerdict::Allowed) {
        EmitError(v, fieldKey, std::string(label) + ": " + pure::EndpointVerdictDescription(verdict));
    }
}

void CheckBaseUrls(PrefsValidation& v, const TrackerConfig& cfg, AiProvider provider) {
    const pure::EndpointPolicy policy = EndpointPolicyForProvider(cfg, provider);
    CheckOneBaseUrl(v, cfg.AiBaseUrl, PrefsFieldKey::kAiBaseUrl, "Base URL", policy);
    if (provider == AiProvider::OllamaNative) {
        CheckOneBaseUrl(v, cfg.AiOllamaBaseUrl, PrefsFieldKey::kAiOllamaBaseUrl, "Ollama base URL", policy);
    }
}

// --- Key format sniff (warnings) ---
// Only nag for the hosted-provider key formats. Local-server keys (rare; sometimes used
// as a literal "sk-no-key-required" placeholder) don't follow a stable prefix.
// Cross-provider paste detection: `sk-ant-` is Anthropic-exclusive. If it shows up in
// the OpenAI or DeepSeek key field the user almost certainly pasted the wrong key into
// the wrong slot — that path used to produce a confusing HTTP 401 from the provider
// ("Authentication Fails, Your api key: ****GwAA is invalid") with no Smatchet-side
// signal that the key shape was wrong. Catch it at validate-time so the user gets a
// clear warning before any network round-trip.
void CheckKeyFormat(PrefsValidation& v, const TrackerConfig& cfg, AiProvider provider) {
    if (provider == AiProvider::OpenAi && !cfg.AiApiKey.empty()) {
        if (StartsWith(cfg.AiApiKey, "sk-ant-")) {
            EmitWarning(v, PrefsFieldKey::kAiApiKey,
                        "OpenAI: API key starts with 'sk-ant-' (Anthropic format) - did you paste the wrong key?");
        } else if (!StartsWith(cfg.AiApiKey, "sk-")) {
            EmitWarning(v, PrefsFieldKey::kAiApiKey, "OpenAI: API key doesn't start with 'sk-' - likely malformed");
        }
    }
    if (provider == AiProvider::Anthropic && !cfg.AiAnthropicApiKey.empty() &&
        !StartsWith(cfg.AiAnthropicApiKey, "sk-ant-")) {
        EmitWarning(v, PrefsFieldKey::kAiAnthropicApiKey,
                    "Anthropic: API key doesn't start with 'sk-ant-' - likely malformed");
    }
    if (provider == AiProvider::DeepSeek && !cfg.AiDeepSeekApiKey.empty()) {
        if (StartsWith(cfg.AiDeepSeekApiKey, "sk-ant-")) {
            EmitWarning(v, PrefsFieldKey::kAiDeepSeekApiKey,
                        "DeepSeek: API key starts with 'sk-ant-' (Anthropic format) - did you paste the wrong key?");
        } else if (!StartsWith(cfg.AiDeepSeekApiKey, "sk-")) {
            EmitWarning(v, PrefsFieldKey::kAiDeepSeekApiKey,
                        "DeepSeek: API key doesn't start with 'sk-' - likely malformed");
        }
    }
}

// --- Unknown model warning (only when catalog non-empty) ---
void CheckUnknownModel(PrefsValidation& v, AiProvider provider, const ActiveModelView& model) {
    if (model.Id.empty()) {
        return;
    }
    const std::vector<ModelOption> catalog = KnownModels(provider);
    if (!catalog.empty() && !IsKnownModel(provider, model.Id)) {
        EmitWarning(v, model.FieldKey,
                    std::string(model.Label) + ": model '" + model.Id + "' not in known catalog - may not exist");
    }
}

} // namespace

PrefsValidation ValidateAiPrefs(const TrackerConfig& cfg) {
    PrefsValidation v;
    const AiProvider provider = ClampProvider(cfg.AiProviderKind);

    CheckKeyPresence(v, cfg, provider);

    const ActiveModelView activeModel = ResolveActiveModel(cfg, provider);
    if (activeModel.Id.empty()) {
        EmitError(v, activeModel.FieldKey, std::string(activeModel.Label) + ": model ID required");
    }

    CheckBaseUrls(v, cfg, provider);
    CheckKeyFormat(v, cfg, provider);
    CheckUnknownModel(v, provider, activeModel);

    // --- Ollama default-URL hint ---
    if (provider == AiProvider::OllamaNative && cfg.AiOllamaBaseUrl.empty()) {
        EmitWarning(v, PrefsFieldKey::kAiOllamaBaseUrl,
                    "Ollama: base URL empty; will default to http://localhost:11434");
    }

    return v;
}

} // namespace ai
} // namespace smatchet
