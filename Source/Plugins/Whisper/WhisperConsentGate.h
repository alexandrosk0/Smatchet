#pragma once

// WhisperConsentGate — single pinch point for the four consent invariants in
// docs/plans/shipped/whisper-dictation.md § Optional + no-data-without-consent.
// Centralising the rule means future call sites (Phase D / E surfaces, ad-hoc
// scripts, scenario harnesses) can not accidentally drift around it: every
// mic-access / model-download / cloud-API site funnels through one of the
// three predicates here. The fourth invariant ("no bundled model files
// shipped with the exe") is a build-hygiene rule enforced by
// scripts/dev/test-whisper-consent-gate.sh, not a runtime check.
// Pure logic — no global state, no filesystem touches. Inputs are a value
// snapshot of TrackerConfig plus (for the download predicate) a freshness
// window in seconds. The downstream caller is responsible for sourcing the
// current wall clock.

#include <cstdint>
#include <string>

struct TrackerConfig;

namespace smatchet {
namespace whisper {
namespace consent {

/// Predicate read by WindowsAudioCapture before any WASAPI call is made.
/// Returns true iff:
///   - cfg.WhisperEnabled is true, AND
///   - cfg.WhisperSetupCompleted is true (the user has actively chosen
///     "enable", not just "decide later").
/// The setup banner sets both flags together when the user clicks
/// "Download + enable"; Preferences flips them independently for the user
/// who toggles Whisper back on after disabling.
bool CanCaptureMic(const TrackerConfig& cfg);

/// Predicate read by ModelDownloader::Start before any network fetch is
/// issued. Returns true iff:
///   - cfg.WhisperEnabled is true (or about to be — the banner clicks
///     "Download + enable" first set Enabled=true, then call Start), AND
///   - the explicit-consent timestamp `cfg.WhisperConsentTimestampSec`
///     was set within the last `freshConsentWithinSec` seconds, AND
///   - `nowEpochSec` is non-zero (a zero clock means the caller failed to
///     obtain a wall clock; default-deny rather than allow a download).
/// The freshness window enforces "no silent re-downloads, no resume-after-
/// restart without re-confirming" from the consent invariants.
bool CanDownloadModel(const TrackerConfig& cfg, std::int64_t nowEpochSec, int freshConsentWithinSec);

/// Predicate read by WhisperApiClient::Transcribe before any cloud HTTP
/// POST. Returns true iff:
///   - cfg.WhisperEnabled is true, AND
///   - cfg.WhisperSetupCompleted is true, AND
///   - `resolvedKey` is non-empty (the 5-row key resolver decided the call
///     has an API key to send; empty means the gate would have produced a
///     toast instead).
bool CanCallCloudApi(const TrackerConfig& cfg, const std::string& resolvedKey);

/// Convenience wrapper that returns the current unix epoch seconds via
/// std::chrono::system_clock. Pulled into a helper so tests can fake the
/// clock by passing their own value into CanDownloadModel.
std::int64_t NowEpochSec();

} // namespace consent

namespace feedback {

/// Why a push-to-talk attempt ended without putting text anywhere. Also an index into the
/// per-kind tables, so keep `Count` last.
enum class DictationFailure : int {
    ConsentOff = 0,        // dictation not enabled, or its setup not finished
    CaptureStartFailed,    // the microphone could not be opened
    TooShort,              // released before any audio was captured
    Busy,                  // the previous clip is still being transcribed
    NoSpeech,              // nothing but silence in the clip
    NoApiKey,              // cloud mode with no key
    ApiKeyRejected,        // HTTP 401 / 403
    RateLimited,           // HTTP 429
    Network,               // the speech service could not be reached
    ServiceError,          // any other HTTP status, or a reply that could not be read
    LocalModelUnavailable, // the local model is missing or could not be loaded
    Other,
    Count
};

/// Localization keys and English fallbacks for one failure. String literals, static lifetime.
struct DictationFailureCopy {
    bool warning; // true: a warning the user has to act on; false: a short notice
    const char* titleKey;
    const char* titleFallback;
    const char* bodyKey;
    const char* bodyFallback;
};

/// The notice for a failure. The body names what to do next and never carries raw error text.
DictationFailureCopy CopyFor(DictationFailure kind);

/// Maps the error text of a failed transcription (the pipeline, the cloud client or the local
/// model) to a kind. Matches the leading words those producers write; text it does not know is
/// `Other`, never silence.
DictationFailure ClassifyTranscriptionError(const std::string& err);

/// True when a notice of one kind may show now. `lastShownMs == 0` means it has not shown yet.
bool FailureToastDue(std::int64_t lastShownMs, std::int64_t nowMs, std::int64_t cooldownMs);

/// True when a press that the consent gate turned down should say so. The push-to-talk key is
/// heard in every application. With dictation never turned on, a press made in another
/// application is that application's own shortcut and gets no notice; a press made in this one
/// does. With dictation turned on but its setup unfinished, every press does.
bool ConsentOffNoticeDue(bool dictationEnabled, bool appInForeground);

} // namespace feedback
} // namespace whisper
} // namespace smatchet
