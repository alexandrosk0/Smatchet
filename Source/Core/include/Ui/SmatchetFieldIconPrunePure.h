#pragma once

// Pure decisions behind the field-icon cache directory's housekeeping (no I/O, no Logger): which files a
// prune deletes and when a cached icon is old enough to re-fetch. SmatchetFieldIconDiskCache does the file
// work.

#include <cstdint>
#include <string>
#include <vector>

namespace SmatchetFieldIconDiskCache {

/// A cached icon older than this is re-fetched once, so a changed server icon is picked up within a week.
constexpr std::int64_t kRevalidateAfterSeconds = 7 * 24 * 60 * 60;

/// An orphaned temp younger than this may still be being written by a concurrent resolve.
constexpr std::int64_t kTempGraceSeconds = 10 * 60;

/// True when a file last written at `modifiedEpochSec` is at least `maxAgeSeconds` old at `nowEpochSec`.
/// A modification time in the future (clock change) is never stale.
inline bool IsStale(std::int64_t modifiedEpochSec, std::int64_t nowEpochSec, std::int64_t maxAgeSeconds) {
    return nowEpochSec - modifiedEpochSec >= maxAgeSeconds && modifiedEpochSec <= nowEpochSec;
}

/// One regular file found in the icon cache directory.
struct IconCacheFile {
    std::string Name;
    std::uint64_t SizeBytes = 0;
    std::int64_t ModifiedEpochSec = 0;
};

/// The names a prune deletes, in deletion order:
///   1. legacy-named files (IsLegacyIconCacheFileName): older builds' 64-bit-hash names, never read again;
///   2. orphaned temps: "<icon name>.part-<digits>-<digits>" last written at least `tempGraceSeconds` ago;
///   3. current icons (IsIconCacheFileName), oldest write first (ties by name), until the remaining current
///      icons total at most `capBytes` (a zero cap drops all of them).
/// Anything else in the directory is not ours and is never selected or counted.
std::vector<std::string> SelectIconPruneVictims(const std::vector<IconCacheFile>& files, std::uint64_t capBytes,
                                                std::int64_t nowEpochSec, std::int64_t tempGraceSeconds);

} // namespace SmatchetFieldIconDiskCache
