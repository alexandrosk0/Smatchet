// WavWriter — doctest coverage for the pure RIFF/WAVE encoder. Exercises the
// header byte layout (every offset matches the spec) plus round-trip of a
// trivial silent payload.
//
// The encoder is the only thing between the WASAPI capture path and OpenAI's
// audio decoder, so a bad header is a silent end-to-end break. The header
// asserts here are the load-bearing tests; the payload-bytes assertion is
// belt-and-braces.

#include <doctest/doctest.h>

#include "WavWriter.h"

#include <cstdint>
#include <vector>

using smatchet::whisper::pure::EncodeWav;

namespace {

// Read a little-endian 32-bit integer out of the byte buffer at offset `off`.
std::uint32_t ReadLE32(const std::vector<std::uint8_t>& buf, std::size_t off) {
    return static_cast<std::uint32_t>(buf[off]) | (static_cast<std::uint32_t>(buf[off + 1]) << 8) |
           (static_cast<std::uint32_t>(buf[off + 2]) << 16) |
           (static_cast<std::uint32_t>(buf[off + 3]) << 24);
}

std::uint16_t ReadLE16(const std::vector<std::uint8_t>& buf, std::size_t off) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(buf[off]) |
                                      (static_cast<std::uint16_t>(buf[off + 1]) << 8));
}

} // namespace

TEST_CASE("WavWriter: header layout for 1 s mono 16 kHz silence") {
    const int sampleRate = 16000;
    const int channels = 1;
    const std::vector<std::int16_t> pcm(sampleRate, 0); // 1 s of zero samples
    const std::vector<std::uint8_t> out = EncodeWav(pcm, sampleRate, channels);

    // 44 bytes header + 16000 samples * 2 bytes/sample = 32044
    REQUIRE(out.size() == 44u + (16000u * 2u));

    // "RIFF" + chunk size (file size - 8) + "WAVE"
    CHECK(out[0] == 'R');
    CHECK(out[1] == 'I');
    CHECK(out[2] == 'F');
    CHECK(out[3] == 'F');
    CHECK(ReadLE32(out, 4) == 36u + (16000u * 2u));
    CHECK(out[8] == 'W');
    CHECK(out[9] == 'A');
    CHECK(out[10] == 'V');
    CHECK(out[11] == 'E');

    // "fmt " sub-chunk header
    CHECK(out[12] == 'f');
    CHECK(out[13] == 'm');
    CHECK(out[14] == 't');
    CHECK(out[15] == ' ');
    CHECK(ReadLE32(out, 16) == 16u);    // PCM fmt chunk size
    CHECK(ReadLE16(out, 20) == 1u);     // PCM format tag
    CHECK(ReadLE16(out, 22) == 1u);     // channels
    CHECK(ReadLE32(out, 24) == 16000u); // sample rate
    CHECK(ReadLE32(out, 28) == 16000u * 1u * 2u); // byte rate
    CHECK(ReadLE16(out, 32) == 1u * 2u);          // block align
    CHECK(ReadLE16(out, 34) == 16u);              // bits per sample

    // "data" sub-chunk header
    CHECK(out[36] == 'd');
    CHECK(out[37] == 'a');
    CHECK(out[38] == 't');
    CHECK(out[39] == 'a');
    CHECK(ReadLE32(out, 40) == 16000u * 2u); // data bytes

    // Payload spot-check: first 8 sample bytes are zero.
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(out[44 + i] == 0u);
    }
}

TEST_CASE("WavWriter: stereo doubles block align + byte rate") {
    const std::vector<std::int16_t> pcm(2u * 16000u, 0); // 1 s stereo
    const std::vector<std::uint8_t> out = EncodeWav(pcm, 16000, 2);
    REQUIRE(out.size() == 44u + (2u * 16000u * 2u));
    CHECK(ReadLE16(out, 22) == 2u);                // channels
    CHECK(ReadLE32(out, 28) == 16000u * 2u * 2u);  // byte rate
    CHECK(ReadLE16(out, 32) == 2u * 2u);           // block align
    CHECK(ReadLE32(out, 40) == 2u * 16000u * 2u);  // data bytes
}

TEST_CASE("WavWriter: zero-length payload still emits a valid 44-byte header") {
    const std::vector<std::int16_t> pcm;
    const std::vector<std::uint8_t> out = EncodeWav(pcm, 16000, 1);
    REQUIRE(out.size() == 44u);
    CHECK(out[0] == 'R');
    CHECK(out[8] == 'W');
    CHECK(ReadLE32(out, 4) == 36u); // 36 + 0 data bytes
    CHECK(ReadLE32(out, 40) == 0u); // data bytes = 0
    CHECK(ReadLE16(out, 22) == 1u); // channels
}

TEST_CASE("WavWriter: invalid sample rate returns zero-filled 44-byte header (no abort)") {
    const std::vector<std::int16_t> pcm(100, 0);
    const std::vector<std::uint8_t> out = EncodeWav(pcm, 0, 1);
    REQUIRE(out.size() == 44u);
    CHECK(out[0] == 0u); // not "RIFF"
}

TEST_CASE("WavWriter: invalid channel count returns zero-filled 44-byte header") {
    const std::vector<std::int16_t> pcm(100, 0);
    const std::vector<std::uint8_t> outZero = EncodeWav(pcm, 16000, 0);
    REQUIRE(outZero.size() == 44u);
    CHECK(outZero[0] == 0u);
    const std::vector<std::uint8_t> outTooMany = EncodeWav(pcm, 16000, 5);
    REQUIRE(outTooMany.size() == 44u);
    CHECK(outTooMany[0] == 0u);
}

TEST_CASE("WavWriter: encoded samples round-trip as little-endian int16") {
    std::vector<std::int16_t> pcm;
    pcm.push_back(0);
    pcm.push_back(1);
    pcm.push_back(-1);
    pcm.push_back(32767);
    pcm.push_back(-32768);
    const std::vector<std::uint8_t> out = EncodeWav(pcm, 16000, 1);
    REQUIRE(out.size() == 44u + (pcm.size() * 2u));

    // 0  -> 00 00
    CHECK(out[44] == 0x00u); CHECK(out[45] == 0x00u);
    // 1  -> 01 00
    CHECK(out[46] == 0x01u); CHECK(out[47] == 0x00u);
    // -1 -> FF FF (two's complement)
    CHECK(out[48] == 0xFFu); CHECK(out[49] == 0xFFu);
    // 32767 -> FF 7F
    CHECK(out[50] == 0xFFu); CHECK(out[51] == 0x7Fu);
    // -32768 -> 00 80
    CHECK(out[52] == 0x00u); CHECK(out[53] == 0x80u);
}
