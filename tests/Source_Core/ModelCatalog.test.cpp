// ModelCatalog — Phase C doctest coverage. The catalog is pure data; the
// tests pin the entry count, the schema (non-empty url + sha256 + display
// name on every row), and the `Find` API's miss case. SHA-256 strings are
// the source of truth ModelDownloader verifies against, so a typo here
// would surface immediately as a failed download in manual smoke tests.

#include <doctest/doctest.h>

#if defined(SMATCHET_WITH_WHISPER)

#include "ModelCatalog.h"

#include <cctype>
#include <string>

namespace cat = smatchet::whisper::catalog;

TEST_CASE("ModelCatalog::All returns four English-only entries in ascending size") {
    // Phase F: catalog grew from 3 -> 4 entries (added ggml-medium.en as the
    // opt-in highest-accuracy Preferences-only row). Ascending-size invariant
    // still holds across the full list.
    const auto& entries = cat::All();
    CHECK(entries.size() == 4u);
    if (entries.size() >= 4u) {
        CHECK(entries[0].SizeBytes < entries[1].SizeBytes);
        CHECK(entries[1].SizeBytes < entries[2].SizeBytes);
        CHECK(entries[2].SizeBytes < entries[3].SizeBytes);
    }
}

TEST_CASE("ModelCatalog::All every entry has non-empty url + sha256 + display name") {
    const auto& entries = cat::All();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        CAPTURE(entries[i].Id);
        CHECK_FALSE(entries[i].Id.empty());
        CHECK_FALSE(entries[i].DisplayName.empty());
        CHECK_FALSE(entries[i].Url.empty());
        CHECK_FALSE(entries[i].Sha256.empty());
        // SHA-256 hex is exactly 64 lowercase hex chars.
        CHECK(entries[i].Sha256.size() == 64u);
        for (char c : entries[i].Sha256) {
            const bool isHex = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            CHECK(isHex);
        }
        CHECK(entries[i].SizeBytes > 0u);
        // Smoke: url points at the canonical huggingface mirror with the
        // expected `ggml-` filename prefix.
        CHECK(entries[i].Url.find(
            "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-") == 0u);
    }
}

TEST_CASE("ModelCatalog: SHA-256 hashes pinned to the live huggingface mirror") {
    // Regression gate against catalog-hash drift. The pre-fix catalog shipped
    // stale hashes for ggml-small.en and ggml-medium.en — every download
    // produced a SHA-256 mismatch and never landed. Pin the live X-Linked-ETag
    // values here so a future hash bump is a knowing edit, not silent breakage.
    //
    // To refresh after a deliberate upstream rotation:
    //   curl -sIL https://huggingface.co/ggerganov/whisper.cpp/resolve/main/<id>.bin
    //     | grep X-Linked-ETag
    auto findHash = [](const std::string& id) -> std::string {
        const auto* e = cat::Find(id);
        return e ? e->Sha256 : std::string();
    };
    CHECK(findHash("ggml-tiny.en") ==
          "921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f");
    CHECK(findHash("ggml-base.en") ==
          "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002");
    CHECK(findHash("ggml-small.en") ==
          "c6138d6d58ecc8322097e0f987c32f1be8bb0a18532a3f88f734d1bbf9c41e5d");
    CHECK(findHash("ggml-medium.en") ==
          "cc37e93478338ec7700281a7ac30a10128929eb8f427dda2e865faa8f6da4356");
}

TEST_CASE("ModelCatalog::Find returns the matching entry for known ids") {
    CHECK(cat::Find("ggml-tiny.en") != nullptr);
    CHECK(cat::Find("ggml-base.en") != nullptr);
    CHECK(cat::Find("ggml-small.en") != nullptr);
    // Phase F — medium.en is now a catalog entry. Surfaced through Preferences
    // only; the first-run banner filters it out at the radio-button loop.
    CHECK(cat::Find("ggml-medium.en") != nullptr);
    const auto* base = cat::Find("ggml-base.en");
    CHECK(base != nullptr);
    if (base != nullptr) {
        CHECK(base->Id == "ggml-base.en");
    }
}

TEST_CASE("ModelCatalog::Find returns nullptr on miss / empty / unknown id") {
    CHECK(cat::Find(std::string()) == nullptr);
    CHECK(cat::Find("ggml-large.v3") == nullptr); // not in catalog (yet)
    CHECK(cat::Find("GGML-BASE.EN") == nullptr);  // case-sensitive match
}

TEST_CASE("ModelCatalog::IsModelPresent returns false on empty inputs") {
    CHECK_FALSE(cat::IsModelPresent("", ""));
    CHECK_FALSE(cat::IsModelPresent("ggml-base.en", ""));
    CHECK_FALSE(cat::IsModelPresent("", "C:\\tmp"));
    // A definitely-not-present file under an existing temp-ish dir.
    CHECK_FALSE(cat::IsModelPresent("ggml-tiny.en", "C:\\nonexistent-dir-for-tests-9384"));
}

#endif // SMATCHET_WITH_WHISPER
