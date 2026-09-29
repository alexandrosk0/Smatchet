#pragma once

// AttachmentDiskCache — attachments kept on disk so a previewed or opened attachment works again offline
// (Quality Pillar 6). One entry per attachment URL under <user data>/attachment_cache (layout and rules
// in AttachmentCachePure.h): the file itself plus entry.json, both written temp-then-rename. The cache is
// pruned back to its cap, least recently used first, after each download. Thread-safe; every method does
// file I/O, so call it from a worker, never the UI thread.

#include "AttachmentCachePure.h"
#include "SmatchetResult.h"
#include "Tracker/TrackerError.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace smatchet {
namespace attachments {

/// A local copy of an attachment, ready for a viewer.
struct LocalAttachment {
    std::string FilePath;
    std::string Mime;
};

/// The bytes a download produced.
struct FetchedAttachment {
    std::string Bytes;
    std::string Mime;
};

class AttachmentDiskCache {
  public:
    /// `userDataDir` is the directory the cache confines itself under; nothing is created until the first
    /// Store.
    explicit AttachmentDiskCache(std::string userDataDir, std::uint64_t capBytes = kCacheCapBytes,
                                 std::int64_t graceSeconds = kEvictionGraceSeconds);

    /// The saved copy of `url`, if there is one, marked as just used. Never touches the network.
    bool Lookup(const std::string& url, LocalAttachment& out);

    /// Saves `fetched` for `url` under a sanitized `fileName` (its extension derived from the MIME type
    /// when the name has none) and returns where it landed.
    Result<LocalAttachment, TrackerError> Store(const std::string& url, const std::string& fileName,
                                                const FetchedAttachment& fetched);

    /// Deletes entries, least recently used first, until the cache fits its cap (SelectEvictions).
    void Prune();

  private:
    bool ResolveEntryPath(const std::string& relativePath, std::string& out) const;

    const std::string userDataDir_;
    const std::uint64_t capBytes_;
    const std::int64_t graceSeconds_;
    std::mutex mutex_; ///< one writer at a time, and no eviction between a lookup's check and its touch
};

struct AttachmentRequest {
    std::string Url;
    std::string FileName;
    bool Offline = false;           ///< the connectivity probe says the tracker is unreachable
    std::string OfflineMissMessage; ///< the error for an attachment never downloaded, when offline
};

using AttachmentFetchFn = std::function<Result<FetchedAttachment, TrackerError>()>;
using UncachedWriteFn = std::function<Result<LocalAttachment, TrackerError>(const FetchedAttachment&)>;

/// Cache first. A saved copy is returned with no network. Offline, a miss fails at once with
/// `OfflineMissMessage`, and so does a fetch that fails with a Transport error. Otherwise the download
/// runs, is saved, and the cache is pruned; when saving fails the bytes go to `writeUncached` instead, so
/// the attachment still opens.
Result<LocalAttachment, TrackerError> ResolveAttachment(AttachmentDiskCache& cache, const AttachmentRequest& request,
                                                        const AttachmentFetchFn& fetch,
                                                        const UncachedWriteFn& writeUncached);

} // namespace attachments
} // namespace smatchet
