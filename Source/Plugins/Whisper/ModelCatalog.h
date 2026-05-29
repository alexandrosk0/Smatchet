#pragma once

// ModelCatalog — static catalog of the Whisper.cpp ggml models Smatchet is
// allowed to download. Pure helper TU: no third-party deps, no I/O beyond a
// single `IsModelPresent` filesystem probe. See
// docs/plans/shipped/whisper-dictation.md § Phase C / ModelCatalog.
//
// Entries describe English-only models in ascending size order:
//   - ggml-tiny.en   ~39 MB    "Smaller, faster"
//   - ggml-base.en   ~148 MB   "Recommended"          (default cfg.WhisperModel)
//   - ggml-small.en  ~466 MB   "Higher accuracy"
//   - ggml-medium.en ~1.5 GB   "Highest accuracy"     (Phase F — Preferences-only)
//
// `ggml-medium.en` is intentionally hidden from the first-run setup banner
// (the banner shows tiny / base / small for guided selection); users opt in
// to the medium model via the Preferences advanced row knowing the CPU /
// disk cost up-front.
//
// SHA-256 hashes come from the ggerganov/whisper.cpp huggingface mirror at
// https://huggingface.co/ggerganov/whisper.cpp/resolve/main/<model>.bin and
// pin the artefact identity ModelDownloader verifies after fetch. A future
// hash bump (model retraining) is a deliberate manifest edit, not a silent
// catalog update.

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace whisper {
namespace catalog {

struct Entry {
    /// Stable model id; matches the filename on huggingface without the
    /// `.bin` suffix. Used as the on-disk basename
    /// `<userData>/whisper/<id>.bin`. Stored in cfg.WhisperModel.
    std::string Id;
    /// Human-readable short label (paired with the size column in the
    /// banner picker + Preferences dropdown). Localised separately in
    /// SmatchetLocalization.cpp via the `whisper.modelPicker.*` keys; the
    /// English value here is the fallback the catalog returns directly to
    /// non-UI consumers (CLI commands, scenario harness).
    std::string DisplayName;
    /// Approximate download size in bytes. Used to display "(~150 MB)" in
    /// the picker and to seed a progress-bar estimate before the server's
    /// Content-Length response arrives.
    std::uint64_t SizeBytes;
    /// SHA-256 of the canonical `.bin` file. Lowercase hex, no separators.
    std::string Sha256;
    /// Absolute download URL on the huggingface mirror.
    std::string Url;
};

/// Returns the static catalog by const reference. Stable across the process
/// lifetime; safe to take a pointer to.
const std::vector<Entry>& All();

/// Returns a pointer to the entry whose `Id` matches `id` exactly, or
/// nullptr on miss. Const view — callers must not mutate.
const Entry* Find(const std::string& id);

/// Returns true when the on-disk model file `<destDir>/<id>.bin` exists.
/// `destDir` is expected to be the resolved
/// `ConfigManager::GetPlatformSharedUserDataDirectory() + "whisper/"`
/// directory; an empty `destDir` always reports false. Pure probe — no
/// hash verification, no size check.
bool IsModelPresent(const std::string& id, const std::string& destDir);

} // namespace catalog
} // namespace whisper
} // namespace smatchet
