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
} // namespace whisper
} // namespace smatchet
