// AiPrefsValidator pure-logic tests - per-provider field presence + format
// sniff + catalog-known-model warnings. No network. No ConfigManager file I/O
// (we construct TrackerConfig in-place).

#include "AiPrefsValidator.h"

#include "AiTypes.h"
#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

bool ContainsSubstring(const std::vector<std::string>& msgs, const std::string& needle) {
    return std::any_of(msgs.begin(), msgs.end(),
                       [&needle](const std::string& m) { return m.find(needle) != std::string::npos; });
}

TrackerConfig DefaultCfg() {
    TrackerConfig cfg;
    // Cfg ctor leaves AiApiKey / AiAnthropicApiKey empty. Models default to
    // valid catalog IDs (gpt-4o-mini / claude-sonnet-4-6 / llama3) via the
    // struct member initializers in ConfigManager.h.
    return cfg;
}

} // namespace

TEST_CASE("AiPrefsValidator: empty OpenAI config errors on missing key only") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0; // OpenAi
    cfg.AiApiKey.clear();   // missing
    cfg.AiAnthropicApiKey.clear();
    // Anthropic key MISSING - but we are not the Anthropic provider, so no error.
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "OpenAI"));
    CHECK(ContainsSubstring(v.Errors, "API key required"));
    // The stale Anthropic key absence MUST NOT produce a separate error - the
    // validator is per-active-provider, not all-providers.
    CHECK_FALSE(ContainsSubstring(v.Errors, "Anthropic"));
}

TEST_CASE("AiPrefsValidator: empty Anthropic config errors on missing Anthropic key") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 1; // Anthropic
    cfg.AiAnthropicApiKey.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "Anthropic"));
    CHECK(ContainsSubstring(v.Errors, "API key required"));
}

TEST_CASE("AiPrefsValidator: OpenAI with valid key + model passes") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "sk-some-valid-looking-key";
    cfg.AiModelOpenAi = "gpt-4o-mini";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK(v.Errors.empty());
    CHECK(v.Warnings.empty());
}

TEST_CASE("AiPrefsValidator: Anthropic with valid key + valid model passes") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 1;
    cfg.AiAnthropicApiKey = "sk-ant-some-key";
    cfg.AiModelAnthropic = "claude-sonnet-4-6";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK(v.Warnings.empty());
}

TEST_CASE("AiPrefsValidator: OpenAI key without 'sk-' prefix warns") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "definitely-not-sk-shape";
    cfg.AiModelOpenAi = "gpt-4o-mini";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(v.Warnings.empty());
    CHECK(ContainsSubstring(v.Warnings, "OpenAI"));
    CHECK(ContainsSubstring(v.Warnings, "sk-"));
}

TEST_CASE("AiPrefsValidator: Anthropic key without 'sk-ant-' prefix warns") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 1;
    cfg.AiAnthropicApiKey = "sk-not-anthropic-shape";
    cfg.AiModelAnthropic = "claude-sonnet-4-6";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(v.Warnings.empty());
    CHECK(ContainsSubstring(v.Warnings, "Anthropic"));
    CHECK(ContainsSubstring(v.Warnings, "sk-ant-"));
}

// [high-risk] Regression gate for the diagnostic case that motivated this
// slice. claude-sonnet-4-6 IS a valid Anthropic ID today; the catalog must
// include it so users persisting that value as their model don't get an
// unknown-model warning. If a future PR drops the catalog entry, this test
// fails loudly.
TEST_CASE("[high-risk] AiPrefsValidator: Anthropic 'claude-sonnet-4-6' is in catalog (no warning)") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 1;
    cfg.AiAnthropicApiKey = "sk-ant-fake";
    cfg.AiModelAnthropic = "claude-sonnet-4-6";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    // Specifically: no unknown-model warning for this ID. Forces the catalog
    // to keep claude-sonnet-4-6 listed; future Anthropic model lifecycle
    // changes that drop this ID must add an explicit deviation entry here.
    CHECK_FALSE(ContainsSubstring(v.Warnings, "claude-sonnet-4-6"));
}

TEST_CASE("AiPrefsValidator: invented Anthropic model warns") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 1;
    cfg.AiAnthropicApiKey = "sk-ant-fake";
    cfg.AiModelAnthropic = "claude-fictional-100-xyz";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(v.Warnings.empty());
    CHECK(ContainsSubstring(v.Warnings, "claude-fictional-100-xyz"));
    CHECK(ContainsSubstring(v.Warnings, "not in known catalog"));
}

TEST_CASE("AiPrefsValidator: bad base URL errors") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "sk-test";
    cfg.AiBaseUrl = "not-a-url";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "http"));
}

TEST_CASE("AiPrefsValidator: valid base URL passes URL check") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "sk-test";
    cfg.AiBaseUrl = "https://api.openai.com/";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
}

TEST_CASE("AiPrefsValidator: empty model field errors") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "sk-test";
    cfg.AiModelOpenAi.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "model ID required"));
}

TEST_CASE("AiPrefsValidator: OllamaNative with blank base URL warns about default") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 3; // OllamaNative
    cfg.AiOllamaBaseUrl.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(v.Warnings.empty());
    CHECK(ContainsSubstring(v.Warnings, "localhost:11434"));
}

TEST_CASE("AiPrefsValidator: OllamaNative bad base URL errors") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 3;
    cfg.AiOllamaBaseUrl = "ftp://nope";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "Ollama"));
    CHECK(ContainsSubstring(v.Errors, "http"));
}

TEST_CASE("AiPrefsValidator: out-of-range AiProviderKind clamps to OpenAI") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 99; // out of enum
    cfg.AiApiKey.clear();    // missing -> OpenAI error
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "OpenAI"));
}

// --- Local OpenAI-compatible (LM Studio / Ollama / LocalAI / vLLM) ---
// The OllamaOpenAiCompat slot covers any local OpenAI-compatible server.
// API key is OPTIONAL for these servers - empty key must NOT block save.
TEST_CASE("AiPrefsValidator: OllamaOpenAiCompat with EMPTY key passes (LM Studio etc are local)") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 2; // OllamaOpenAiCompat
    cfg.AiApiKey.clear();   // empty - must not error for local servers
    cfg.AiModelOpenAi = "any-local-model";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK(v.Errors.empty());
}

// And even when a key IS supplied for a local server, the 'sk-' format sniff
// must NOT fire - local servers often accept any token or use exotic formats.
TEST_CASE("AiPrefsValidator: OllamaOpenAiCompat with non-sk- key produces no warning") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 2;
    cfg.AiApiKey = "anything";
    cfg.AiModelOpenAi = "local-model";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(ContainsSubstring(v.Warnings, "sk-"));
}

// OllamaNative has always been keyless; reaffirm here so the pattern is uniform.
TEST_CASE("AiPrefsValidator: OllamaNative with EMPTY key passes (no key required)") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 3; // OllamaNative
    cfg.AiApiKey.clear();
    cfg.AiAnthropicApiKey.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    // OllamaNative emits a warning about default-URL when AiOllamaBaseUrl
    // is blank but no key error.
    CHECK(v.IsOk());
}

// --- Structured PrefsValidationIssue records (Fix 5b) ---
// Each emitted error / warning must carry a FieldKey mapping so the
// Preferences UI can render per-field glyphs.
TEST_CASE("AiPrefsValidator: missing OpenAI key issue carries FieldKey kAiApiKey") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    REQUIRE_FALSE(v.Issues.empty());
    bool sawKeyIssue = false;
    for (const auto& iss : v.Issues) {
        if (iss.FieldKey == smatchet::ai::PrefsFieldKey::kAiApiKey) {
            CHECK(iss.Severity == smatchet::ai::PrefsSeverity::Error);
            CHECK(iss.Message.find("OpenAI") != std::string::npos);
            sawKeyIssue = true;
        }
    }
    CHECK(sawKeyIssue);
}

TEST_CASE("AiPrefsValidator: malformed key warning carries FieldKey + Warning severity") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "not-sk-prefix-key";
    cfg.AiModelOpenAi = "gpt-4o-mini";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk()); // warning only
    bool sawKeyWarn = false;
    for (const auto& iss : v.Issues) {
        if (iss.FieldKey == smatchet::ai::PrefsFieldKey::kAiApiKey &&
            iss.Severity == smatchet::ai::PrefsSeverity::Warning) {
            sawKeyWarn = true;
        }
    }
    CHECK(sawKeyWarn);
}

// --- DeepSeek (provider 4) ---
//
// DeepSeek is a hosted OpenAI-compatible provider. Behaviour mirrors Anthropic:
//   * key required when slot empty
//   * known catalog entries pass without warning
//   * unknown model warns
//   * key format sniff: 'sk-' prefix (same shape as OpenAI keys per DeepSeek docs)

TEST_CASE("AiPrefsValidator: empty DeepSeek config errors on missing DeepSeek key") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 4; // DeepSeek
    cfg.AiDeepSeekApiKey.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "DeepSeek"));
    CHECK(ContainsSubstring(v.Errors, "API key required"));
}

TEST_CASE("AiPrefsValidator: DeepSeek with valid key + known model passes (no warnings)") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 4;
    cfg.AiDeepSeekApiKey = "sk-deepseek-fake";
    cfg.AiModelDeepSeek = "deepseek-chat";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK(v.Warnings.empty());
}

TEST_CASE("AiPrefsValidator: DeepSeek reasoner model recognised in catalog") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 4;
    cfg.AiDeepSeekApiKey = "sk-fake";
    cfg.AiModelDeepSeek = "deepseek-reasoner";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(ContainsSubstring(v.Warnings, "deepseek-reasoner"));
}

TEST_CASE("AiPrefsValidator: invented DeepSeek model warns") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 4;
    cfg.AiDeepSeekApiKey = "sk-fake";
    cfg.AiModelDeepSeek = "deepseek-fictional-99";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(v.Warnings.empty());
    CHECK(ContainsSubstring(v.Warnings, "deepseek-fictional-99"));
    CHECK(ContainsSubstring(v.Warnings, "not in known catalog"));
}

TEST_CASE("AiPrefsValidator: DeepSeek key without 'sk-' prefix warns") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 4;
    cfg.AiDeepSeekApiKey = "nope-no-prefix";
    cfg.AiModelDeepSeek = "deepseek-chat";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(v.Warnings.empty());
    CHECK(ContainsSubstring(v.Warnings, "DeepSeek"));
    CHECK(ContainsSubstring(v.Warnings, "sk-"));
}

TEST_CASE("AiPrefsValidator: DeepSeek with Anthropic-shape 'sk-ant-' key warns cross-paste") {
    // Regression: an Anthropic key pasted into the DeepSeek slot
    // satisfies the loose `sk-` prefix check, slips through validate, and then yields a
    // server-side HTTP 401 with no Smatchet-side clue what went wrong. Tight check on
    // the Anthropic-exclusive `sk-ant-` prefix surfaces the mistake before the network.
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 4;
    cfg.AiDeepSeekApiKey = "sk-ant-api03-anthropic-key-pasted-into-deepseek-slot";
    cfg.AiModelDeepSeek = "deepseek-chat";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(v.Warnings.empty());
    CHECK(ContainsSubstring(v.Warnings, "DeepSeek"));
    CHECK(ContainsSubstring(v.Warnings, "sk-ant-"));
    CHECK(ContainsSubstring(v.Warnings, "Anthropic"));
}

TEST_CASE("AiPrefsValidator: OpenAI with Anthropic-shape 'sk-ant-' key warns cross-paste") {
    // Symmetric guard: same mistake against the OpenAI slot. OpenAI keys begin `sk-` but
    // never `sk-ant-`, so the prefix is a reliable signal of a wrong-slot paste.
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "sk-ant-api03-anthropic-key-pasted-into-openai-slot";
    cfg.AiModelOpenAi = "gpt-4o";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(v.Warnings.empty());
    CHECK(ContainsSubstring(v.Warnings, "OpenAI"));
    CHECK(ContainsSubstring(v.Warnings, "sk-ant-"));
    CHECK(ContainsSubstring(v.Warnings, "Anthropic"));
}

TEST_CASE("AiPrefsValidator: DeepSeek missing-key issue carries FieldKey kAiDeepSeekApiKey") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 4;
    cfg.AiDeepSeekApiKey.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    REQUIRE_FALSE(v.Issues.empty());
    bool sawKeyIssue = false;
    for (const auto& iss : v.Issues) {
        if (iss.FieldKey == smatchet::ai::PrefsFieldKey::kAiDeepSeekApiKey) {
            CHECK(iss.Severity == smatchet::ai::PrefsSeverity::Error);
            CHECK(iss.Message.find("DeepSeek") != std::string::npos);
            sawKeyIssue = true;
        }
    }
    CHECK(sawKeyIssue);
}

// --- Helper-decomposition coverage (PR B8-B9 function-size split) ---
// ValidateAiPrefs was decomposed into CheckKeyPresence / ResolveActiveModel /
// CheckBaseUrls / CheckKeyFormat / CheckUnknownModel without behaviour change.
// These cases pin the per-helper branches the split surfaced.

TEST_CASE("AiPrefsValidator: missing-model error carries the active provider's model FieldKey (Anthropic)") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 1; // Anthropic
    cfg.AiAnthropicApiKey = "sk-ant-fake";
    cfg.AiModelAnthropic.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "model ID required"));
    bool sawModelIssue = false;
    for (const auto& iss : v.Issues) {
        if (iss.FieldKey == smatchet::ai::PrefsFieldKey::kAiModelAnthropic &&
            iss.Severity == smatchet::ai::PrefsSeverity::Error) {
            sawModelIssue = true;
        }
    }
    CHECK(sawModelIssue);
}

TEST_CASE("AiPrefsValidator: OllamaOpenAiCompat resolves its model from the OpenAI model field") {
    // The compat slot shares AiModelOpenAi. An empty AiModelOpenAi must yield a
    // missing-model error keyed to the OpenAI model field, with the compat label.
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 2; // OllamaOpenAiCompat
    cfg.AiApiKey.clear();   // optional for local
    cfg.AiModelOpenAi.clear();
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "model ID required"));
    bool sawCompatModelKey = false;
    for (const auto& iss : v.Issues) {
        if (iss.FieldKey == smatchet::ai::PrefsFieldKey::kAiModelOpenAi &&
            iss.Severity == smatchet::ai::PrefsSeverity::Error) {
            sawCompatModelKey = true;
        }
    }
    CHECK(sawCompatModelKey);
}

TEST_CASE("AiPrefsValidator: AiBaseUrl is validated for non-Ollama providers (DeepSeek)") {
    // CheckBaseUrls validates AiBaseUrl unconditionally; a malformed value errors
    // even when the active provider is DeepSeek (which reads its own DeepSeek URL).
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 4; // DeepSeek
    cfg.AiDeepSeekApiKey = "sk-fake";
    cfg.AiModelDeepSeek = "deepseek-chat";
    cfg.AiBaseUrl = "not-a-url";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    bool sawBaseIssue = false;
    for (const auto& iss : v.Issues) {
        if (iss.FieldKey == smatchet::ai::PrefsFieldKey::kAiBaseUrl &&
            iss.Severity == smatchet::ai::PrefsSeverity::Error) {
            sawBaseIssue = true;
        }
    }
    CHECK(sawBaseIssue);
}

TEST_CASE("AiPrefsValidator: bad AiBaseUrl issue carries FieldKey kAiBaseUrl") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "sk-x";
    cfg.AiBaseUrl = "not-a-url";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    bool sawBaseIssue = false;
    for (const auto& iss : v.Issues) {
        if (iss.FieldKey == smatchet::ai::PrefsFieldKey::kAiBaseUrl) {
            CHECK(iss.Severity == smatchet::ai::PrefsSeverity::Error);
            sawBaseIssue = true;
        }
    }
    CHECK(sawBaseIssue);
}

// --- SSRF denylist at prefs time (security audit Item B) ---
// CheckBaseUrls now routes every configured base URL through the shared
// SanitizeAiEndpointUrl (EndpointPolicyForProvider), so Preferences rejects the
// exact set of hosts the live request refuses — no looser. Previously a
// cloud-metadata / private / alternate-encoding host saved clean here and was
// only caught at request time. These [high-risk] cases lock the prefs-time
// enforcement so a future refactor cannot silently re-loosen the gate. The
// OllamaOpenAiCompat provider is unpinned (no host pin, http allowed), so ONLY
// the SSRF denylist can produce the rejection — isolating the behaviour we test.

TEST_CASE("[high-risk] AiPrefsValidator: cloud-metadata IP in AiBaseUrl is rejected at prefs time") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 2; // OllamaOpenAiCompat — unpinned, isolates the denylist
    cfg.AiModelOpenAi = "local-model";
    cfg.AiBaseUrl = "http://169.254.169.254/latest/meta-data/";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "cloud-metadata"));
    bool keyed = false;
    for (const auto& iss : v.Issues) {
        if (iss.FieldKey == smatchet::ai::PrefsFieldKey::kAiBaseUrl &&
            iss.Severity == smatchet::ai::PrefsSeverity::Error) {
            keyed = true;
        }
    }
    CHECK(keyed);
}

TEST_CASE("[high-risk] AiPrefsValidator: decimal-encoded metadata IP in AiBaseUrl is rejected") {
    // 2852039166 == 169.254.169.254. The canonicaliser must collapse the alternate
    // encoding before the denylist compare, else the scheme-only check would pass it.
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 2;
    cfg.AiModelOpenAi = "local-model";
    cfg.AiBaseUrl = "http://2852039166/";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "cloud-metadata"));
}

TEST_CASE("[high-risk] AiPrefsValidator: private-network IP in AiBaseUrl is rejected") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 2;
    cfg.AiModelOpenAi = "local-model";
    cfg.AiBaseUrl = "http://192.168.1.50:11434";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "private-network"));
}

TEST_CASE("[high-risk] AiPrefsValidator: OpenAI non-provider host rejected without custom-endpoint consent") {
    // Mirrors the request-time host pin: a config-write that repoints OpenAI's
    // key-bearing request at an arbitrary host must be refused unless the user
    // explicitly enabled the per-provider custom-endpoint toggle.
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0; // OpenAi (pinned to api.openai.com)
    cfg.AiApiKey = "sk-test";
    cfg.AiModelOpenAi = "gpt-4o-mini";
    cfg.AiBaseUrl = "https://gateway.evil.test/v1";
    cfg.AiAllowCustomEndpointOpenAi = false;
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK_FALSE(v.IsOk());
    CHECK(ContainsSubstring(v.Errors, "non-provider host"));
}

TEST_CASE("AiPrefsValidator: OpenAI custom host PASSES once custom-endpoint consent is granted") {
    // Same shape as the rejection case, but with consent — must now validate clean,
    // matching what the live request would accept.
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 0;
    cfg.AiApiKey = "sk-test";
    cfg.AiModelOpenAi = "gpt-4o-mini";
    cfg.AiBaseUrl = "https://gateway.example.test/v1";
    cfg.AiAllowCustomEndpointOpenAi = true;
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK_FALSE(ContainsSubstring(v.Errors, "non-provider host"));
}

TEST_CASE("AiPrefsValidator: local Ollama http://localhost:11434 stays allowed (no false positive)") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 3; // OllamaNative
    cfg.AiOllamaBaseUrl = "http://localhost:11434";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK(v.Errors.empty());
}

TEST_CASE("AiPrefsValidator: local Ollama http://127.0.0.1:11434 stays allowed (loopback exempt)") {
    TrackerConfig cfg = DefaultCfg();
    cfg.AiProviderKind = 3; // OllamaNative
    cfg.AiOllamaBaseUrl = "http://127.0.0.1:11434";
    const auto v = smatchet::ai::ValidateAiPrefs(cfg);
    CHECK(v.IsOk());
    CHECK(v.Errors.empty());
}
