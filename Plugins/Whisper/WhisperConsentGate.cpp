// WhisperConsentGate — see header for design pointers. Pure logic, zero
// state, zero I/O beyond the steady_clock query helper. Tests drive the
// three predicates with synthetic configs + a synthetic clock to lock the
// four consent invariants from docs/plans/shipped/whisper-dictation.md.

#include "WhisperConsentGate.h"

#include "ConfigManager.h"

#include <chrono>
#include <cstdint>
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
} // namespace whisper
} // namespace smatchet
