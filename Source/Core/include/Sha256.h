#pragma once

// Sha256 — a small, dependency-free, header-only SHA-256 (FIPS 180-4) used for
// content fingerprinting (e.g. the Lua script consent gate keys approvals on the
// sha-256 of the script bytes). Header-only + inline so no translation unit needs
// registering in CMake and any consumer just includes this. NOT a constant-time /
// side-channel-hardened primitive — it is a content digest, not a MAC or password
// hash. For streaming or very large inputs prefer an OS primitive; this hashes a
// whole in-memory buffer, which is all the consent gate needs (scripts are small
// and already read fully into memory before execution).

#include <cstddef>
#include <cstdint>
#include <string>

namespace smatchet {
namespace hashing {

namespace sha256_detail {

inline std::uint32_t Rotr(std::uint32_t value, unsigned bits) { return (value >> bits) | (value << (32u - bits)); }

} // namespace sha256_detail

// Return the lowercase-hex SHA-256 of `bytes`. Deterministic; empty input hashes
// to the well-known e3b0c442... digest.
inline std::string Sha256Hex(const std::string& bytes) {
    using sha256_detail::Rotr;

    static const std::uint32_t kRoundConstants[64] = {
        // SMATCHET_DEVIATION(rule=duplication; reason=fixed SHA-256 constants; owner=orchestrator; revisit=2099-01-01)
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    // Padded message: original bytes + 0x80 + zero-fill to 56 mod 64 + 64-bit big-endian bit length.
    const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8u;
    std::string msg = bytes;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64u != 56u) {
        msg.push_back('\0');
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        msg.push_back(static_cast<char>((bitLength >> shift) & 0xffu));
    }

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64u) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const std::size_t p = chunk + static_cast<std::size_t>(i) * 4u;
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(msg[p])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(msg[p + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(msg[p + 2])) << 8) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(msg[p + 3])));
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = hh + S1 + ch + kRoundConstants[i] + w[i];
            const std::uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    static const char* const kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(kHex[(h[i] >> shift) & 0xfu]);
        }
    }
    return out;
}

} // namespace hashing
} // namespace smatchet
