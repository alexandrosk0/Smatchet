// AiClientFactory pure-logic tests — exercises Source/Core/include/AiClientFactory.h.
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
    CHECK(AiClientFactory::ProviderToString(AiProvider::DeepSeek) == "deepseek");
}

TEST_CASE("AiClientFactory: ProviderFromString round-trips every known key") {
    const AiProvider kinds[] = {AiProvider::OpenAi, AiProvider::Anthropic, AiProvider::OllamaOpenAiCompat,
                                AiProvider::OllamaNative, AiProvider::DeepSeek};
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

TEST_CASE("AiClientFactory: EnumeratedProviders returns 5 entries in stable order") {
    const std::vector<AiClientFactory::ProviderEntry> entries = AiClientFactory::EnumeratedProviders();
    REQUIRE(entries.size() == 5);

    // Stable insertion order matches enum declaration order — load-bearing for the
    // Preferences Combo: a re-ordered list would silently corrupt persisted indices
    // on installs that round-tripped before the re-order.
    CHECK(entries[0].Kind == AiProvider::OpenAi);
    CHECK(entries[1].Kind == AiProvider::Anthropic);
    CHECK(entries[2].Kind == AiProvider::OllamaOpenAiCompat);
    CHECK(entries[3].Kind == AiProvider::OllamaNative);
    CHECK(entries[4].Kind == AiProvider::DeepSeek);

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

TEST_CASE("AiClientFactory: MakeAiClient(DeepSeek) returns a non-null OpenAI-shaped client") {
    // DeepSeek's wire is OpenAI-protocol-compatible; the factory reuses
    // OpenAiClient. GetProviderName therefore reports "openai" — DeepSeek-vs-OpenAI
    // is distinguished by the configured base URL + model, not by the client class.
    std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(AiProvider::DeepSeek);
    REQUIRE(client != nullptr);
    CHECK(client->GetProviderName() == "openai");
}

// --- SetTestOverride seam (bucket-E support; see docs/plans/shipped/ai-client-test-override.md) ----

namespace {

class TestStubClient : public IAiClient {
  public:
    std::string GetProviderName() const override { return "stub-test-override"; }
    std::string ProbeReachability(const AiClientConfig& /*cfg*/) override { return std::string(); }
    void SendStreaming(const AiClientConfig& /*cfg*/, const AiChatRequest& /*req*/, const DeltaCallback& /*onDelta*/,
                       const ErrorCallback& /*onError*/, const CancelToken& /*cancel*/) override {}
};

std::unique_ptr<IAiClient> MakeTestStub(AiProvider /*provider*/) {
    return std::unique_ptr<IAiClient>(new TestStubClient());
}

} // namespace

TEST_CASE("AiClientFactory: SetTestOverride routes MakeAiClient through injector") {
    AiClientFactory::SetTestOverride(&MakeTestStub);

    // Override fires regardless of the requested provider.
    {
        std::unique_ptr<IAiClient> c = AiClientFactory::MakeAiClient(AiProvider::OpenAi);
        REQUIRE(c != nullptr);
        CHECK(c->GetProviderName() == "stub-test-override");
    }
    {
        std::unique_ptr<IAiClient> c = AiClientFactory::MakeAiClient(AiProvider::Anthropic);
        REQUIRE(c != nullptr);
        CHECK(c->GetProviderName() == "stub-test-override");
    }

    // Clear → default provider-kind switch resumes.
    AiClientFactory::SetTestOverride(nullptr);
    {
        std::unique_ptr<IAiClient> c = AiClientFactory::MakeAiClient(AiProvider::OpenAi);
        REQUIRE(c != nullptr);
        CHECK(c->GetProviderName() == "openai");
    }
}

TEST_CASE("AiClientFactory: SetTestOverride hands out non-null on repeated calls") {
    AiClientFactory::SetTestOverride(&MakeTestStub);
    for (int i = 0; i < 3; ++i) {
        std::unique_ptr<IAiClient> c = AiClientFactory::MakeAiClient(AiProvider::OpenAi);
        REQUIRE(c != nullptr);
        CHECK(c->GetProviderName() == "stub-test-override");
    }
    AiClientFactory::SetTestOverride(nullptr);
}
