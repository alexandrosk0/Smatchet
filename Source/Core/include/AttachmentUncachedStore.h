#pragma once

// AttachmentUncachedStore — the fallback copy of a downloaded attachment, for when the attachment disk cache
// cannot hold it (disk full, no cache folder). The bytes go to <user data>/cache/attachment_tmp/ under a unique
// name, owner-only, and files there are swept once a day old; the system temp folder and the working directory
// are never used. Every function does file I/O: worker only.

#include "AttachmentDiskCache.h"
#include "AttachmentAppUpdateService_detail.h"
#include "SmatchetResult.h"
#include "TrackerError.h"

#include <cstddef>
#include <string>

namespace smatchet {
namespace attachments {

/// Writes `fetched` as a uniquely named file under ResolveUncachedAttachmentDir(userDataDir) (created if
/// missing; expired copies there are swept first). Err when `userDataDir` is empty or the write fails: the
/// caller then opens the attachment by its URL.
Result<LocalAttachment, TrackerError>
WriteUncachedAttachment(const std::string& userDataDir, const std::string& filename, const FetchedAttachment& fetched);

/// Deletes files in `dir` last written at least kUncachedMaxAgeSeconds ago. Best-effort: a file a viewer still
/// holds open (Windows) is skipped. Returns how many files were removed.
std::size_t SweepUncachedAttachments(const std::string& dir);

} // namespace attachments
} // namespace smatchet
