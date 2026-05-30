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
    RejectedBadScheme,     // not http:// or https://
    RejectedControlChars,  // CR / LF / NUL embedded (header smuggling)
    RejectedCloudMetadata, // 169.254.169.254 / 100.100.100.200 etc.
    RejectedLinkLocal,     // 169.254.0.0/16 (link-local IPv4)
    RejectedMalformed,     // missing host, empty after scheme, etc.
};

// Validate a configured AI endpoint URL. `out_url` is populated with the
// normalised URL when the verdict is Allowed; otherwise left empty. The
// validator is intentionally narrow — it rejects the obvious SSRF pivots
// (cloud metadata IPs, link-local IPv4) and protocol smuggling, but does NOT
// allow-list per-provider hosts (so legitimate proxies like LiteLLM, Azure
// OpenAI, openrouter, and self-hosted Ollama-compat shims still work).
EndpointVerdict SanitizeAiEndpointUrl(const std::string& url, std::string& out_url);

// Human-readable description for the verdict, used in error log lines and the
// UI error strip when the controller falls back to "no endpoint" mode.
const char* EndpointVerdictDescription(EndpointVerdict v);

} // namespace pure
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_ENDPOINT_SANITIZE_H
