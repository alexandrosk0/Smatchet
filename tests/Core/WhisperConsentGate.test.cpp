// WhisperConsentGate — Phase C doctest coverage. The three predicates funnel
// the four consent invariants from docs/plans/shipped/whisper-dictation.md, so the
// tests below pin every relevant state transition. Pure logic, no I/O — the
// clock is faked by passing nowEpochSec directly into CanDownloadModel.

#include <doctest/doctest.h>

#if defined(SMATCHET_WITH_WHISPER)

#include "ConfigManager.h"
#include "WhisperConsentGate.h"

#include <algorithm>
#include <string>
#include <vector>

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

    SUBCASE("negative window fails") { CHECK_FALSE(gate::CanDownloadModel(c, now, -1)); }
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

namespace fb = smatchet::whisper::feedback;

TEST_CASE("ClassifyTranscriptionError: consent, key and transport") {
    CHECK(fb::ClassifyTranscriptionError("consent required: enable Whisper dictation in Preferences first") ==
          fb::DictationFailure::ConsentOff);
    CHECK(fb::ClassifyTranscriptionError("no API key available - set WhisperApiKey or AiApiKey (provider=openai)") ==
          fb::DictationFailure::NoApiKey);
    CHECK(fb::ClassifyTranscriptionError("transport: Could not resolve host") == fb::DictationFailure::Network);
}

TEST_CASE("ClassifyTranscriptionError: HTTP status decides the bucket") {
    CHECK(fb::ClassifyTranscriptionError("HTTP 401") == fb::DictationFailure::ApiKeyRejected);
    CHECK(fb::ClassifyTranscriptionError("HTTP 403: {\"error\":\"forbidden\"}") ==
          fb::DictationFailure::ApiKeyRejected);
    CHECK(fb::ClassifyTranscriptionError("HTTP 429") == fb::DictationFailure::RateLimited);
    CHECK(fb::ClassifyTranscriptionError("HTTP 429: slow down") == fb::DictationFailure::RateLimited);
    CHECK(fb::ClassifyTranscriptionError("HTTP 500: oops") == fb::DictationFailure::ServiceError);
    // The whole number is read, so a longer status is not taken for 429 or 401.
    CHECK(fb::ClassifyTranscriptionError("HTTP 4290") == fb::DictationFailure::ServiceError);
    CHECK(fb::ClassifyTranscriptionError("HTTP 4011") == fb::DictationFailure::ServiceError);
    CHECK(fb::ClassifyTranscriptionError("HTTP ") == fb::DictationFailure::ServiceError);
}

TEST_CASE("ClassifyTranscriptionError: a reply that cannot be read is a service error") {
    CHECK(fb::ClassifyTranscriptionError("malformed JSON response: x") == fb::DictationFailure::ServiceError);
    CHECK(fb::ClassifyTranscriptionError("response is not a JSON object") == fb::DictationFailure::ServiceError);
    CHECK(fb::ClassifyTranscriptionError("response JSON missing string 'text' field") ==
          fb::DictationFailure::ServiceError);
    CHECK(fb::ClassifyTranscriptionError("empty response body") == fb::DictationFailure::ServiceError);
}

TEST_CASE("ClassifyTranscriptionError: local model failures") {
    CHECK(fb::ClassifyTranscriptionError("local model load failed: model file not found or unreadable: C:/m.bin") ==
          fb::DictationFailure::LocalModelUnavailable);
    CHECK(fb::ClassifyTranscriptionError("local backend not built (set SMATCHET_WITH_WHISPER_LOCAL)") ==
          fb::DictationFailure::LocalModelUnavailable);
    CHECK(fb::ClassifyTranscriptionError("model not loaded") == fb::DictationFailure::LocalModelUnavailable);
    CHECK(fb::ClassifyTranscriptionError("model path is empty") == fb::DictationFailure::LocalModelUnavailable);
    CHECK(fb::ClassifyTranscriptionError("model file not found or unreadable: C:/m.bin") ==
          fb::DictationFailure::LocalModelUnavailable);
    CHECK(fb::ClassifyTranscriptionError("whisper.cpp init returned null for model: C:/m.bin") ==
          fb::DictationFailure::LocalModelUnavailable);
}

TEST_CASE("ClassifyTranscriptionError: a clip with nothing in it is NoSpeech") {
    CHECK(fb::ClassifyTranscriptionError("audio buffer empty after trim (all silence)") ==
          fb::DictationFailure::NoSpeech);
    CHECK(fb::ClassifyTranscriptionError("no audio captured (0 samples)") == fb::DictationFailure::NoSpeech);
    CHECK(fb::ClassifyTranscriptionError("WAV payload is empty (encode failed)") == fb::DictationFailure::NoSpeech);
    CHECK(fb::ClassifyTranscriptionError("audio buffer is empty") == fb::DictationFailure::NoSpeech);
    CHECK(fb::ClassifyTranscriptionError("empty PCM payload") == fb::DictationFailure::NoSpeech);
}

TEST_CASE("ClassifyTranscriptionError: unknown and empty text fall back to Other") {
    CHECK(fb::ClassifyTranscriptionError("whisper_full failed (rc=-1)") == fb::DictationFailure::Other);
    CHECK(fb::ClassifyTranscriptionError("") == fb::DictationFailure::Other);
    CHECK(fb::ClassifyTranscriptionError("HTTPS proxy said no") == fb::DictationFailure::Other);
}

TEST_CASE("CopyFor: every kind has its own keys and non-empty text") {
    std::vector<std::string> titleKeys;
    std::vector<std::string> bodyKeys;
    for (int i = 0; i < static_cast<int>(fb::DictationFailure::Count); ++i) {
        const fb::DictationFailureCopy c = fb::CopyFor(static_cast<fb::DictationFailure>(i));
        REQUIRE(c.titleKey != nullptr);
        REQUIRE(c.titleFallback != nullptr);
        REQUIRE(c.bodyKey != nullptr);
        REQUIRE(c.bodyFallback != nullptr);
        CHECK(std::string(c.titleKey).compare(0, 13, "whisper.fail.") == 0);
        CHECK(std::string(c.bodyKey).compare(0, 13, "whisper.fail.") == 0);
        CHECK_FALSE(std::string(c.titleFallback).empty());
        CHECK_FALSE(std::string(c.bodyFallback).empty());
        titleKeys.push_back(c.titleKey);
        bodyKeys.push_back(c.bodyKey);
    }
    std::sort(titleKeys.begin(), titleKeys.end());
    std::sort(bodyKeys.begin(), bodyKeys.end());
    CHECK(std::adjacent_find(titleKeys.begin(), titleKeys.end()) == titleKeys.end());
    CHECK(std::adjacent_find(bodyKeys.begin(), bodyKeys.end()) == bodyKeys.end());
}

TEST_CASE("CopyFor: the text names the fix, and routine misses are not warnings") {
    const std::string consentBody = fb::CopyFor(fb::DictationFailure::ConsentOff).bodyFallback;
    CHECK(consentBody.find("Preferences > AI & Voice") != std::string::npos);
    CHECK(consentBody.find("setup") != std::string::npos);
    const std::string captureBody = fb::CopyFor(fb::DictationFailure::CaptureStartFailed).bodyFallback;
    CHECK(captureBody.find("microphone") != std::string::npos);
    CHECK_FALSE(fb::CopyFor(fb::DictationFailure::Busy).warning);
    CHECK_FALSE(fb::CopyFor(fb::DictationFailure::NoSpeech).warning);
    CHECK_FALSE(fb::CopyFor(fb::DictationFailure::TooShort).warning);
    CHECK(fb::CopyFor(fb::DictationFailure::ConsentOff).warning);
    CHECK(fb::CopyFor(fb::DictationFailure::Network).warning);
}

TEST_CASE("CopyFor: an out-of-range kind gets the generic notice") {
    const fb::DictationFailureCopy other = fb::CopyFor(fb::DictationFailure::Other);
    CHECK(std::string(fb::CopyFor(fb::DictationFailure::Count).titleKey) == other.titleKey);
    CHECK(std::string(fb::CopyFor(static_cast<fb::DictationFailure>(-1)).titleKey) == other.titleKey);
}

TEST_CASE("FailureToastDue: first show, inside the cooldown, after it") {
    CHECK(fb::FailureToastDue(0, 1000, 4000));
    CHECK_FALSE(fb::FailureToastDue(1000, 4999, 4000));
    CHECK(fb::FailureToastDue(1000, 5000, 4000));
}

TEST_CASE("ConsentOffNoticeDue: silent only for a press elsewhere with dictation never turned on") {
    CHECK_FALSE(fb::ConsentOffNoticeDue(/*dictationEnabled=*/false, /*appInForeground=*/false));
    CHECK(fb::ConsentOffNoticeDue(false, true));
    CHECK(fb::ConsentOffNoticeDue(true, false));
    CHECK(fb::ConsentOffNoticeDue(true, true));
}

#endif // SMATCHET_WITH_WHISPER
