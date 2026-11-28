#pragma once

// SmatchetFieldIconDiskCache — the disk half of the field-icon pipeline (grid priority icons, Lua icon maps).
// WORKER ONLY: every function here does file I/O, a network fetch or a decode, so it must run inside
// AppController::LaunchBackgroundTask and never on the UI thread (Pillar 2). It touches no ImGui, no
// texture cache and no AppController: the caller injects the fetch (cpr in production) and the decode
// (SmatchetImageTextureCache::DecodeIconBytesAnyThread), and posts the decoded pixels to the UI thread
// for the texture upload.
//
// Layout: <user data>/cache/field_icons/ holds one file per URL, named icon_ plus the SHA-256 of the URL plus
// .bin, which are the raw downloaded bytes, written atomically and owner-only (FileIo.h). A cache file that
// no longer decodes is deleted and fetched again, and bytes are never cached until they decode.

#include "Persistence/DecodedIcon.h"
#include "SmatchetFieldIconPrunePure.h"
#include "SmatchetResult.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace SmatchetFieldIconDiskCache {

/// Largest icon file read or fetched, in bytes. Real icons are a few KiB; the cap bounds what an
/// untrusted tracker or a hostile local file can make the worker allocate and decode.
constexpr std::size_t kMaxIconFileBytes = 512u * 1024u;

using IconBytes = std::vector<unsigned char>;
/// Fetch `url` (already host-confined by the caller) and return its body, at most kMaxIconFileBytes.
using FetchFn = std::function<Result<IconBytes>(const std::string& url)>;
/// Decode image bytes to RGBA32; must be safe off the UI thread.
using DecodeFn = std::function<Result<DecodedIcon>(const unsigned char* bytes, std::size_t size)>;

enum class IconSource {
    Disk,      ///< served from the icon cache directory
    Network,   ///< fetched just now (and cached when it decoded)
    LocalFile, ///< a local file the user or the app bundle supplied
};

struct IconResolveOutcome {
    bool Ok = false;
    DecodedIcon Icon;
    IconSource Source = IconSource::Disk;
    std::string Error;           ///< why it failed (empty when Ok)
    bool RemovedBadFile = false; ///< an undecodable cache file was deleted on the way
    bool NotFound = false;       ///< a local file did not exist (expected for an optional bundled icon)
};

/// `<userDataDir>cache/field_icons/`, created if missing. `userDataDir` ends in a separator (as
/// ConfigManager::GetUserDataDirectory returns it). False with `outError` set on an empty base or a
/// directory that cannot be created.
bool EnsureCacheDir(const std::string& userDataDir, std::string& outDir, std::string& outError);

/// The decoded icon for `url`: the cache file when it decodes, otherwise a fetch whose bytes are
/// decoded, then cached. A cache file that is too large or does not decode is removed and re-fetched
/// once. A failed fetch or decode leaves nothing in the cache.
IconResolveOutcome ResolveUrlIcon(const std::string& cacheDir, const std::string& url, const FetchFn& fetch,
                                  const DecodeFn& decode);

/// The decoded icon for a local file: a bounded read, then the decode. The file is never modified or
/// deleted (it is not ours).
IconResolveOutcome ResolveFileIcon(const std::string& path, const DecodeFn& decode);

enum class RevalidateResult {
    Fresh,     ///< no cache file, or it was written less than kRevalidateAfterSeconds ago: nothing done
    Refreshed, ///< the file was stale; a fetched, decodable copy replaced it
    Failed,    ///< the file was stale but the fetch or decode failed; the old file is untouched
};

/// Re-fetch `url` when its cache file is at least kRevalidateAfterSeconds old, so a changed server icon is
/// picked up within a week. The old file is replaced only by bytes that fetch within the size cap and decode;
/// any failure leaves it in place (an offline start never loses a good icon). Never throws. The decoded pixels
/// are not returned: the caller already showed the old copy, and the new one is used from the next launch.
RevalidateResult RevalidateUrlIcon(const std::string& cacheDir, const std::string& url, const FetchFn& fetch,
                                   const DecodeFn& decode);

/// Housekeeping of `cacheDir` (SelectIconPruneVictims): deletes legacy-named files, orphaned temps older than
/// `tempGraceSeconds`, and the oldest current icons until they total at most `capBytes`. Files whose names are
/// not icon cache names are never touched. Best-effort per file; returns how many files were removed.
std::size_t PruneCacheDir(const std::string& cacheDir, std::uint64_t capBytes,
                          std::int64_t tempGraceSeconds = kTempGraceSeconds);

/// Deletes every icon cache file in `cacheDir` (current and legacy names and temps); other files stay.
/// Returns how many files were removed.
std::size_t PurgeCacheDir(const std::string& cacheDir);

} // namespace SmatchetFieldIconDiskCache
