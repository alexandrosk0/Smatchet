// AiModelCatalog pure-logic tests - asserts the hardcoded model catalog for
// each provider is what we expect at link time. Sourced from the providers'
// public model docs; updates here track the providers' model lifecycle.

#include "AiModelCatalog.h"

#include "AiTypes.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

bool ContainsId(const std::vector<smatchet::ai::ModelOption>& models, const std::string& id) {
    for (std::size_t i = 0; i < models.size(); ++i) {
        if (models[i].Id == id) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("AiModelCatalog: OpenAi seed contains expected public IDs") {
    const auto v = smatchet::ai::KnownModels(AiProvider::OpenAi);
    REQUIRE(v.size() >= 3);
    CHECK(ContainsId(v, "gpt-4o"));
    CHECK(ContainsId(v, "gpt-4o-mini"));
    CHECK(ContainsId(v, "gpt-4-turbo"));
    CHECK(ContainsId(v, "gpt-3.5-turbo"));
    for (std::size_t i = 0; i < v.size(); ++i) {
        CHECK_FALSE(v[i].Id.empty());
        CHECK_FALSE(v[i].DisplayName.empty());
    }
}

TEST_CASE("AiModelCatalog: Anthropic seed contains current public IDs") {
    const auto v = smatchet::ai::KnownModels(AiProvider::Anthropic);
    REQUIRE(v.size() >= 3);
    CHECK(ContainsId(v, "claude-opus-4-7"));
    CHECK(ContainsId(v, "claude-sonnet-4-6"));
    CHECK(ContainsId(v, "claude-haiku-4-5"));
    for (std::size_t i = 0; i < v.size(); ++i) {
        CHECK_FALSE(v[i].Id.empty());
        CHECK_FALSE(v[i].DisplayName.empty());
    }
}

TEST_CASE("AiModelCatalog: OllamaNative returns empty (free-form)") {
    const auto v = smatchet::ai::KnownModels(AiProvider::OllamaNative);
    CHECK(v.empty());
}

TEST_CASE("AiModelCatalog: OllamaOpenAiCompat shares OpenAi convenience catalog") {
    const auto v = smatchet::ai::KnownModels(AiProvider::OllamaOpenAiCompat);
    // Convenience: OpenAI-shaped seed exposed for the user; actual served
    // models are local but the dropdown is friendlier than free-form here.
    CHECK(v.size() >= 1);
}

TEST_CASE("AiModelCatalog: IsKnownModel exact match true / non-match false") {
    CHECK(smatchet::ai::IsKnownModel(AiProvider::OpenAi, "gpt-4o-mini"));
    CHECK(smatchet::ai::IsKnownModel(AiProvider::Anthropic, "claude-sonnet-4-6"));
    CHECK(smatchet::ai::IsKnownModel(AiProvider::Anthropic, "claude-opus-4-7"));
    CHECK_FALSE(smatchet::ai::IsKnownModel(AiProvider::OpenAi, ""));
    CHECK_FALSE(smatchet::ai::IsKnownModel(AiProvider::OpenAi, "gpt-99-invented"));
    CHECK_FALSE(smatchet::ai::IsKnownModel(AiProvider::Anthropic, "claude-fictional-model"));
    // Cross-provider miss: OpenAI ID never matches Anthropic catalog.
    CHECK_FALSE(smatchet::ai::IsKnownModel(AiProvider::Anthropic, "gpt-4o-mini"));
}

TEST_CASE("AiModelCatalog: IsKnownModel handles empty-catalog providers") {
    // OllamaNative has no fixed catalog - every ID is "unknown" by this API,
    // but the validator carefully skips the unknown-model warning for
    // empty-catalog providers so users with local-installed models don't get
    // nagged.
    CHECK_FALSE(smatchet::ai::IsKnownModel(AiProvider::OllamaNative, "llama3"));
    CHECK_FALSE(smatchet::ai::IsKnownModel(AiProvider::OllamaNative, "anything"));
    CHECK_FALSE(smatchet::ai::IsKnownModel(AiProvider::OllamaNative, ""));
}
