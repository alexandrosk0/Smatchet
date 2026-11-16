// WhisperConsentGate — see header for design pointers. Pure logic, zero
// state, zero I/O beyond the steady_clock query helper. Tests drive the
// three predicates with synthetic configs + a synthetic clock to lock the
// four consent invariants from docs/plans/shipped/whisper-dictation.md.

#include "WhisperConsentGate.h"

#include "ConfigManager.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace smatchet {
namespace whisper {
namespace consent {

bool CanCaptureMic(const TrackerConfig& cfg) {
#if defined(SMATCHET_WITH_WHISPER)
    if (!cfg.WhisperEnabled) {
        return false;
    }
    if (!cfg.WhisperSetupCompleted) {
        return false;
    }
    return true;
#else
    (void)cfg;
    return false;
#endif
}

bool CanDownloadModel(const TrackerConfig& cfg, std::int64_t nowEpochSec, int freshConsentWithinSec) {
#if defined(SMATCHET_WITH_WHISPER)
    if (!cfg.WhisperEnabled) {
        return false;
    }
    if (nowEpochSec <= 0) {
        return false; // caller failed to source a wall clock — default-deny
    }
    // WhisperConsentTimestampSec is the moment the user actively clicked the
    // "Download + enable" / "Download" button. The freshness window is the
    // packet's 30 s default; a longer window would dilute the "no silent
    // re-downloads" invariant.
    if (cfg.WhisperConsentTimestampSec <= 0) {
        return false;
    }
    if (freshConsentWithinSec < 0) {
        return false;
    }
    const std::int64_t ageSec = nowEpochSec - cfg.WhisperConsentTimestampSec;
    if (ageSec < 0) {
        // Clock went backwards (e.g. NTP correction). Refuse rather than
        // unbounded-allow; the user can re-click to refresh the stamp.
        return false;
    }
    if (ageSec > static_cast<std::int64_t>(freshConsentWithinSec)) {
        return false;
    }
    return true;
#else
    (void)cfg;
    (void)nowEpochSec;
    (void)freshConsentWithinSec;
    return false;
#endif
}

bool CanCallCloudApi(const TrackerConfig& cfg, const std::string& resolvedKey) {
#if defined(SMATCHET_WITH_WHISPER)
    if (!cfg.WhisperEnabled) {
        return false;
    }
    if (!cfg.WhisperSetupCompleted) {
        return false;
    }
    if (resolvedKey.empty()) {
        return false;
    }
    return true;
#else
    (void)cfg;
    (void)resolvedKey;
    return false;
#endif
}

std::int64_t NowEpochSec() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

} // namespace consent

namespace feedback {

namespace {

bool StartsWith(const std::string& s, const char* prefix) { return s.compare(0, std::strlen(prefix), prefix) == 0; }

template <std::size_t N> bool StartsWithAny(const std::string& s, const char* const (&prefixes)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (StartsWith(s, prefixes[i])) {
            return true;
        }
    }
    return false;
}

// "HTTP <status>" or "HTTP <status>: <body>" gives the status. 0 when the text is not an HTTP
// error; -1 when it is one with no readable status.
int HttpStatusOf(const std::string& err) {
    static const char kPrefix[] = "HTTP ";
    if (!StartsWith(err, kPrefix)) {
        return 0;
    }
    int status = 0;
    int digits = 0;
    for (std::size_t i = sizeof(kPrefix) - 1; i < err.size() && digits < 6; ++i, ++digits) {
        if (err[i] < '0' || err[i] > '9') {
            break;
        }
        status = status * 10 + (err[i] - '0');
    }
    return digits > 0 ? status : -1;
}

} // namespace

DictationFailureCopy CopyFor(DictationFailure kind) {
    // The flag leads each row. With it last, the duplication gate takes the rows for a copy of the
    // preferences schema's tables.
    static const DictationFailureCopy kCopy[] = {
        {true, "whisper.fail.consent_off.title", "Dictation is off", "whisper.fail.consent_off.body",
         "Turn on \"Enable voice dictation\" in Preferences > AI & Voice. If it is already on, finish the "
         "dictation setup there."},
        {true, "whisper.fail.capture.title", "Microphone unavailable", "whisper.fail.capture.body",
         "Smatchet could not open the microphone. Check that one is connected and that Windows lets apps "
         "use it (Settings > Privacy > Microphone)."},
        {false, "whisper.fail.too_short.title", "Too short to transcribe", "whisper.fail.too_short.body",
         "Hold the dictation key while you speak, then release it."},
        {false, "whisper.fail.busy.title", "Still transcribing", "whisper.fail.busy.body",
         "The previous clip is still being transcribed, so this one was dropped. Try again when it finishes."},
        {false, "whisper.fail.no_speech.title", "Didn't catch that", "whisper.fail.no_speech.body",
         "No speech was heard. Speak closer to the microphone and try again."},
        {true, "whisper.fail.no_key.title", "No speech API key", "whisper.fail.no_key.body",
         "Add an OpenAI API key in Preferences > AI & Voice, or install a local model."},
        {true, "whisper.fail.key_rejected.title", "API key rejected", "whisper.fail.key_rejected.body",
         "The speech service rejected the API key. Check it in Preferences > AI & Voice."},
        {true, "whisper.fail.rate_limited.title", "Speech service busy", "whisper.fail.rate_limited.body",
         "The rate limit was reached. Wait a moment and try again."},
        {true, "whisper.fail.network.title", "Can't reach the speech service", "whisper.fail.network.body",
         "Check your connection, or install a local model to dictate offline."},
        {true, "whisper.fail.service.title", "Transcription failed", "whisper.fail.service.body",
         "The speech service returned an error. Try again; the log has the details."},
        {true, "whisper.fail.local_model.title", "Local model unavailable", "whisper.fail.local_model.body",
         "The local Whisper model could not be loaded. Download it again in Preferences > AI & Voice, or "
         "switch the mode to Cloud."},
        {true, "whisper.fail.other.title", "Dictation failed", "whisper.fail.other.body",
         "Dictation could not finish. The log has the details."},
    };
    static_assert(sizeof(kCopy) / sizeof(kCopy[0]) == static_cast<std::size_t>(DictationFailure::Count),
                  "one notice per DictationFailure");
    const int idx = static_cast<int>(kind);
    if (idx < 0 || idx >= static_cast<int>(DictationFailure::Count)) {
        return kCopy[static_cast<int>(DictationFailure::Other)];
    }
    return kCopy[idx];
}

DictationFailure ClassifyTranscriptionError(const std::string& err) {
    static const char* const kService[] = {"empty response body", "malformed JSON response",
                                           "response is not a JSON object", "response JSON missing"};
    static const char* const kLocalModel[] = {"local model load failed", "local backend not built",
                                              "model not loaded",        "model path is empty",
                                              "model file not found",    "whisper.cpp init returned null"};
    static const char* const kNoSpeech[] = {"no audio captured", "audio buffer empty after trim",
                                            "WAV payload is empty", "audio buffer is empty", "empty PCM payload"};
    if (StartsWith(err, "consent required")) {
        return DictationFailure::ConsentOff;
    }
    if (StartsWith(err, "no API key available")) {
        return DictationFailure::NoApiKey;
    }
    if (StartsWith(err, "transport: ")) {
        return DictationFailure::Network;
    }
    const int http = HttpStatusOf(err);
    if (http == 401 || http == 403) {
        return DictationFailure::ApiKeyRejected;
    }
    if (http == 429) {
        return DictationFailure::RateLimited;
    }
    if (http != 0 || StartsWithAny(err, kService)) {
        return DictationFailure::ServiceError;
    }
    if (StartsWithAny(err, kLocalModel)) {
        return DictationFailure::LocalModelUnavailable;
    }
    if (StartsWithAny(err, kNoSpeech)) {
        return DictationFailure::NoSpeech;
    }
    return DictationFailure::Other;
}

bool FailureToastDue(std::int64_t lastShownMs, std::int64_t nowMs, std::int64_t cooldownMs) {
    return lastShownMs == 0 || nowMs - lastShownMs >= cooldownMs;
}

bool ConsentOffNoticeDue(bool dictationEnabled, bool appInForeground) { return dictationEnabled || appInForeground; }

} // namespace feedback
} // namespace whisper
} // namespace smatchet
