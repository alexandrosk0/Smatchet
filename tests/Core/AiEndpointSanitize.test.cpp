#include <doctest/doctest.h>

#include "AiEndpointSanitize.h"

using smatchet::ai::pure::EndpointVerdict;
using smatchet::ai::pure::EndpointVerdictDescription;
using smatchet::ai::pure::SanitizeAiEndpointUrl;

TEST_CASE("SanitizeAiEndpointUrl allows empty URL (use provider default)" * doctest::test_suite("[security]")) {
    std::string out = "non-empty";
    CHECK(SanitizeAiEndpointUrl("", out) == EndpointVerdict::Allowed);
    CHECK(out.empty());
}

TEST_CASE("SanitizeAiEndpointUrl allows https provider hosts" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com", out) == EndpointVerdict::Allowed);
    CHECK(out == "https://api.openai.com");
    CHECK(SanitizeAiEndpointUrl("https://api.anthropic.com/v1/messages", out) == EndpointVerdict::Allowed);
    CHECK(out == "https://api.anthropic.com/v1/messages");
}

TEST_CASE("SanitizeAiEndpointUrl allows http loopback (ollama)" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("http://localhost:11434", out) == EndpointVerdict::Allowed);
    CHECK(out == "http://localhost:11434");
    CHECK(SanitizeAiEndpointUrl("http://127.0.0.1:11434/v1", out) == EndpointVerdict::Allowed);
}

TEST_CASE("SanitizeAiEndpointUrl rejects non-http schemes" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("file:///etc/passwd", out) == EndpointVerdict::RejectedBadScheme);
    CHECK(out.empty());
    CHECK(SanitizeAiEndpointUrl("ftp://example.com", out) == EndpointVerdict::RejectedBadScheme);
    CHECK(SanitizeAiEndpointUrl("javascript:alert(1)", out) == EndpointVerdict::RejectedBadScheme);
    CHECK(SanitizeAiEndpointUrl("api.openai.com", out) == EndpointVerdict::RejectedBadScheme); // missing scheme
}

TEST_CASE("SanitizeAiEndpointUrl rejects CR/LF/NUL (header smuggling)" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com\r\nX-Evil: yes", out) == EndpointVerdict::RejectedControlChars);
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com\nFoo", out) == EndpointVerdict::RejectedControlChars);
    std::string withNul = "https://api.openai.com";
    withNul.push_back('\0');
    withNul.append("/extra");
    CHECK(SanitizeAiEndpointUrl(withNul, out) == EndpointVerdict::RejectedControlChars);
}

TEST_CASE("SanitizeAiEndpointUrl rejects cloud-metadata IPs (SSRF)" * doctest::test_suite("[security]")) {
    std::string out;
    // AWS / GCP / Azure IMDS.
    CHECK(SanitizeAiEndpointUrl("http://169.254.169.254/latest/meta-data/", out) ==
          EndpointVerdict::RejectedCloudMetadata);
    CHECK(SanitizeAiEndpointUrl("https://169.254.169.254", out) == EndpointVerdict::RejectedCloudMetadata);
    // Alibaba.
    CHECK(SanitizeAiEndpointUrl("http://100.100.100.200/latest/meta-data/", out) ==
          EndpointVerdict::RejectedCloudMetadata);
}

TEST_CASE("SanitizeAiEndpointUrl rejects link-local 169.254/16 (SSRF)" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("http://169.254.1.1", out) == EndpointVerdict::RejectedLinkLocal);
    CHECK(SanitizeAiEndpointUrl("http://169.254.254.254/path", out) == EndpointVerdict::RejectedLinkLocal);
    // Boundary: 169.254.169.254 is RejectedCloudMetadata, not LinkLocal, by precedence.
    CHECK(SanitizeAiEndpointUrl("http://169.254.169.254", out) == EndpointVerdict::RejectedCloudMetadata);
}

TEST_CASE("SanitizeAiEndpointUrl rejects malformed (missing host)" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https:///path", out) == EndpointVerdict::RejectedMalformed);
    CHECK(SanitizeAiEndpointUrl("http:///", out) == EndpointVerdict::RejectedMalformed);
}

TEST_CASE("EndpointVerdictDescription returns non-empty for every value" * doctest::test_suite("[security]")) {
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::Allowed)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedBadScheme)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedControlChars)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedCloudMetadata)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedLinkLocal)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedMalformed)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedNonProviderHost)).size() > 0);
    CHECK(std::string(EndpointVerdictDescription(EndpointVerdict::RejectedInsecureHttp)).size() > 0);
}

TEST_CASE("ExtractUrlHost parses + lowercases the host, port stripped" * doctest::test_suite("[security]")) {
    using smatchet::ai::pure::ExtractUrlHost;
    CHECK(ExtractUrlHost("https://api.openai.com/v1") == "api.openai.com");
    CHECK(ExtractUrlHost("https://API.OpenAI.com:443/v1") == "api.openai.com");
    CHECK(ExtractUrlHost("http://127.0.0.1:11434") == "127.0.0.1");
    // The subdomain-attack shape must NOT collapse to the canonical host — this is
    // exactly why the config migration compares the host, not a substring.
    CHECK(ExtractUrlHost("https://api.openai.com.proxy.corp/v1") == "api.openai.com.proxy.corp");
    // unparseable / schemeless -> empty
    CHECK(ExtractUrlHost("").empty());
    CHECK(ExtractUrlHost("api.openai.com").empty());
    CHECK(ExtractUrlHost("not a url").empty());
}

// --- Per-provider policy path (B4 — host pin + insecure-http consent) ---

using smatchet::ai::pure::EndpointPolicy;

namespace {
// Strict cloud policy: pinned host, no custom-host / insecure-http consent.
EndpointPolicy StrictOpenAi() {
    EndpointPolicy p;
    p.CanonicalHost = "api.openai.com";
    p.AllowCustomHost = false;
    p.AllowInsecureHttp = false;
    return p;
}
} // namespace

TEST_CASE("policy: strict OpenAi allows the canonical host over https" * doctest::test_suite("[security]")) {
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com/v1", StrictOpenAi(), out) == EndpointVerdict::Allowed);
    CHECK(out == "https://api.openai.com/v1");
    // host match is case-insensitive + port-tolerant
    CHECK(SanitizeAiEndpointUrl("https://API.OpenAI.com:443", StrictOpenAi(), out) == EndpointVerdict::Allowed);
}

TEST_CASE("policy: strict OpenAi rejects a non-provider host (SSRF repoint)" * doctest::test_suite("[security]")) {
    std::string out = "seed";
    CHECK(SanitizeAiEndpointUrl("https://attacker.example.com/v1", StrictOpenAi(), out) ==
          EndpointVerdict::RejectedNonProviderHost);
    CHECK(out.empty());
    // even a look-alike subdomain is rejected (exact host match)
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com.evil.test", StrictOpenAi(), out) ==
          EndpointVerdict::RejectedNonProviderHost);
}

TEST_CASE("policy: AllowCustomHost consent lets a proxy host through" * doctest::test_suite("[security]")) {
    EndpointPolicy p = StrictOpenAi();
    p.AllowCustomHost = true;
    p.AllowInsecureHttp = true;
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https://litellm.internal.corp/v1", p, out) == EndpointVerdict::Allowed);
    CHECK(out == "https://litellm.internal.corp/v1");
}

TEST_CASE("policy: plain http to a non-loopback host needs insecure-http consent" * doctest::test_suite("[security]")) {
    std::string out = "seed";
    // strict: blocked
    CHECK(SanitizeAiEndpointUrl("http://proxy.corp:8080/v1", StrictOpenAi(), out) ==
          EndpointVerdict::RejectedInsecureHttp);
    CHECK(out.empty());
    // loopback http on a PINNED provider is NOT auto-exempt — repointing a
    // key-bearing cloud request at a local listener still needs consent.
    CHECK(SanitizeAiEndpointUrl("http://127.0.0.1:1234/v1", StrictOpenAi(), out) ==
          EndpointVerdict::RejectedInsecureHttp);
    CHECK(out.empty());
    // loopback http is auto-allowed only for an UNPINNED/local provider (Ollama).
    EndpointPolicy ollama; // permissive defaults, empty CanonicalHost
    CHECK(SanitizeAiEndpointUrl("http://localhost:11434", ollama, out) == EndpointVerdict::Allowed);
    CHECK(SanitizeAiEndpointUrl("http://127.0.0.1:1234/v1", ollama, out) == EndpointVerdict::Allowed);
    // with consent: a remote http proxy is allowed (host still pinned unless AllowCustomHost)
    EndpointPolicy httpOk = StrictOpenAi();
    httpOk.AllowInsecureHttp = true;
    httpOk.AllowCustomHost = true;
    CHECK(SanitizeAiEndpointUrl("http://proxy.corp:8080/v1", httpOk, out) == EndpointVerdict::Allowed);
}

TEST_CASE("policy: pinned provider rejects loopback https without custom-host consent" *
          doctest::test_suite("[security]")) {
    std::string out = "seed";
    // https://127.0.0.1 for a pinned provider is a host mismatch (not the canonical
    // host) and must be rejected unless custom-host consent is granted.
    CHECK(SanitizeAiEndpointUrl("https://127.0.0.1:8443/v1", StrictOpenAi(), out) ==
          EndpointVerdict::RejectedNonProviderHost);
    EndpointPolicy consented = StrictOpenAi();
    consented.AllowCustomHost = true;
    CHECK(SanitizeAiEndpointUrl("https://127.0.0.1:8443/v1", consented, out) == EndpointVerdict::Allowed);
}

TEST_CASE("policy: SSRF pivots are rejected even with full consent" * doctest::test_suite("[security]")) {
    EndpointPolicy permissive; // defaults: AllowCustomHost + AllowInsecureHttp true, no pin
    std::string out;
    // cloud-metadata + link-local always lose, consent or not
    CHECK(SanitizeAiEndpointUrl("http://169.254.169.254/latest/meta-data", permissive, out) ==
          EndpointVerdict::RejectedCloudMetadata);
    CHECK(SanitizeAiEndpointUrl("http://169.254.0.1/", permissive, out) == EndpointVerdict::RejectedLinkLocal);
}

TEST_CASE("policy: unpinned provider (empty CanonicalHost) allows any https host" * doctest::test_suite("[security]")) {
    EndpointPolicy ollama; // permissive defaults, no pin — mirrors Ollama / DeepSeek
    std::string out;
    CHECK(SanitizeAiEndpointUrl("https://api.deepseek.com", ollama, out) == EndpointVerdict::Allowed);
    CHECK(SanitizeAiEndpointUrl("https://anything.example.com", ollama, out) == EndpointVerdict::Allowed);
}

TEST_CASE("legacy two-arg overload stays permissive (no regression)" * doctest::test_suite("[security]")) {
    std::string out;
    // pre-B4 behaviour: any non-SSRF host allowed, http allowed
    CHECK(SanitizeAiEndpointUrl("https://attacker.example.com", out) == EndpointVerdict::Allowed);
    CHECK(SanitizeAiEndpointUrl("http://proxy.corp:8080", out) == EndpointVerdict::Allowed);
}
