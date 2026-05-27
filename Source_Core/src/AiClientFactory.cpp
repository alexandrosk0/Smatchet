#include "AiClientFactory.h"

#include "AnthropicClient.h"
#include "Logger.h"
#include "OllamaClient.h"
#include "OpenAiClient.h"

#include <memory>
#include <string>
#include <vector>

namespace AiClientFactory {

namespace {
TestOverrideFn s_testOverride = nullptr;
}

void SetTestOverride(TestOverrideFn fn) { s_testOverride = fn; }

std::unique_ptr<IAiClient> MakeAiClient(AiProvider provider) {
    if (s_testOverride) {
        return s_testOverride(provider);
    }
    switch (provider) {
    case AiProvider::OpenAi:
    case AiProvider::OllamaOpenAiCompat:
    case AiProvider::DeepSeek:
        // DeepSeek's wire is OpenAI-protocol-compatible (`/v1/chat/completions`
        // with the same streaming SSE shape + body fields). Reuse OpenAiClient;
        // the base URL + model ID select between OpenAI / DeepSeek at request
        // time.
        return std::unique_ptr<IAiClient>(new OpenAiClient()); // custom-deleter — make_unique inapplicable (base-type unique_ptr wrapping derived)
    case AiProvider::Anthropic:
        return std::unique_ptr<IAiClient>(new AnthropicClient()); // custom-deleter — make_unique inapplicable (base-type unique_ptr wrapping derived)
    case AiProvider::OllamaNative:
        return std::unique_ptr<IAiClient>(new OllamaClient()); // custom-deleter — make_unique inapplicable (base-type unique_ptr wrapping derived)
    }
    LOG_ERROR("AiClientFactory: unknown AiProvider enum value %d", static_cast<int>(provider));
    return nullptr;
}

std::string ProviderToString(AiProvider provider) {
    switch (provider) {
    case AiProvider::OpenAi:
        return "openai";
    case AiProvider::Anthropic:
        return "anthropic";
    case AiProvider::OllamaOpenAiCompat:
        return "ollama-openai";
    case AiProvider::OllamaNative:
        return "ollama-native";
    case AiProvider::DeepSeek:
        return "deepseek";
    }
    return "openai";
}

bool ProviderFromString(const std::string& s, AiProvider& out) {
    if (s == "openai") {
        out = AiProvider::OpenAi;
        return true;
    }
    if (s == "anthropic") {
        out = AiProvider::Anthropic;
        return true;
    }
    if (s == "ollama-openai") {
        out = AiProvider::OllamaOpenAiCompat;
        return true;
    }
    if (s == "ollama-native") {
        out = AiProvider::OllamaNative;
        return true;
    }
    if (s == "deepseek") {
        out = AiProvider::DeepSeek;
        return true;
    }
    return false;
}

std::vector<ProviderEntry> EnumeratedProviders() {
    std::vector<ProviderEntry> v;
    v.push_back({AiProvider::OpenAi, "openai", "OpenAI (or OpenAI-compatible)"});
    v.push_back({AiProvider::Anthropic, "anthropic", "Anthropic"});
    // Display copy expanded so the LM Studio / LocalAI / vLLM cases the slot covers
    // are discoverable in the Combo. The CLI string ("ollama-openai") is unchanged so
    // round-tripping persisted AiProviderKind / CLI flags stays stable.
    v.push_back({AiProvider::OllamaOpenAiCompat, "ollama-openai",
                 "OpenAI-compatible local (LM Studio / Ollama / LocalAI / vLLM)"});
    v.push_back({AiProvider::OllamaNative, "ollama-native", "Ollama (native /api/chat)"});
    v.push_back({AiProvider::DeepSeek, "deepseek", "DeepSeek"});
    return v;
}

} // namespace AiClientFactory
