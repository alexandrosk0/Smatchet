// Shared provider->AiClientConfig seam (AGENTIC_INFRA_AUDIT.md B4 / P4) — the
// production BuildClientConfig consumed by both AiAssistantController and the
// debug ai.* commands (ai.dump-request / ai.probe / ai.send-once). Asserts the
// sanitizers the debug path used to skip: header-value control-char stripping
// and the endpoint sanitize-with-consent gate.

#include "AiRequestBuilder.h"
#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::ai::BuildClientConfig;
using smatchet::ai::SanitizeHeaderValue;

TEST_CASE("SanitizeHeaderValue strips CR/LF/NUL and passes clean values through") {
    CHECK(SanitizeHeaderValue("sk-clean-key") == "sk-clean-key");
    CHECK(SanitizeHeaderValue("sk-a\r\nX-Evil: yes") == "sk-aX-Evil: yes");
    CHECK(SanitizeHeaderValue(std::string("a\0b", 3)) == "ab");
    CHECK(SanitizeHeaderValue(std::string()).empty());
}

TEST_CASE("BuildClientConfig sanitizes the per-provider API key") {
    TrackerConfig cfg;
    cfg.AiApiKey = "sk-open\r\nai";
    cfg.AiAnthropicApiKey = "sk-ant\nhro";
    const AiClientConfig openai = BuildClientConfig(cfg, AiProvider::OpenAi);
    CHECK(openai.ApiKey == "sk-openai");
    const AiClientConfig anthropic = BuildClientConfig(cfg, AiProvider::Anthropic);
    CHECK(anthropic.ApiKey == "sk-anthro");
    // Ollama-native has no API key regardless of what is configured.
    const AiClientConfig ollama = BuildClientConfig(cfg, AiProvider::OllamaNative);
    CHECK(ollama.ApiKey.empty());
}

TEST_CASE("BuildClientConfig rejects an SSRF endpoint and falls back to empty BaseUrl") {
    TrackerConfig cfg;
    cfg.AiBaseUrl = "http://169.254.169.254/latest/meta-data";
    const AiClientConfig out = BuildClientConfig(cfg, AiProvider::OpenAi);
    CHECK(out.BaseUrl.empty());
}

TEST_CASE("BuildClientConfig enforces the OpenAI host pin unless consent is granted") {
    TrackerConfig cfg;
    cfg.AiBaseUrl = "https://proxy.example.test/v1";
    cfg.AiAllowCustomEndpointOpenAi = false;
    CHECK(BuildClientConfig(cfg, AiProvider::OpenAi).BaseUrl.empty());
    cfg.AiAllowCustomEndpointOpenAi = true;
    CHECK(BuildClientConfig(cfg, AiProvider::OpenAi).BaseUrl == "https://proxy.example.test/v1");
}

TEST_CASE("BuildClientConfig keeps the loopback Ollama endpoint and the DeepSeek default") {
    TrackerConfig cfg;
    cfg.AiOllamaBaseUrl = "http://localhost:11434";
    CHECK(BuildClientConfig(cfg, AiProvider::OllamaNative).BaseUrl == "http://localhost:11434");
    cfg.AiDeepSeekBaseUrl.clear();
    CHECK(BuildClientConfig(cfg, AiProvider::DeepSeek).BaseUrl == "https://api.deepseek.com");
}

TEST_CASE("BuildClientConfig applies the streaming total timeout") {
    TrackerConfig cfg;
    CHECK(BuildClientConfig(cfg, AiProvider::OpenAi).TotalTimeoutMs == 600000);
}
