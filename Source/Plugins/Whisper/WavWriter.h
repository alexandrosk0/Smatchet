#pragma once

// WavWriter — pure helper that serialises a PCM int16 sample buffer into a
// standard RIFF/WAVE byte stream (44-byte header + PCM payload).
//
// Pure: no I/O, no globals, no third-party deps beyond <vector>/<cstdint>.
// The caller decides where the resulting bytes land (memory buffer fed to a
// cpr multipart, std::ofstream for debugging, etc.). Tested directly by the
// doctest rig — see tests/Core/WavWriter.test.cpp.
//
// Format chosen: 16-bit signed PCM, little-endian. Matches OpenAI Whisper's
// preferred input (audio/wav, 16 kHz mono, 16-bit) and avoids the
// resampling / format-conversion noise that WMF / Media Foundation would
// otherwise impose on the capture path.

#include <cstdint>
#include <vector>

namespace smatchet {
namespace whisper {
namespace pure {

// Build a RIFF/WAVE byte stream from the supplied PCM int16 buffer. Always
// emits a valid 44-byte header even when `pcm` is empty (zero-length payload
// is a legal WAV — useful for "no audio captured" failure-path testing).
//
// Parameters:
//   pcm         interleaved int16 samples; size() must be a multiple of channels
//   sampleRate  Hz (e.g. 16000 for Whisper)
//   channels    1 (mono) or 2 (stereo)
//
// Invalid inputs (sampleRate <= 0, channels < 1 or > 2) return a 44-byte
// zero-filled header — the function never aborts; the caller can inspect
// the output and toast "invalid capture format".
std::vector<std::uint8_t> EncodeWav(const std::vector<std::int16_t>& pcm,
                                    int sampleRate,
                                    int channels);

} // namespace pure
} // namespace whisper
} // namespace smatchet
