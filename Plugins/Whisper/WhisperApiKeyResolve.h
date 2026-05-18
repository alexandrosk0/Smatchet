#pragma once

// WhisperApiKeyResolve — pure helper that implements the 5-row API-key fallback
// decision table from docs/design/whisper-dictation.md § API key fallback rule.
//
// Pure: no I/O, no globals, no SMATCHET_WITH_WHISPER gate, no third-party deps.
// The TU lives under Plugins/Whisper/ because the rule is owned by the Whisper
// feature, but the function is tested by the doctest rig directly (per the
// pure-helper TU-split recipe in AGENTS.md § Delegation packet).
//
// Inputs are the three relevant fields straight off ConfigManager — the caller
// (WhisperPlugin / WhisperApiClient) is responsible for converting
// `cfg.AiProviderKind` to its canonical lowercase string ("openai" /
// "anthropic" / "ollama-openai" / "ollama-native") before invoking. Pass the
// exact same string the AI Assistant Factory uses (AiClientFactory's
// ProviderToString) — case- and dash-sensitive matching.

#include <string>

namespace smatchet {
namespace whisper {
namespace pure {

// Returns the OpenAI API key Whisper should use for this transcription. Empty
// return means "no key available" — caller should surface a "configure
// Whisper API key" message and refuse to fire the HTTP request.
//
// Decision table (mirrors the plan):
//   1. whisperApiKey set            -> whisperApiKey  (explicit always wins)
//   2. whisperApiKey empty + aiProvider == "openai" + aiApiKey set
//                                   -> aiApiKey       (convenience fallback)
//   3. whisperApiKey empty + aiProvider == "openai" + aiApiKey empty
//                                   -> ""
//   4. whisperApiKey empty + aiProvider == "anthropic" / "ollama-*" / other
//                                   -> ""             (no Whisper there)
std::string ResolveWhisperApiKey(const std::string& whisperApiKey,
                                 const std::string& aiProvider,
                                 const std::string& aiApiKey);

} // namespace pure
} // namespace whisper
} // namespace smatchet
