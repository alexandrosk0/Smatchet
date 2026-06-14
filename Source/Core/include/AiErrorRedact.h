// AiErrorRedact.h — sanitise AI-provider HTTP error bodies before they reach
// user-visible error surfaces (AiStreamError::Message, log lines).
// Provider 401/403 bodies have been observed echoing the Authorization header
// verbatim, the request body, and org / project identifiers. Defense-in-depth
// against that class of leak.
// Lives in a sibling TU (AiErrorRedact.cpp) — no cpr / cpp-httplib / SQLite
// includes — so the doctest rig can link it without dragging the HTTP layer.
// Instance of the Pure-helper TU-split recipe (AGENTS.md § Orchestrator
// delegation packet).

#ifndef SMATCHET_AI_ERROR_REDACT_H
#define SMATCHET_AI_ERROR_REDACT_H

#include <cstddef>
#include <string>

namespace smatchet {
namespace ai {
namespace pure {

// Maximum chars of provider error body surfaced to the user via AiStreamError
// after sanitisation. Provider 4xx bodies routinely echo the request body or
// org / project identifiers; the user only needs the human-readable message.
constexpr std::size_t kMaxProviderErrorBodyChars = 240;

// Redirect policy for every AI-provider cpr request that carries a provider key
// (OpenAI/Whisper Bearer, Anthropic x-api-key): never follow redirects, so a
// cross-host 30x can't forward the key (a caller-set RAW header that curl's
// CURLOPT_UNRESTRICTED_AUTH=0 default does NOT strip) to an attacker/MITM host
// — security synthesis #11. Mirrors tracker MakeTrackerRedirectPolicy(); the AI
// REST/SSE verbs respond directly with 2xx/4xx and never depend on a 30x. A pure
// bool (no cpr type) so the cpr-free doctest rig can pin it; each client builds
// cpr::Redirect{kAiFollowRedirects, false}.
constexpr bool kAiFollowRedirects = false;

// Strip secrets / identifiers from a provider error body before it is surfaced
// to UI. Heuristics covered:
//   * Bearer <token>            -> Bearer [REDACTED]
//   * "api_key" / "apiKey" / "Authorization" / "authorization" JSON values
//   * sk-... / sk_... / org-... / proj_... / asst_... id prefixes
// Length-capped to kMaxProviderErrorBodyChars after sanitisation (a "…" suffix
// indicates truncation).
std::string RedactProviderErrorBody(const std::string& body);

} // namespace pure
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_ERROR_REDACT_H
