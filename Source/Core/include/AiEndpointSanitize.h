// AiEndpointSanitize.h — pure URL validator for AI provider endpoints.
// Lives in a sibling TU (AiEndpointSanitize.cpp) — no cpr / cpp-httplib /
// SQLite includes — so the doctest rig can link it without dragging the HTTP
// layer (per the Pure-helper TU-split recipe, AGENTS.md § Orchestrator
// delegation packet).
// Defense-in-depth against config-write SSRF: a user with write access to
// `smatchet_config.json` (per-user, but also reachable via roaming-profile
// sync, shared workstation, or a malicious `config.set` MCP call) must not be
// able to repoint Anthropic/OpenAI URLs at cloud-metadata or link-local
// addresses and exfil the API key + prompt + agents.md + audit payload via the
// outbound POST.

#ifndef SMATCHET_AI_ENDPOINT_SANITIZE_H
#define SMATCHET_AI_ENDPOINT_SANITIZE_H

#include <string>

namespace smatchet {
namespace ai {
namespace pure {

enum class EndpointVerdict {
    Allowed,
    RejectedBadScheme,       // not http:// or https://
    RejectedControlChars,    // CR / LF / NUL embedded (header smuggling)
    RejectedCloudMetadata,   // 169.254.169.254 / 100.100.100.200 etc.
    RejectedLinkLocal,       // 169.254.0.0/16 (link-local IPv4)
    RejectedMalformed,       // missing host, empty after scheme, etc.
    RejectedNonProviderHost, // host != provider canonical host + custom-endpoint consent not granted
    RejectedInsecureHttp,    // plain http:// to a non-loopback host + insecure-http consent not granted
};

// Per-provider enforcement policy threaded in by the caller (the pure validator
// has no ConfigManager access). Defaults are intentionally PERMISSIVE so the
// legacy two-arg overload + the unpinned providers (Ollama / DeepSeek) preserve
// the prior "any host" behaviour; the cloud providers (OpenAi / Anthropic) pass
// a strict policy built from the per-provider consent flags.
struct EndpointPolicy {
    // Canonical host the provider's API key is scoped to (e.g. "api.openai.com").
    // Empty = no host pinning (proxy-friendly; used for Ollama / DeepSeek).
    std::string CanonicalHost;
    // User granted explicit consent (Preferences toggle) to point this provider
    // at a non-canonical host. When false + CanonicalHost set, a mismatching host
    // is RejectedNonProviderHost.
    bool AllowCustomHost = true;
    // User granted explicit consent to use plain http:// to a NON-loopback host
    // (cleartext API key on the wire). Loopback (localhost / 127.0.0.0/8) is
    // always allowed regardless — the legitimate local-Ollama case.
    bool AllowInsecureHttp = true;
};

// Validate a configured AI endpoint URL against `policy`. `out_url` is populated
// with the normalised URL when the verdict is Allowed; otherwise left empty.
// Always rejects the obvious SSRF pivots (cloud-metadata IPs, link-local IPv4)
// and protocol smuggling; additionally enforces the per-provider host pin +
// insecure-http consent carried in `policy`.
EndpointVerdict SanitizeAiEndpointUrl(const std::string& url, const EndpointPolicy& policy, std::string& out_url);

// Legacy permissive overload (no host pin, http to any host allowed) — kept for
// callers / tests without a provider policy. Equivalent to a default EndpointPolicy.
EndpointVerdict SanitizeAiEndpointUrl(const std::string& url, std::string& out_url);

// Human-readable description for the verdict, used in error log lines and the
// UI error strip when the controller falls back to "no endpoint" mode.
const char* EndpointVerdictDescription(EndpointVerdict v);

} // namespace pure
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_ENDPOINT_SANITIZE_H
