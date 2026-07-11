#include <doctest/doctest.h>

// Coverage for the pre-network half of the preferences Test-connection probe
// (TEST_COVERAGE_GAP_MAP.md Tier 1 #8). The contract under test is credential ROUTING:
// which API-key slot, base URL, and model id each provider reads (the CPP_CODE_AUDIT.md #2
// defect class — a mis-routed credential silently probes unauthenticated), the
// local-provider default-URL fallback, the header-smuggling key strip, and the
// endpoint-sanitiser gate falling back to the provider default on rejection.
#include "AiPrefsTestConnectionPure.h"

#include <string>

using AiPrefsTestConnectionPure::BuildProbeClientConfig;
using AiPrefsTestConnectionPure::DefaultBaseUrlFor;
using AiPrefsTestConnectionPure::PlanProbe;
using AiPrefsTestConnectionPure::ProbePlan;
using AiPrefsTestConnectionPure::ProbeTarget;
using AiPrefsTestConnectionPure::ResolveProbeTarget;

namespace {

TrackerConfig MakeCfg() {
    TrackerConfig cfg;
    cfg.AiApiKey = "openai-key";
    cfg.AiAnthropicApiKey = "anthropic-key";
    cfg.AiDeepSeekApiKey = "deepseek-key";
    cfg.AiBaseUrl = "https://openai.example";
    cfg.AiOllamaBaseUrl = "http://127.0.0.1:11434";
    cfg.AiDeepSeekBaseUrl = "https://deepseek.example";
    cfg.AiModelOpenAi = "gpt-x";
    cfg.AiModelAnthropic = "claude-x";
    cfg.AiModelOllama = "llama-x";
    cfg.AiModelDeepSeek = "deepseek-x";
    return cfg;
}

smatchet::ai::pure::EndpointPolicy OpenPolicy() {
    smatchet::ai::pure::EndpointPolicy policy;
    policy.CanonicalHost.clear();
    policy.AllowCustomHost = true;
    policy.AllowInsecureHttp = true;
    return policy;
}

} // namespace

TEST_CASE("ResolveProbeTarget routes each provider to its own credential slot") {
    const TrackerConfig cfg = MakeCfg();

    const ProbeTarget openai = ResolveProbeTarget(cfg, AiProvider::OpenAi);
    CHECK(openai.ApiKey == "openai-key");
    CHECK(openai.BaseUrl == "https://openai.example");
    CHECK(openai.ModelId == "gpt-x");

    const ProbeTarget anthropic = ResolveProbeTarget(cfg, AiProvider::Anthropic);
    CHECK(anthropic.ApiKey == "anthropic-key");
    CHECK(anthropic.ModelId == "claude-x");

    const ProbeTarget deepseek = ResolveProbeTarget(cfg, AiProvider::DeepSeek);
    CHECK(deepseek.ApiKey == "deepseek-key");
    CHECK(deepseek.BaseUrl == "https://deepseek.example");
    CHECK(deepseek.ModelId == "deepseek-x");

    // Native Ollama never sends a key; it reads the Ollama URL slot.
    const ProbeTarget ollama = ResolveProbeTarget(cfg, AiProvider::OllamaNative);
    CHECK(ollama.ApiKey.empty());
    CHECK(ollama.BaseUrl == "http://127.0.0.1:11434");
    CHECK(ollama.ModelId == "llama-x");
}

TEST_CASE("OllamaOpenAiCompat prefers the OpenAI-compat URL slot, falling back to the Ollama slot") {
    TrackerConfig cfg = MakeCfg();
    const ProbeTarget withBase = ResolveProbeTarget(cfg, AiProvider::OllamaOpenAiCompat);
    CHECK(withBase.ApiKey == "openai-key");
    CHECK(withBase.BaseUrl == "https://openai.example");

    cfg.AiBaseUrl.clear();
    const ProbeTarget fallback = ResolveProbeTarget(cfg, AiProvider::OllamaOpenAiCompat);
    CHECK(fallback.BaseUrl == "http://127.0.0.1:11434");
}

TEST_CASE("DefaultBaseUrlFor: canonical local/hosted defaults; empty for key-canonical providers") {
    CHECK(DefaultBaseUrlFor(AiProvider::OllamaOpenAiCompat) == "http://127.0.0.1:1234");
    CHECK(DefaultBaseUrlFor(AiProvider::OllamaNative) == "http://localhost:11434");
    CHECK(DefaultBaseUrlFor(AiProvider::DeepSeek) == "https://api.deepseek.com");
    CHECK(DefaultBaseUrlFor(AiProvider::OpenAi).empty());
    CHECK(DefaultBaseUrlFor(AiProvider::Anthropic).empty());
}

TEST_CASE("BuildProbeClientConfig strips header-smuggling characters from the key") {
    std::string hostileKey = "sk-";
    hostileKey.push_back('\r');
    hostileKey.push_back('\n');
    hostileKey += "abc";
    hostileKey.push_back('\0');
    hostileKey += "def";
    const AiClientConfig cfg = BuildProbeClientConfig(hostileKey, "", OpenPolicy());
    CHECK(cfg.ApiKey == "sk-abcdef");
    CHECK(cfg.ConnectTimeoutMs == 5000);
    CHECK(cfg.TotalTimeoutMs == 15000);
}

TEST_CASE("BuildProbeClientConfig gates the base URL through the endpoint sanitiser") {
    // Loopback is always allowed (the legitimate local-Ollama case).
    const AiClientConfig ok = BuildProbeClientConfig("k", "http://127.0.0.1:1234", OpenPolicy());
    CHECK(!ok.BaseUrl.empty());
    CHECK(ok.BaseUrl.find("127.0.0.1") != std::string::npos);

    // Cloud-metadata SSRF pivot is always rejected -> empty base (provider default).
    const AiClientConfig rejected = BuildProbeClientConfig("k", "http://169.254.169.254/latest", OpenPolicy());
    CHECK(rejected.BaseUrl.empty());
}

TEST_CASE("PlanProbe substitutes and records the provider default when the slot is empty") {
    TrackerConfig cfg = MakeCfg();
    cfg.AiOllamaBaseUrl.clear();
    const ProbePlan plan = PlanProbe(cfg, AiProvider::OllamaNative, OpenPolicy());
    CHECK(plan.DefaultedBaseUrl == "http://localhost:11434");
    CHECK(!plan.ClientCfg.BaseUrl.empty());
    CHECK(plan.ModelId == "llama-x");
}

TEST_CASE("PlanProbe leaves DefaultedBaseUrl empty when the slot is configured") {
    const ProbePlan plan = PlanProbe(MakeCfg(), AiProvider::OllamaNative, OpenPolicy());
    CHECK(plan.DefaultedBaseUrl.empty());
    CHECK(plan.ClientCfg.BaseUrl.find("127.0.0.1") != std::string::npos);
}
