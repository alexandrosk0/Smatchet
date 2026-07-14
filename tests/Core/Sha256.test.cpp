// Sha256 — header-only FIPS 180-4 digest used for Lua-script content
// fingerprinting (LuaScriptConsent). Pinned against the canonical NIST/known
// test vectors so a regression in the round logic is caught immediately.

#include <doctest/doctest.h>

#include "Sha256.h"

#include <string>

using smatchet::hashing::Sha256Hex;

TEST_CASE("Sha256Hex — empty input is the canonical e3b0c442 digest") {
    CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("Sha256Hex — 'abc' NIST vector") {
    CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("Sha256Hex — 448-bit block-boundary NIST vector") {
    CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("Sha256Hex — multi-block (896-bit) NIST vector") {
    CHECK(Sha256Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrs"
                    "mnopqrstnopqrstu") == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("Sha256Hex — one million 'a' characters") {
    const std::string million(1000000, 'a');
    CHECK(Sha256Hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("Sha256Hex — length-56 input exercises the extra padding block") {
    // 56 bytes forces the 0x80 + length to spill into a second 64-byte block.
    const std::string s(56, 'x');
    const std::string digest = Sha256Hex(s);
    CHECK(digest.size() == 64);
    // A single differing byte must change the digest (avalanche sanity).
    std::string s2 = s;
    s2[55] = 'y';
    CHECK(Sha256Hex(s2) != digest);
}

TEST_CASE("Sha256Hex — output is always 64 lowercase hex chars") {
    const std::string d = Sha256Hex("Smatchet automation script v1\n");
    CHECK(d.size() == 64);
    for (char c : d) {
        CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
}
