// WhisperApiClient — OpenAI /v1/audio/transcriptions REST client. See header
// for the threading contract.

#include "WhisperApiClient.h"

#include "AiErrorRedact.h"
#include "Logger.h"
#include "NetworkUsageTracker.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace smatchet {
namespace whisper {

namespace {

// Mock seam — see WhisperApiClient.h for the contract. Process-wide singleton
// gated on an atomic so the fast-path read in Transcribe is lock-free when no
// mock is installed.
std::mutex& MockMutex() {
    static std::mutex m;
    return m;
}
std::atomic<bool>& MockActiveAtom() {
    static std::atomic<bool> a{false};
    return a;
}
std::string& MockTextStorage() {
    static std::string s;
    return s;
}

} // namespace

void WhisperApiClient::SetMockResponse(const std::string& text) {
    std::lock_guard<std::mutex> lk(MockMutex());
    MockTextStorage() = text;
    MockActiveAtom().store(true, std::memory_order_release);
    LOG_INFO("WhisperApiClient: mock response armed (%zu bytes)", text.size());
}

void WhisperApiClient::ClearMockResponse() {
    std::lock_guard<std::mutex> lk(MockMutex());
    MockActiveAtom().store(false, std::memory_order_release);
    MockTextStorage().clear();
    LOG_INFO("WhisperApiClient: mock response cleared");
}

bool WhisperApiClient::MockResponseActive() {
    return MockActiveAtom().load(std::memory_order_acquire);
}

namespace pure {

const char* CloudEndpointUrl() {
    return "https://api.openai.com/v1/audio/transcriptions";
}

bool ParseWhisperResponse(const std::string& jsonBody,
                          std::string& outText,
                          std::string& outError) {
    outText.clear();
    outError.clear();
    if (jsonBody.empty()) {
        outError = "empty response body";
        return false;
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(jsonBody);
    } catch (const std::exception& e) {
        outError = std::string("malformed JSON response: ") + e.what();
        return false;
    }
    if (!parsed.is_object()) {
        outError = "response is not a JSON object";
        return false;
    }
    // The documented `response_format=json` shape is `{"text": "..."}`; the
    // verbose-json variant also carries a top-level `text`. We accept any
    // object whose `text` field is a string.
    auto it = parsed.find("text");
    if (it == parsed.end() || !it->is_string()) {
        outError = "response JSON missing string 'text' field";
        return false;
    }
    outText = it->get<std::string>();
    return true;
}

} // namespace pure

WhisperApiClient::WhisperApiClient() = default;
WhisperApiClient::~WhisperApiClient() = default;

bool WhisperApiClient::Transcribe(const std::vector<std::uint8_t>& wavBytes,
                                  const std::string& apiKey,
                                  std::string& outText,
                                  std::string& outError) {
    // Legacy entry point — no language hint, server-side autodetect.
    return Transcribe(wavBytes, apiKey, std::string(), outText, outError);
}

bool WhisperApiClient::Transcribe(const std::vector<std::uint8_t>& wavBytes,
                                  const std::string& apiKey,
                                  const std::string& language,
                                  std::string& outText,
                                  std::string& outError) {
    outText.clear();
    outError.clear();

    // Mock seam — short-circuit before any input validation so tests can pin
    // the response without supplying a non-empty WAV or a credential. See
    // header for the test-only contract.
    if (MockActiveAtom().load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(MockMutex());
        outText = MockTextStorage();
        LOG_DEBUG("WhisperApiClient: returning mock response (%zu bytes, language='%s')",
                  outText.size(), language.c_str());
        return true;
    }

    if (apiKey.empty()) {
        outError = "no API key available - set WhisperApiKey or AiApiKey (provider=openai)";
        return false;
    }
    if (wavBytes.empty()) {
        outError = "audio buffer is empty";
        return false;
    }

    const std::string url = pure::CloudEndpointUrl();

    cpr::Header headers{
        {"Authorization", std::string("Bearer ") + apiKey},
    };

    // cpr::Multipart accepts cpr::Buffer for in-memory file uploads. The
    // {begin, end, filename} ctor copies into an internal buffer — wavBytes
    // can safely fall out of scope after this Post returns. Content-Type for
    // the file part is "audio/wav" so Whisper's audio-format auto-detect
    // takes the fast path.
    // Phase F — append a `language` multipart part when the caller supplied a
    // concrete ISO code. "auto" / empty omit the field so the server
    // autodetects from its supported language set.
    const char* wavStart = reinterpret_cast<const char*>(wavBytes.data());
    const char* wavEnd = wavStart + wavBytes.size();
    cpr::Multipart multipart{
        cpr::Part{"file", cpr::Buffer{wavStart, wavEnd, "audio.wav"}, "audio/wav"},
        cpr::Part{"model", "whisper-1"},
        cpr::Part{"response_format", "json"},
    };
    if (!language.empty() && language != "auto") {
        multipart.parts.push_back(cpr::Part{"language", language});
    }

    // Connect timeout: short so a hung DNS / TCP handshake doesn't freeze the
    // CLI for the user. Total timeout: 60s — Whisper API quotes p99 < 30s for
    // a 30-second clip; doubling that gives slack for network jitter.
    cpr::Response r = cpr::Post(cpr::Url{url}, headers, multipart, cpr::ConnectTimeout{5000},
                                cpr::Timeout{60000});

    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Ai,
                                           static_cast<std::uint64_t>(wavBytes.size()), r);

    if (r.error.code != cpr::ErrorCode::OK) {
        LOG_ERROR("WhisperApiClient: transport error - cpr code %d, message: %s",
                  static_cast<int>(r.error.code), r.error.message.c_str());
        outError = std::string("transport: ") + r.error.message;
        return false;
    }

    if (r.status_code < 200 || r.status_code >= 300) {
        // Mirror OpenAiClient's redaction discipline: provider 401/403/429
        // bodies have historically echoed the Authorization header verbatim,
        // request bodies, and org / project identifiers.
        const std::string redactedBody = smatchet::ai::pure::RedactProviderErrorBody(r.text);
        LOG_ERROR("WhisperApiClient: HTTP %ld - body: %s",
                  static_cast<long>(r.status_code), redactedBody.c_str());
        outError = std::string("HTTP ") + std::to_string(r.status_code);
        if (!redactedBody.empty()) {
            outError.append(": ");
            outError.append(redactedBody);
        }
        return false;
    }

    std::string parseError;
    if (!pure::ParseWhisperResponse(r.text, outText, parseError)) {
        LOG_ERROR("WhisperApiClient: parse failure - %s", parseError.c_str());
        outError = parseError;
        return false;
    }
    return true;
}

} // namespace whisper
} // namespace smatchet
