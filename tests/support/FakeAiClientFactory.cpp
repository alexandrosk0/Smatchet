// FakeAiClientFactory — test-only implementation of the AiClientFactory free functions that OMITS
// the real HTTP clients (AnthropicClient / OpenAiClient / OllamaClient → cpr). Linking the real
// Source/Core/src/AiClientFactory.cpp would drag cpr into the cpr-free SmatchetTsanTests target;
// this stub provides the same symbols so AiAssistantController links headless. `MakeAiClient`
// returns whatever the test installed via SetTestOverride (a StubAiClient); the provider
// string/enumeration helpers are copied verbatim from the production impl (they carry no client
// dependency). Production links the real factory; only this TSan target links the stub.

#include "AiClientFactory.h"

namespace AiClientFactory {

namespace {
TestOverrideFn s_testOverride = nullptr;
}

void SetTestOverride(TestOverrideFn fn) { s_testOverride = fn; }

std::unique_ptr<IAiClient> MakeAiClient(AiProvider provider) {
    // No real-client switch here (that is the cpr edge) — the test override is the only path.
    if (s_testOverride) {
        return s_testOverride(provider);
    }
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
    return {
        {AiProvider::OpenAi, "openai", "OpenAI"},
        {AiProvider::Anthropic, "anthropic", "Anthropic"},
        {AiProvider::OllamaOpenAiCompat, "ollama-openai", "Ollama (OpenAI-compatible)"},
        {AiProvider::OllamaNative, "ollama-native", "Ollama (native)"},
        {AiProvider::DeepSeek, "deepseek", "DeepSeek"},
    };
}

} // namespace AiClientFactory
