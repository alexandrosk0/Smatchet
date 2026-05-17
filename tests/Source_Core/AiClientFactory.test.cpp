// AiClientFactory pure-logic tests — exercises Source_Core/include/AiClientFactory.h.
// Phase A' coverage: enum round-trip stability + factory branch-by-provider behaviour.
//
// MakeAiClient(OpenAi) returns a non-null OpenAiClient. Phase D flipped the Anthropic
// and OllamaNative branches to return non-null AnthropicClient + OllamaClient instances.
// EnumeratedProviders() must list all four kinds in stable insertion order so the
// Preferences Combo (Phase B / Phase D) renders deterministically.

#include "AiClientFactory.h"
#include "AiTypes.h"
#include "IAiClient.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

TEST_CASE("AiClientFactory: ProviderToString covers every enum value") {
    CHECK(AiClientFactory::ProviderToString(AiProvider::OpenAi) == "openai");
    CHECK(AiClientFactory::ProviderToString(AiProvider::Anthropic) == "anthropic");
    CHECK(AiClientFactory::ProviderToString(AiProvider::OllamaOpenAiCompat) == "ollama-openai");
    CHECK(AiClientFactory::ProviderToString(AiProvider::OllamaNative) == "ollama-native");
}

TEST_CASE("AiClientFactory: ProviderFromString round-trips every known key") {
    const AiProvider kinds[] = {AiProvider::OpenAi, AiProvider::Anthropic, AiProvider::OllamaOpenAiCompat,
                                AiProvider::OllamaNative};
    for (AiProvider kind : kinds) {
        const std::string key = AiClientFactory::ProviderToString(kind);
        AiProvider out = AiProvider::OpenAi;
        const bool ok = AiClientFactory::ProviderFromString(key, out);
        CHECK(ok);
        CHECK(static_cast<int>(out) == static_cast<int>(kind));
    }
}

TEST_CASE("AiClientFactory: ProviderFromString rejects unknown / empty input") {
    AiProvider out = AiProvider::Anthropic; // poison value to confirm no overwrite
    CHECK_FALSE(AiClientFactory::ProviderFromString("", out));
    CHECK(out == AiProvider::Anthropic); // out untouched on failure

    CHECK_FALSE(AiClientFactory::ProviderFromString("unknown_provider_xyz", out));
    CHECK(out == AiProvider::Anthropic);

    // Case sensitivity: factory uses exact-match lowercase keys (verified against impl).
    CHECK_FALSE(AiClientFactory::ProviderFromString("OPENAI", out));
    CHECK_FALSE(AiClientFactory::ProviderFromString("OpenAI", out));
}

TEST_CASE("AiClientFactory: EnumeratedProviders returns 4 entries in stable order") {
    const std::vector<AiClientFactory::ProviderEntry> entries = AiClientFactory::EnumeratedProviders();
    REQUIRE(entries.size() == 4);

    // Stable insertion order matches enum declaration order — load-bearing for the
    // Preferences Combo: a re-ordered list would silently corrupt persisted indices
    // on installs that round-tripped before the re-order.
    CHECK(entries[0].Kind == AiProvider::OpenAi);
    CHECK(entries[1].Kind == AiProvider::Anthropic);
    CHECK(entries[2].Kind == AiProvider::OllamaOpenAiCompat);
    CHECK(entries[3].Kind == AiProvider::OllamaNative);

    // Each entry's Key must round-trip through ProviderFromString.
    for (const AiClientFactory::ProviderEntry& e : entries) {
        AiProvider parsed = AiProvider::OpenAi;
        const bool ok = AiClientFactory::ProviderFromString(e.Key, parsed);
        CHECK(ok);
        CHECK(static_cast<int>(parsed) == static_cast<int>(e.Kind));
        CHECK_FALSE(e.Display.empty()); // user-facing label populated
    }
}

TEST_CASE("AiClientFactory: MakeAiClient(OpenAi) returns a non-null IAiClient") {
    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(AiProvider::OpenAi);
    REQUIRE(client != nullptr);
    CHECK(client->GetProviderName() == "openai");
}

TEST_CASE("AiClientFactory: MakeAiClient(OllamaOpenAiCompat) also returns a non-null OpenAI-shaped client") {
    // OllamaOpenAiCompat re-uses the OpenAiClient (same /v1/chat/completions wire format).
    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(AiProvider::OllamaOpenAiCompat);
    REQUIRE(client != nullptr);
    CHECK(client->GetProviderName() == "openai");
}

TEST_CASE("AiClientFactory: MakeAiClient(Anthropic) returns a non-null AnthropicClient (Phase D)") {
    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(AiProvider::Anthropic);
    REQUIRE(client != nullptr);
    CHECK(client->GetProviderName() == "anthropic");
}

TEST_CASE("AiClientFactory: MakeAiClient(OllamaNative) returns a non-null OllamaClient (Phase D)") {
    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(AiProvider::OllamaNative);
    REQUIRE(client != nullptr);
    CHECK(client->GetProviderName() == "ollama");
}
