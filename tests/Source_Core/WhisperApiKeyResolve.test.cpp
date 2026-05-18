// WhisperApiKeyResolve — pure doctest coverage for the 5-row API-key fallback
// decision table from docs/design/whisper-dictation.md § API key fallback rule.
//
// Every row in the plan's table corresponds to a CHECK below. Adding a sixth
// row to the rule (or removing an existing row) must update both the plan
// table and this file in the same commit — that's the drift-prevention shape.

#include <doctest/doctest.h>

#include "WhisperApiKeyResolve.h"

#include <string>

using smatchet::whisper::pure::ResolveWhisperApiKey;

TEST_CASE("WhisperApiKeyResolve: row 1 - explicit WhisperApiKey always wins") {
    CHECK(ResolveWhisperApiKey("sk-whisper", "openai", "sk-ai") == "sk-whisper");
    // Wins even when the AI side disagrees on provider.
    CHECK(ResolveWhisperApiKey("sk-whisper", "anthropic", "sk-ai") == "sk-whisper");
    CHECK(ResolveWhisperApiKey("sk-whisper", "ollama-openai", "") == "sk-whisper");
    CHECK(ResolveWhisperApiKey("sk-whisper", "", "") == "sk-whisper");
}

TEST_CASE("WhisperApiKeyResolve: row 2 - empty Whisper key + openai AI provider falls back") {
    CHECK(ResolveWhisperApiKey("", "openai", "sk-ai") == "sk-ai");
}

TEST_CASE("WhisperApiKeyResolve: row 3 - empty Whisper key + openai + empty AI key returns empty") {
    CHECK(ResolveWhisperApiKey("", "openai", "") == "");
}

TEST_CASE("WhisperApiKeyResolve: row 4 - empty Whisper key + anthropic returns empty") {
    CHECK(ResolveWhisperApiKey("", "anthropic", "sk-anthropic") == "");
    CHECK(ResolveWhisperApiKey("", "anthropic", "") == "");
}

TEST_CASE("WhisperApiKeyResolve: row 5 - ollama / lmstudio / unknown providers return empty") {
    CHECK(ResolveWhisperApiKey("", "ollama-openai", "ignored") == "");
    CHECK(ResolveWhisperApiKey("", "ollama-native", "ignored") == "");
    CHECK(ResolveWhisperApiKey("", "lmstudio", "ignored") == "");
    CHECK(ResolveWhisperApiKey("", "", "ignored") == "");
    CHECK(ResolveWhisperApiKey("", "totally-unknown", "ignored") == "");
}

TEST_CASE("WhisperApiKeyResolve: matching is case-sensitive (openai != OpenAI)") {
    // Caller is responsible for lowercasing — AiClientFactory::ProviderToString
    // already emits lowercase, so a mismatch here means the caller is passing
    // the wrong shape and should not silently get the fallback.
    CHECK(ResolveWhisperApiKey("", "OpenAI", "sk-ai") == "");
    CHECK(ResolveWhisperApiKey("", "OPENAI", "sk-ai") == "");
}
