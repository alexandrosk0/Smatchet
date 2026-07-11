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

TEST_CASE("SanitizeAiEndpointUrl rejects alternate IPv4 encodings of the metadata IP (SSRF)" *
          doctest::test_suite("[security]")) {
    std::string out;
    // 169.254.169.254 re-encoded — every form must canonicalise + hit the denylist.
    // decimal (0xA9FEA9FE = 2852039166)
    CHECK(SanitizeAiEndpointUrl("http://2852039166/latest/meta-data/", out) == EndpointVerdict::RejectedCloudMetadata);
    // hex (single 32-bit literal)
    CHECK(SanitizeAiEndpointUrl("http://0xA9FEA9FE", out) == EndpointVerdict::RejectedCloudMetadata);
    CHECK(SanitizeAiEndpointUrl("http://0xa9fea9fe", out) == EndpointVerdict::RejectedCloudMetadata);
    // dotted octal
    CHECK(SanitizeAiEndpointUrl("http://0251.0376.0251.0376", out) == EndpointVerdict::RejectedCloudMetadata);
    // dotted hex
    CHECK(SanitizeAiEndpointUrl("http://0xA9.0xFE.0xA9.0xFE", out) == EndpointVerdict::RejectedCloudMetadata);
    // short form: 169.254.43518 packs the last two octets like inet_aton
    CHECK(SanitizeAiEndpointUrl("http://169.254.43518", out) == EndpointVerdict::RejectedCloudMetadata);
    // IPv4-mapped IPv6 (dotted tail)
    CHECK(SanitizeAiEndpointUrl("http://[::ffff:169.254.169.254]/latest", out) ==
          EndpointVerdict::RejectedCloudMetadata);
    // DR2: pure-hextet IPv4-mapped (::ffff:a9fe:a9fe) and IPv4-compatible (::a9fe:a9fe)
    // forms have NO dotted tail — they must still decode to 169.254.169.254 and be denied.
    CHECK(SanitizeAiEndpointUrl("http://[::ffff:a9fe:a9fe]/latest", out) == EndpointVerdict::RejectedCloudMetadata);
    CHECK(SanitizeAiEndpointUrl("http://[::a9fe:a9fe]", out) == EndpointVerdict::RejectedCloudMetadata);
    // hex-grouped private / link-local mapped forms too
    CHECK(SanitizeAiEndpointUrl("http://[::ffff:0a00:0005]", out) == EndpointVerdict::RejectedPrivateNetwork); // 10.0.0.5
    CHECK(SanitizeAiEndpointUrl("http://[::ffff:a9fe:0101]", out) == EndpointVerdict::RejectedLinkLocal);      // 169.254.1.1
}

TEST_CASE("SanitizeAiEndpointUrl strips URL userinfo before host validation (SSRF)" *
          doctest::test_suite("[security]")) {
    using smatchet::ai::pure::EndpointPolicy;
    using smatchet::ai::pure::SanitizeAiEndpointUrl;
    std::string out;
    // DR2: "user:pass@realhost" — the host is everything after the LAST '@', not the
    // userinfo. A metadata IP hidden behind fake userinfo must still be denied.
    CHECK(SanitizeAiEndpointUrl("http://api.openai.com:x@169.254.169.254/latest", out) ==
          EndpointVerdict::RejectedCloudMetadata);
    CHECK(SanitizeAiEndpointUrl("http://a:b@10.0.0.5", out) == EndpointVerdict::RejectedPrivateNetwork);
    // Against a pinned provider, userinfo spoofing the canonical host must not pass the
    // host-pin when the real host differs.
    EndpointPolicy p;
    p.CanonicalHost = "api.openai.com";
    p.AllowCustomHost = false;
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com:x@evil.example.com/v1", p, out) ==
          EndpointVerdict::RejectedNonProviderHost);
    // A legitimate userinfo on the real canonical host still resolves to that host.
    CHECK(SanitizeAiEndpointUrl("https://user@api.openai.com/v1", p, out) == EndpointVerdict::Allowed);
}

TEST_CASE("SanitizeAiEndpointUrl rejects link-local + private + IPv6 ranges (SSRF)" *
          doctest::test_suite("[security]")) {
    std::string out;
    // link-local via decimal: 169.254.1.1 = 2851996161
    CHECK(SanitizeAiEndpointUrl("http://2851996161", out) == EndpointVerdict::RejectedLinkLocal);
    // RFC1918 private ranges (any encoding)
    CHECK(SanitizeAiEndpointUrl("http://10.0.0.5", out) == EndpointVerdict::RejectedPrivateNetwork);
    CHECK(SanitizeAiEndpointUrl("http://172.16.0.1", out) == EndpointVerdict::RejectedPrivateNetwork);
    CHECK(SanitizeAiEndpointUrl("http://192.168.1.1", out) == EndpointVerdict::RejectedPrivateNetwork);
    CHECK(SanitizeAiEndpointUrl("http://0xC0A80101", out) == EndpointVerdict::RejectedPrivateNetwork); // 192.168.1.1
    // IPv6 link-local + ULA
    CHECK(SanitizeAiEndpointUrl("http://[fe80::1]", out) == EndpointVerdict::RejectedLinkLocal);
    CHECK(SanitizeAiEndpointUrl("http://[fd00::1]/v1", out) == EndpointVerdict::RejectedPrivateNetwork);
    CHECK(SanitizeAiEndpointUrl("http://[fc00::1]", out) == EndpointVerdict::RejectedPrivateNetwork);
}

TEST_CASE("SanitizeAiEndpointUrl rejects the full fe80::/10 IPv6 link-local range, not just the fe80: prefix" *
          doctest::test_suite("[security]")) {
    std::string out;
    // CPP_CODE_AUDIT.md #16: the denylist used to string-match only "fe80:", letting the upper
    // half of the /10 (fe90:: through febf::) slip through as Allowed. IsIpv6LinkLocalHextet now
    // range-checks the first hextet against [0xfe80, 0xfebf] instead.
    CHECK(SanitizeAiEndpointUrl("http://[fe90::1]", out) == EndpointVerdict::RejectedLinkLocal);
    CHECK(SanitizeAiEndpointUrl("http://[fea0::1]", out) == EndpointVerdict::RejectedLinkLocal);
    CHECK(SanitizeAiEndpointUrl("http://[febf::1]", out) == EndpointVerdict::RejectedLinkLocal);
    // fec0:: is one hextet past the /10 boundary (old deprecated site-local, not link-local) —
    // must stay Allowed so the range check isn't accidentally widened past fe80::/10.
    CHECK(SanitizeAiEndpointUrl("http://[fec0::1]", out) == EndpointVerdict::Allowed);
}

TEST_CASE("SanitizeAiEndpointUrl: legitimate public host + IPv6 loopback accepted" *
          doctest::test_suite("[security]")) {
    std::string out;
    // a legitimate public host is unaffected by the canonicaliser
    CHECK(SanitizeAiEndpointUrl("https://api.openai.com/v1", out) == EndpointVerdict::Allowed);
    CHECK(out == "https://api.openai.com/v1");
    // IPv6 loopback is a legitimate local target (unpinned provider, like Ollama)
    CHECK(SanitizeAiEndpointUrl("http://[::1]:11434/v1", out) == EndpointVerdict::Allowed);
    // a public IPv6 literal is allowed for an unpinned provider (no denied prefix)
    CHECK(SanitizeAiEndpointUrl("https://[2001:4860:4860::8888]", out) == EndpointVerdict::Allowed);
}

TEST_CASE("SanitizeAiEndpointUrl: overflow + garbage IP-ish inputs are handled (no UB, rejected)" *
          doctest::test_suite("[security]")) {
    std::string out;
    // > 32-bit decimal must not wrap a denied IP into an allowed one — rejected as non-IP host.
    // 99999999999 is not a valid IPv4 integer; falls through to host-pin/allow (treated as a hostname-shape).
    // The key invariant: it must NOT be mis-canonicalised to a denied address.
    CHECK(SanitizeAiEndpointUrl("http://99999999999", out) != EndpointVerdict::RejectedCloudMetadata);
    // octal part with an out-of-range digit ('8') is not a valid octal octet
    CHECK(SanitizeAiEndpointUrl("http://0251.0376.0251.0378", out) != EndpointVerdict::RejectedCloudMetadata);
    // 5 dotted parts is not an IPv4 literal
    CHECK(SanitizeAiEndpointUrl("http://1.2.3.4.5", out) != EndpointVerdict::RejectedLinkLocal);
    // an octet > 255 in a 4-part form is rejected as an IP (256.254.169.254)
    CHECK(SanitizeAiEndpointUrl("http://256.254.169.254", out) != EndpointVerdict::RejectedCloudMetadata);
    // bare "0x" / lone dots do not crash and are never mis-read as a denied IP
    CHECK(SanitizeAiEndpointUrl("http://0x", out) != EndpointVerdict::RejectedCloudMetadata);
    CHECK(SanitizeAiEndpointUrl("http://...", out) != EndpointVerdict::RejectedCloudMetadata);
    CHECK(SanitizeAiEndpointUrl("http://...", out) != EndpointVerdict::RejectedLinkLocal);
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
