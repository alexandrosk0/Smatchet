// WavWriter — see header for the format contract.

#include "WavWriter.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace smatchet {
namespace whisper {
namespace pure {

namespace {

// Helpers to write little-endian integers into a byte buffer at offset `off`.
// Standalone functions instead of templates so the C++14 build stays simple
// and clang-tidy doesn't flag implicit-conversion warnings on int promotion.
void WriteLE16(std::vector<std::uint8_t>& out, std::size_t off, std::uint16_t v) {
    out[off + 0] = static_cast<std::uint8_t>(v & 0xFFu);
    out[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void WriteLE32(std::vector<std::uint8_t>& out, std::size_t off, std::uint32_t v) {
    out[off + 0] = static_cast<std::uint8_t>(v & 0xFFu);
    out[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    out[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    out[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

constexpr std::size_t kHeaderBytes = 44;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr std::uint16_t kPcmFormatTag = 1; // WAVE_FORMAT_PCM
constexpr std::uint16_t kBytesPerSample = kBitsPerSample / 8;

} // namespace

std::vector<std::uint8_t> EncodeWav(const std::vector<std::int16_t>& pcm,
                                    int sampleRate,
                                    int channels) {
    std::vector<std::uint8_t> out;
    out.resize(kHeaderBytes);
    std::memset(out.data(), 0, kHeaderBytes);

    // Reject pathological inputs without aborting. A 44-byte zero header is a
    // legal (if useless) blob; the caller decides whether to ship it.
    if (sampleRate <= 0 || channels < 1 || channels > 2) {
        return out;
    }

    const std::uint32_t numSamples = static_cast<std::uint32_t>(pcm.size());
    const std::uint32_t dataBytes = numSamples * kBytesPerSample;
    const std::uint32_t fileMinusRiff = 36u + dataBytes; // 36 = 44 - 8 (RIFF header)

    // RIFF chunk descriptor: "RIFF" | size | "WAVE"
    out[0] = 'R'; out[1] = 'I'; out[2] = 'F'; out[3] = 'F';
    WriteLE32(out, 4, fileMinusRiff);
    out[8] = 'W'; out[9] = 'A'; out[10] = 'V'; out[11] = 'E';

    // "fmt " sub-chunk: id | size=16 | audioFormat=PCM=1 | numChannels |
    //     sampleRate | byteRate | blockAlign | bitsPerSample
    out[12] = 'f'; out[13] = 'm'; out[14] = 't'; out[15] = ' ';
    WriteLE32(out, 16, 16u);
    WriteLE16(out, 20, kPcmFormatTag);
    WriteLE16(out, 22, static_cast<std::uint16_t>(channels));
    WriteLE32(out, 24, static_cast<std::uint32_t>(sampleRate));
    const std::uint32_t byteRate =
        static_cast<std::uint32_t>(sampleRate) * static_cast<std::uint32_t>(channels) * kBytesPerSample;
    WriteLE32(out, 28, byteRate);
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels) * kBytesPerSample;
    WriteLE16(out, 32, blockAlign);
    WriteLE16(out, 34, kBitsPerSample);

    // "data" sub-chunk: id | size | payload
    out[36] = 'd'; out[37] = 'a'; out[38] = 't'; out[39] = 'a';
    WriteLE32(out, 40, dataBytes);

    if (dataBytes > 0) {
        out.resize(kHeaderBytes + dataBytes);
        // memcpy the int16 samples as little-endian bytes. Smatchet targets x86
        // / x86_64 — both little-endian — so a direct copy is correct. If we
        // ever ship to ARM big-endian (unlikely for a Windows desktop app) the
        // copy must become a per-sample WriteLE16 loop instead.
        std::memcpy(out.data() + kHeaderBytes, pcm.data(), dataBytes);
    }
    return out;
}

} // namespace pure
} // namespace whisper
} // namespace smatchet
