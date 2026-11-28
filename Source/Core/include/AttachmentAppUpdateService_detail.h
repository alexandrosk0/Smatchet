#pragma once

// Pure helpers of AttachmentAppUpdateService (no I/O, no cpr): where an attachment that could not be cached
// is written, and which of those files have outlived their use. Header-only so the Linux doctest subset
// covers them without the cpr-linked service TU.

#include "AttachmentCachePure.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace attachments {

/// An uncached attachment copy is deleted once it is this old: any viewer opened it long ago.
constexpr std::int64_t kUncachedMaxAgeSeconds = 24 * 60 * 60;

/// The directory an attachment is written to when the cache cannot hold it: `<userDataDir>cache/attachment_tmp/`
/// (a separator is added when `userDataDir` lacks one). Empty when `userDataDir` is empty: there is no
/// fallback to the working directory or the system temp folder, so a missing user data folder means the
/// attachment is opened by its URL instead of landing in a world-readable or arbitrary place.
inline std::string ResolveUncachedAttachmentDir(const std::string& userDataDir) {
    if (userDataDir.empty()) {
        return std::string();
    }
    std::string dir = userDataDir;
    const char last = dir.back();
    if (last != '/' && last != '\\') {
        dir.push_back('/');
    }
    return dir + "cache/attachment_tmp/";
}

/// The names of the files in the uncached-copy directory last written at least `maxAgeSeconds` before
/// `nowEpochSec`, sorted. A modification time in the future (a clock change) is never expired.
inline std::vector<std::string> SelectExpiredUncachedFiles(const std::vector<EntryFileInfo>& files,
                                                           std::int64_t nowEpochSec, std::int64_t maxAgeSeconds) {
    std::vector<std::string> expired;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::int64_t age = nowEpochSec - files[i].LastModifiedEpochSec;
        if (age >= maxAgeSeconds && files[i].LastModifiedEpochSec <= nowEpochSec) {
            expired.push_back(files[i].Name);
        }
    }
    std::sort(expired.begin(), expired.end());
    return expired;
}

} // namespace attachments
} // namespace smatchet
