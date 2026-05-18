// SilenceTrim — see header. Pure C++14 helper; doctest-covered in
// tests/Source_Core/SilenceTrim.test.cpp.

#include "SilenceTrim.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace smatchet {
namespace whisper {
namespace pure {

namespace {

// Compute the absolute-value sum of `[begin, end)` int16 samples. Uses 64-bit
// accumulator so 100 ms of full-scale PCM (16000 * 0.1 = 1600 samples *
// 32767 ~= 5.2e7) never overflows.
std::int64_t WindowEnergy(const std::int16_t* begin, const std::int16_t* end) {
    std::int64_t sum = 0;
    for (const std::int16_t* p = begin; p < end; ++p) {
        sum += std::abs(static_cast<int>(*p));
    }
    return sum;
}

} // namespace

std::vector<std::int16_t> TrimLeadingTrailingSilence(
    const std::vector<std::int16_t>& pcm,
    int sampleRate,
    int windowMs,
    float peakRelativeThreshold) {
    if (pcm.empty() || sampleRate <= 0 || windowMs <= 0) {
        return std::vector<std::int16_t>();
    }
    if (peakRelativeThreshold < 0.0f) {
        peakRelativeThreshold = 0.0f;
    }

    // Find the absolute peak amplitude in one pass.
    int peak = 0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const int a = std::abs(static_cast<int>(pcm[i]));
        if (a > peak) {
            peak = a;
        }
    }
    if (peak == 0) {
        // Pure silence — drop everything.
        return std::vector<std::int16_t>();
    }

    // Per-window energy threshold: 5 % of the peak amplitude times the
    // window's sample count. Multiplying once here avoids per-window
    // floating-point work later.
    const std::size_t samplesPerWindow =
        static_cast<std::size_t>((static_cast<long long>(sampleRate) * windowMs) / 1000);
    if (samplesPerWindow == 0 || samplesPerWindow > pcm.size()) {
        // Window larger than input — fall back to a "keep all non-zero" probe.
        return pcm;
    }
    const std::int64_t perSampleThreshold =
        static_cast<std::int64_t>(static_cast<float>(peak) * peakRelativeThreshold);
    const std::int64_t windowThreshold =
        perSampleThreshold * static_cast<std::int64_t>(samplesPerWindow);

    // Walk windows from the front until one exceeds threshold.
    std::size_t startSample = 0;
    while (startSample + samplesPerWindow <= pcm.size()) {
        const std::int64_t e = WindowEnergy(&pcm[startSample], &pcm[startSample] + samplesPerWindow);
        if (e > windowThreshold) {
            break;
        }
        startSample += samplesPerWindow;
    }

    // Walk windows from the tail until one exceeds threshold.
    std::size_t endSample = pcm.size();
    while (endSample >= startSample + samplesPerWindow) {
        const std::size_t windowStart = endSample - samplesPerWindow;
        const std::int64_t e = WindowEnergy(&pcm[windowStart], &pcm[windowStart] + samplesPerWindow);
        if (e > windowThreshold) {
            break;
        }
        endSample = windowStart;
    }

    if (endSample <= startSample) {
        // No window passed threshold — entire clip was below the gate.
        return std::vector<std::int16_t>();
    }
    return std::vector<std::int16_t>(pcm.begin() + static_cast<std::ptrdiff_t>(startSample),
                                     pcm.begin() + static_cast<std::ptrdiff_t>(endSample));
}

std::vector<std::int16_t> CapClipSamples(
    const std::vector<std::int16_t>& pcm,
    std::size_t maxSamples) {
    if (maxSamples == 0 || pcm.size() <= maxSamples) {
        return pcm;
    }
    return std::vector<std::int16_t>(pcm.begin(),
                                     pcm.begin() + static_cast<std::ptrdiff_t>(maxSamples));
}

} // namespace pure
} // namespace whisper
} // namespace smatchet
