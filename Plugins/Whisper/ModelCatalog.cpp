// ModelCatalog — see header for design pointers. Pure helper: the static
// `kEntries` table below is the single source of truth for "which whisper
// models is Smatchet allowed to download?". A future model rotation edits
// this table and bumps the SHA-256 in lockstep; ModelDownloader rejects any
// downloaded bytes whose hash does not match.
//
// Sizes are the byte counts reported by the huggingface mirror at the time
// of cataloguing; ModelDownloader uses them only as the pre-headers
// progress-bar estimate (the live Content-Length wins once the response
// arrives). SHA-256 values are the widely-published ggerganov whisper.cpp
// huggingface checksums; bumping them is a knowing manifest edit.

#include "ModelCatalog.h"

#include <ghc/filesystem.hpp>

#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

namespace smatchet {
namespace whisper {
namespace catalog {

namespace {

const std::vector<Entry>& BuildCatalog() {
    // Function-local static — constructed on first call, never re-initialised.
    static const std::vector<Entry> kEntries = []() {
        std::vector<Entry> v;
        v.reserve(3);

        Entry tiny;
        tiny.Id = "ggml-tiny.en";
        tiny.DisplayName = "Smaller, faster";
        tiny.SizeBytes = 77704715ull; // ~74 MiB on disk; rounded "~40 MB" in UI
        tiny.Sha256 = "921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f";
        tiny.Url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin";
        v.push_back(tiny);

        Entry base;
        base.Id = "ggml-base.en";
        base.DisplayName = "Recommended";
        base.SizeBytes = 147951465ull; // ~141 MiB on disk; rounded "~150 MB" in UI
        base.Sha256 = "a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002";
        base.Url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin";
        v.push_back(base);

        Entry small;
        small.Id = "ggml-small.en";
        small.DisplayName = "Higher accuracy";
        small.SizeBytes = 487601967ull; // ~465 MiB on disk; rounded "~500 MB" in UI
        small.Sha256 = "f953ad0fd29cacd07d5a9eef5c1e3c4f48996b2dac0a8a3f25ad434ea0d10cc3";
        small.Url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin";
        v.push_back(small);

        // Phase F addition — opt-in highest-accuracy model. CPU-heavy and ~1.5 GB on
        // disk; surfaced through Preferences only (the first-run banner sticks to
        // tiny / base / small for guided selection).
        Entry medium;
        medium.Id = "ggml-medium.en";
        medium.DisplayName = "Highest accuracy (1.5 GB) — CPU-heavy";
        medium.SizeBytes = 1533763059ull; // ~1.43 GiB on disk; rounded "~1.5 GB" in UI
        medium.Sha256 = "cc37e93478338ec7700281a7ac30a10128929eb8f427dda2e8faa4eccc8d0b5f";
        medium.Url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.en.bin";
        v.push_back(medium);

        return v;
    }();
    return kEntries;
}

} // namespace

const std::vector<Entry>& All() {
    return BuildCatalog();
}

const Entry* Find(const std::string& id) {
    if (id.empty()) {
        return nullptr;
    }
    const std::vector<Entry>& all = BuildCatalog();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].Id == id) {
            return &all[i];
        }
    }
    return nullptr;
}

bool IsModelPresent(const std::string& id, const std::string& destDir) {
    if (id.empty() || destDir.empty()) {
        return false;
    }
    // Compose `<destDir>/<id>.bin` without forcing the caller to strip a
    // trailing slash — both `<dir>/` and `<dir>` shapes are accepted.
    std::string path = destDir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += id;
    path += ".bin";

    std::error_code ec;
    const bool exists = ghc::filesystem::exists(path, ec);
    if (ec) {
        return false;
    }
    if (!exists) {
        return false;
    }
    // Reject zero-byte files: an aborted download that left
    // `<destDir>/<id>.bin` (not `<id>.bin.partial`) would otherwise look
    // present and skip future retries. SHA-256 verification is the
    // downloader's job; presence is the banner / Preferences hint only.
    const auto sz = ghc::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    return sz > 0;
}

} // namespace catalog
} // namespace whisper
} // namespace smatchet
