// libFuzzer driver for the AI endpoint URL validator —
// smatchet::ai::pure::SanitizeAiEndpointUrl + ExtractUrlHost (AiEndpointSanitize.cpp).
// Defense-in-depth against config-write SSRF: a user (or a malicious `config.set`)
// must not be able to repoint a provider URL at cloud-metadata / link-local /
// private-network addresses and exfil the API key + prompt. The validator parses an
// attacker-controlled URL string (scheme/host/port/IPv4/IPv6 literal handling) — a
// dense, bug-prone byte surface. The fuzz bytes ARE the URL. Drives both overloads,
// the host extractor, and the verdict describer so every parse branch is reachable.
// nightly-monkey-tester Layer 3.
#include "AiEndpointSanitize.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string url(reinterpret_cast<const char*>(data), size);
    using namespace smatchet::ai::pure;

    std::string out;
    (void)SanitizeAiEndpointUrl(url, out); // legacy permissive overload

    out.clear();
    EndpointPolicy policy;                 // strict-ish: pin a canonical host, no consents
    policy.CanonicalHost = "api.openai.com";
    policy.AllowCustomHost = false;
    policy.AllowInsecureHttp = false;
    const EndpointVerdict verdict = SanitizeAiEndpointUrl(url, policy, out);
    (void)EndpointVerdictDescription(verdict);

    (void)ExtractUrlHost(url);
    return 0;
}
