// FakeAiClientFactory — test-only implementation of the AiClientFactory free functions that OMITS
// the real HTTP clients (AnthropicClient / OpenAiClient / OllamaClient → cpr). Linking the real
// Source/Core/src/AiClientFactory.cpp would drag cpr into the cpr-free SmatchetTsanTests target;
// this stub provides only the symbols AiAssistantController actually references so it links
// headless: SetTestOverride + MakeAiClient (the test installs a StubAiClient via the override) and
// ProviderToString (the controller's one provider-string call). Production links the real factory;
// only this TSan target links the stub.

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

} // namespace AiClientFactory
