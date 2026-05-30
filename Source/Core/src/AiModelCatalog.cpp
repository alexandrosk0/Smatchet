#include "AiModelCatalog.h"

#include <algorithm>

namespace smatchet {
namespace ai {

std::vector<ModelOption> KnownModels(AiProvider provider) {
    std::vector<ModelOption> out;
    switch (provider) {
    case AiProvider::OpenAi:
    case AiProvider::OllamaOpenAiCompat:
        // OllamaOpenAiCompat shares the OpenAI catalog only for the in-app
        // dropdown convenience — actual Ollama-served models are local and
        // free-form, so the Preferences UI exposes a "custom" escape hatch
        // for that path.
        out.push_back({"gpt-4o", "GPT-4o"});
        out.push_back({"gpt-4o-mini", "GPT-4o mini"});
        out.push_back({"gpt-4-turbo", "GPT-4 Turbo"});
        out.push_back({"gpt-4", "GPT-4"});
        out.push_back({"gpt-3.5-turbo", "GPT-3.5 Turbo"});
        out.push_back({"o1-mini", "o1-mini"});
        out.push_back({"o1-preview", "o1-preview"});
        break;
    case AiProvider::Anthropic:
        // Sourced from https://docs.anthropic.com/en/docs/about-claude/models
        // (latest models comparison table). Pinned snapshot IDs — every entry
        // here is a stable identifier that resolves to a specific release.
        out.push_back({"claude-opus-4-7", "Claude Opus 4.7"});
        out.push_back({"claude-sonnet-4-6", "Claude Sonnet 4.6"});
        out.push_back({"claude-haiku-4-5", "Claude Haiku 4.5"});
        out.push_back({"claude-opus-4-6", "Claude Opus 4.6 (legacy)"});
        out.push_back({"claude-sonnet-4-5", "Claude Sonnet 4.5 (legacy)"});
        out.push_back({"claude-opus-4-5", "Claude Opus 4.5 (legacy)"});
        out.push_back({"claude-opus-4-1", "Claude Opus 4.1 (legacy)"});
        break;
    case AiProvider::OllamaNative:
        // Free-form — `ollama list` enumerates locally-installed models.
        // The Preferences UI falls back to an InputText when KnownModels()
        // returns empty.
        break;
    case AiProvider::DeepSeek:
        // DeepSeek's published model catalog. The wire is OpenAI-compatible
        // (served at https://api.deepseek.com/v1/chat/completions) but the
        // model IDs are DeepSeek-specific — `deepseek-chat` is the general
        // chat model, `deepseek-reasoner` is the reasoning-tuned variant
        // which accepts the OpenAI-style `reasoning_effort` parameter.
        out.push_back({"deepseek-chat", "DeepSeek Chat"});
        out.push_back({"deepseek-reasoner", "DeepSeek Reasoner"});
        break;
    }
    return out;
}

bool IsKnownModel(AiProvider provider, const std::string& modelId) {
    if (modelId.empty()) {
        return false;
    }
    const std::vector<ModelOption> catalog = KnownModels(provider);
    return std::any_of(catalog.begin(), catalog.end(), [&modelId](const ModelOption& m) { return m.Id == modelId; });
}

} // namespace ai
} // namespace smatchet
