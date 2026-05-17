#include "AiClientFactory.h"

#include "AnthropicClient.h"
#include "Logger.h"
#include "OllamaClient.h"
#include "OpenAiClient.h"

#include <memory>
#include <string>
#include <vector>

namespace AiClientFactory {

std::unique_ptr<IAiClient> MakeAiClient(AiProvider provider) {
    switch (provider) {
    case AiProvider::OpenAi:
    case AiProvider::OllamaOpenAiCompat:
        return std::unique_ptr<IAiClient>(new OpenAiClient());
    case AiProvider::Anthropic:
        return std::unique_ptr<IAiClient>(new AnthropicClient());
    case AiProvider::OllamaNative:
        return std::unique_ptr<IAiClient>(new OllamaClient());
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
    return false;
}

std::vector<ProviderEntry> EnumeratedProviders() {
    std::vector<ProviderEntry> v;
    v.push_back({AiProvider::OpenAi, "openai", "OpenAI (or OpenAI-compatible)"});
    v.push_back({AiProvider::Anthropic, "anthropic", "Anthropic"});
    v.push_back({AiProvider::OllamaOpenAiCompat, "ollama-openai", "Ollama (OpenAI-compat /v1)"});
    v.push_back({AiProvider::OllamaNative, "ollama-native", "Ollama (native /api/chat)"});
    return v;
}

} // namespace AiClientFactory
