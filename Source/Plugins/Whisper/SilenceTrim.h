#pragma once

// SilenceTrim — pure helper that strips leading + trailing silence from a
// captured PCM buffer before transcription. Pillar 1 / Pillar 2 compliance:
// the algorithm is O(N) on 16 kHz mono int16 samples; trimming a 10 s clip
// takes < 1 ms on a modern CPU. Worker-thread call site only — never invoked
// from the UI thread.
// Algorithm — peak-relative amplitude gate over 100 ms windows:
//   1. Scan once for the absolute peak amplitude.
//   2. Pick a per-window energy threshold of `peak * 0.05` (5 % of peak,
//      empirically the boundary between "definite silence" and "barely-audible
//      breath" on typical WASAPI capture).
//   3. From the front: drop 100 ms windows whose sum-of-absolute-values is
//      below threshold until one exceeds it; repeat symmetrically from the
//      tail.
//   4. Return the remaining slice. Empty input → empty output. All-silence
//      input → empty output (every window drops).
// Header is pure C++14 — no Win32 / WASAPI / whisper.cpp deps — so the
// doctest rig can link it without dragging the Phase B/C/E transport in.

#include <cstdint>
#include <vector>

namespace smatchet {
namespace whisper {
namespace pure {

/// Trim leading + trailing silence from `pcm` (16-bit mono PCM at
/// `sampleRate` Hz). `windowMs` defaults to 100 ms; `peakRelativeThreshold`
/// defaults to 0.05f (5 % of the global peak). Both parameters are exposed
/// so the doctest rig can pin behaviour on synthetic inputs.
/// Returns the trimmed buffer. Pure — no I/O, no globals, no allocation
/// beyond the result vector.
std::vector<std::int16_t> TrimLeadingTrailingSilence(
    const std::vector<std::int16_t>& pcm,
    int sampleRate,
    int windowMs = 100,
    float peakRelativeThreshold = 0.05f);

/// Cap `pcm` at `maxSamples` (truncate from the tail). `maxSamples == 0`
/// disables the cap (returns `pcm` unchanged). Negative inputs are treated
/// as zero. Exposed as a separate helper so the WhisperPlugin worker can
/// apply the cap after trim without re-deriving sample-count math at every
/// call site.
std::vector<std::int16_t> CapClipSamples(
    const std::vector<std::int16_t>& pcm,
    std::size_t maxSamples);

} // namespace pure
} // namespace whisper
} // namespace smatchet
