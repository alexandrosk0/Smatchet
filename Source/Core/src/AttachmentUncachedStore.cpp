#include "AttachmentUncachedStore.h"

#include "AttachmentMimeUtils.h"
#include "FileIo.h"
#include "ListRegularFiles.h"
#include "Logger.h"

#include <ghc/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace smatchet {
namespace attachments {

namespace {

namespace fs = ghc::filesystem;

// A name no other download picks: time, thread and a counter, so concurrent downloads never collide even when
// the clock is coarser than the download rate. `dir` ends in a separator.
std::string UniqueFilePath(const std::string& dir, const std::string& filename, const std::string& extension) {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    const std::size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const std::uint64_t unique = static_cast<std::uint64_t>(now) ^ (static_cast<std::uint64_t>(tid) << 16) ^
                                 counter.fetch_add(1, std::memory_order_relaxed);
    return dir + std::to_string(unique) + "_" + StoredFileName(filename, extension);
}

} // namespace

std::size_t SweepUncachedAttachments(const std::string& dir, std::int64_t maxAgeSeconds) {
    std::size_t removed = 0;
    try {
        const std::int64_t now = filetime::NowFileClockEpochSeconds();
        std::vector<EntryFileInfo> files;
        for (const filetime::ListedFile& listed : filetime::ListRegularFiles(dir)) {
            EntryFileInfo file;
            file.Name = listed.Name;
            file.LastModifiedEpochSec = listed.ModifiedEpochSec;
            files.push_back(std::move(file));
        }
        for (const std::string& name : SelectExpiredUncachedFiles(files, now, maxAgeSeconds)) {
            std::error_code rmEc;
            if (fs::remove(fs::path(dir) / name, rmEc) && !rmEc) {
                ++removed;
            } else if (rmEc) {
                LOG_DEBUG("AttachmentUncachedStore: could not delete old copy '%s': %s", name.c_str(),
                          rmEc.message().c_str());
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("AttachmentUncachedStore: sweep of '%s' failed: %s", dir.c_str(), ex.what());
    }
    return removed;
}

Result<LocalAttachment, TrackerError>
WriteUncachedAttachment(const std::string& userDataDir, const std::string& filename, const FetchedAttachment& fetched) {
    using WriteResult = Result<LocalAttachment, TrackerError>;
    const std::string dir = ResolveUncachedAttachmentDir(userDataDir);
    if (dir.empty()) {
        return WriteResult::Err(TrackerErrorUnknown("There is no user data folder for a temporary attachment copy."));
    }
    try {
        std::error_code ec;
        fs::create_directories(fs::path(dir), ec);
        if (ec) {
            return WriteResult::Err(
                TrackerErrorUnknown("Failed to create the temporary attachment folder: " + ec.message()));
        }
        (void)SweepUncachedAttachments(dir);
        const std::string filePath = UniqueFilePath(dir, filename, ExtensionFromMime(fetched.Mime));
        fileio::AtomicWriteOptions options;
        options.OwnerOnly = true;
        options.Durable = false;
        std::string writeError;
        if (!fileio::WriteFileAtomically(filePath, fetched.Bytes.data(), fetched.Bytes.size(), options, writeError)) {
            return WriteResult::Err(TrackerErrorUnknown("Failed to write downloaded attachment bytes: " + writeError));
        }
        LocalAttachment local;
        local.FilePath = filePath;
        local.Mime = fetched.Mime;
        return WriteResult::Ok(std::move(local));
    } catch (const std::exception& ex) {
        return WriteResult::Err(TrackerErrorUnknown(std::string("Failed to save the attachment copy: ") + ex.what()));
    }
}

} // namespace attachments
} // namespace smatchet
