#pragma once

// WhisperApiClient — OpenAI /v1/audio/transcriptions REST client. Cloud path
// for the Whisper dictation plugin; complement to the local whisper.cpp client
// landing in Phase C.
// Threading contract: Transcribe() is synchronous and BLOCKS for the duration
// of the HTTP round-trip (typically 1-3 s on a 5-second audio clip). It must
// NOT be called from any function reachable from `ImGui::*`-frame stack — that
// would violate UX Pillar 2 (UI never freezes). The Phase B CLI command
// (whisper.transcribe-once) is fine because CLI commands run on a non-UI
// thread; the Phase E hotkey path wraps Transcribe in
// `AppController::LaunchBackgroundTask` and posts the result back via
// `MainThreadDispatcher::PostToMainThread` (Pattern A, mirror of PR #186 /
// #191). Documented here so future callers don't relearn the rule.
// Pure-helper split: response-body parsing (nlohmann::json -> text) is its
// own static function so the doctest rig can validate the parse without
// linking cpr. Only Transcribe() itself reaches the network.

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace whisper {

class WhisperApiClient {
  public:
    WhisperApiClient();
    ~WhisperApiClient();

    WhisperApiClient(const WhisperApiClient&) = delete;
    WhisperApiClient& operator=(const WhisperApiClient&) = delete;

    /// Test-only seam — when set, every subsequent `Transcribe` call returns
    /// the canned text in `outText` and skips the HTTP request entirely. The
    /// higher-level `WhisperPlugin::SetMockTranscription` is preferred for
    /// scenario tests because it bypasses both the WASAPI capture loop and the
    /// transcription backend; this lower-level seam exists for tests that need
    /// to exercise the API-client code path itself (mode-router branch,
    /// caller-side error mapping, fixture-driven golden checks) without
    /// burning real OpenAI credits. Process-wide; thread-safe (mutex-guarded
    /// copy). Production code never sets this.
    static void SetMockResponse(const std::string& text);
    static void ClearMockResponse();
    static bool MockResponseActive();

    /// POST the WAV bytes to OpenAI's transcription endpoint and return the
    /// recognised text in `outText`. `apiKey` is the resolved Bearer token
    /// (caller invokes WhisperApiKeyResolve::ResolveWhisperApiKey first).
    /// Returns true on a 2xx response with a parseable body. Returns false on
    /// transport error / non-2xx HTTP / unparseable JSON; `outError` carries a
    /// human-readable summary (provider error bodies are redacted via
    /// `AiErrorRedact::RedactProviderErrorBody` before they reach `outError`).
    /// `wavBytes` must be a complete RIFF/WAVE buffer (see WavWriter).
    /// Suggested input format: 16 kHz mono 16-bit PCM, < 25 MB (OpenAI's
    /// documented file-size cap).
    bool Transcribe(const std::vector<std::uint8_t>& wavBytes,
                    const std::string& apiKey,
                    std::string& outText,
                    std::string& outError);

    /// Phase F language-aware overload. `language` is a two-letter ISO code
    /// (e.g. "en", "fr") forwarded to OpenAI's multipart `language` field;
    /// "auto" / empty omit the field so Whisper autodetects. All other
    /// semantics match the no-arg `Transcribe` above.
    bool Transcribe(const std::vector<std::uint8_t>& wavBytes,
                    const std::string& apiKey,
                    const std::string& language,
                    std::string& outText,
                    std::string& outError);
};

namespace pure {

// Parse OpenAI's `/v1/audio/transcriptions` JSON response into the recognised
// text. The documented shape is `{"text": "..."}` for `response_format=json`;
// `verbose_json` adds segments / language fields which we ignore. Returns
// false when the JSON is malformed or missing the `text` field.
// Pure helper — exposed for the doctest rig.
bool ParseWhisperResponse(const std::string& jsonBody,
                          std::string& outText,
                          std::string& outError);

// Endpoint used by the cloud client. Exposed as a string constant so the
// doctest rig can assert it without linking cpr.
const char* CloudEndpointUrl();

} // namespace pure

} // namespace whisper
} // namespace smatchet
