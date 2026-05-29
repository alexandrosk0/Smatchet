// WhisperConsentGate — Phase C doctest coverage. The three predicates funnel
// the four consent invariants from docs/plans/shipped/whisper-dictation.md, so the
// tests below pin every relevant state transition. Pure logic, no I/O — the
// clock is faked by passing nowEpochSec directly into CanDownloadModel.

#include <doctest/doctest.h>

#if defined(SMATCHET_WITH_WHISPER)

#include "ConfigManager.h"
#include "WhisperConsentGate.h"

#include <string>

namespace gate = smatchet::whisper::consent;

namespace {

TrackerConfig DefaultOff() {
    TrackerConfig c;
    c.WhisperEnabled = false;
    c.WhisperSetupCompleted = false;
    c.WhisperConsentTimestampSec = 0;
    return c;
}

TrackerConfig EnabledAndCompleted() {
    TrackerConfig c;
    c.WhisperEnabled = true;
    c.WhisperSetupCompleted = true;
    c.WhisperConsentTimestampSec = 1700000000; // fixed reference moment
    return c;
}

} // namespace

TEST_CASE("CanCaptureMic: requires both enabled + setup-completed") {
    const TrackerConfig off = DefaultOff();
    CHECK_FALSE(gate::CanCaptureMic(off));

    TrackerConfig partial = off;
    partial.WhisperEnabled = true; // enabled but not setup-completed
    CHECK_FALSE(gate::CanCaptureMic(partial));

    partial.WhisperEnabled = false;
    partial.WhisperSetupCompleted = true; // setup-completed but disabled
    CHECK_FALSE(gate::CanCaptureMic(partial));

    const TrackerConfig on = EnabledAndCompleted();
    CHECK(gate::CanCaptureMic(on));
}

TEST_CASE("CanDownloadModel: requires enabled + fresh consent within window") {
    TrackerConfig c = EnabledAndCompleted();
    const std::int64_t now = c.WhisperConsentTimestampSec; // stamped just now
    const int window = 30;

    SUBCASE("fresh consent passes") {
        CHECK(gate::CanDownloadModel(c, now, window));
        CHECK(gate::CanDownloadModel(c, now + window, window)); // at the boundary
    }

    SUBCASE("stale consent fails") {
        CHECK_FALSE(gate::CanDownloadModel(c, now + window + 1, window));
        CHECK_FALSE(gate::CanDownloadModel(c, now + 3600, window));
    }

    SUBCASE("backwards clock fails (refuse to allow unbounded)") {
        CHECK_FALSE(gate::CanDownloadModel(c, now - 1, window));
    }

    SUBCASE("missing consent timestamp fails") {
        c.WhisperConsentTimestampSec = 0;
        CHECK_FALSE(gate::CanDownloadModel(c, now, window));
    }

    SUBCASE("disabled fails even with fresh stamp") {
        c.WhisperEnabled = false;
        CHECK_FALSE(gate::CanDownloadModel(c, now, window));
    }

    SUBCASE("zero / negative clock fails") {
        CHECK_FALSE(gate::CanDownloadModel(c, 0, window));
        CHECK_FALSE(gate::CanDownloadModel(c, -1, window));
    }

    SUBCASE("negative window fails") {
        CHECK_FALSE(gate::CanDownloadModel(c, now, -1));
    }
}

TEST_CASE("CanCallCloudApi: requires enabled + setup-completed + non-empty resolved key") {
    const TrackerConfig off = DefaultOff();
    CHECK_FALSE(gate::CanCallCloudApi(off, "sk-test"));

    TrackerConfig partial = off;
    partial.WhisperEnabled = true;
    CHECK_FALSE(gate::CanCallCloudApi(partial, "sk-test")); // not setup-completed

    partial.WhisperSetupCompleted = true;
    CHECK_FALSE(gate::CanCallCloudApi(partial, std::string())); // empty key

    CHECK(gate::CanCallCloudApi(partial, "sk-test"));
}

TEST_CASE("NowEpochSec returns a positive value (sanity)") {
    const std::int64_t now = gate::NowEpochSec();
    // 2026 is around 1.78e9 seconds; any positive value in roughly that
    // ballpark satisfies the "wall clock is sane" check.
    CHECK(now > 1500000000);
}

#endif // SMATCHET_WITH_WHISPER
