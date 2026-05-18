// SilenceTrim — Phase F pure-helper doctest coverage. The helper has two
// surfaces: TrimLeadingTrailingSilence (peak-relative amplitude gate over
// 100 ms windows) and CapClipSamples (truncate-to-N).

#include <doctest/doctest.h>

#if defined(SMATCHET_WITH_WHISPER)

#include "SilenceTrim.h"

#include <cstdint>
#include <vector>

namespace pure = smatchet::whisper::pure;

namespace {

constexpr int kSampleRate = 16000;

// Build a buffer: `silenceLeadingMs` ms of zeros, then `signalMs` ms of a
// constant amplitude signal, then `silenceTrailingMs` ms of zeros. Sample
// rate is 16 kHz mono.
std::vector<std::int16_t> BuildClip(int silenceLeadingMs, int signalMs,
                                    int silenceTrailingMs, std::int16_t signalAmp) {
    auto msToSamples = [](int ms) {
        return static_cast<std::size_t>((static_cast<long long>(kSampleRate) * ms) / 1000);
    };
    const std::size_t lead = msToSamples(silenceLeadingMs);
    const std::size_t sig = msToSamples(signalMs);
    const std::size_t tail = msToSamples(silenceTrailingMs);
    std::vector<std::int16_t> out;
    out.reserve(lead + sig + tail);
    out.insert(out.end(), lead, std::int16_t{0});
    out.insert(out.end(), sig, signalAmp);
    out.insert(out.end(), tail, std::int16_t{0});
    return out;
}

} // namespace

TEST_CASE("TrimLeadingTrailingSilence drops empty input safely") {
    std::vector<std::int16_t> empty;
    const auto out = pure::TrimLeadingTrailingSilence(empty, kSampleRate);
    CHECK(out.empty());
}

TEST_CASE("TrimLeadingTrailingSilence drops pure-silence input") {
    std::vector<std::int16_t> silence(kSampleRate, std::int16_t{0}); // 1 s of zeros
    const auto out = pure::TrimLeadingTrailingSilence(silence, kSampleRate);
    CHECK(out.empty());
}

TEST_CASE("TrimLeadingTrailingSilence preserves a clip with no silence") {
    // 500 ms of constant amplitude, no surrounding zeros.
    auto clip = BuildClip(0, 500, 0, 10000);
    const auto out = pure::TrimLeadingTrailingSilence(clip, kSampleRate);
    // No leading/trailing silence -> output matches input length within one
    // window's worth of rounding (we don't guarantee exact equality because
    // an internal lead window might align differently with the boundary).
    CHECK(out.size() <= clip.size());
    CHECK(out.size() >= clip.size() - static_cast<std::size_t>(kSampleRate / 10));
}

TEST_CASE("TrimLeadingTrailingSilence strips leading and trailing zeros") {
    // 300 ms silence + 500 ms signal + 300 ms silence.
    auto clip = BuildClip(300, 500, 300, 10000);
    const std::size_t before = clip.size();
    const auto out = pure::TrimLeadingTrailingSilence(clip, kSampleRate);
    CHECK(out.size() < before);
    // The signal section is 500 ms = 8000 samples. The trimmed output
    // should be at least that large (allowing for window-boundary rounding).
    CHECK(out.size() >= static_cast<std::size_t>(kSampleRate / 2)); // >= 500 ms
    // And at most input size — we never grow.
    CHECK(out.size() <= before);
}

TEST_CASE("TrimLeadingTrailingSilence ignores zero / negative sample rate") {
    auto clip = BuildClip(0, 100, 0, 5000);
    CHECK(pure::TrimLeadingTrailingSilence(clip, 0).empty());
    CHECK(pure::TrimLeadingTrailingSilence(clip, -1).empty());
}

TEST_CASE("CapClipSamples — zero cap disables truncation") {
    std::vector<std::int16_t> clip(1000, std::int16_t{42});
    const auto out = pure::CapClipSamples(clip, 0);
    CHECK(out.size() == 1000u);
}

TEST_CASE("CapClipSamples — cap below size truncates from the tail") {
    std::vector<std::int16_t> clip(1000, std::int16_t{42});
    const auto out = pure::CapClipSamples(clip, 100);
    CHECK(out.size() == 100u);
    // Truncation keeps the head, so the first sample is unchanged.
    CHECK(out.front() == 42);
}

TEST_CASE("CapClipSamples — cap above size returns input unchanged") {
    std::vector<std::int16_t> clip(50, std::int16_t{7});
    const auto out = pure::CapClipSamples(clip, 1000);
    CHECK(out.size() == 50u);
}

#endif // SMATCHET_WITH_WHISPER
