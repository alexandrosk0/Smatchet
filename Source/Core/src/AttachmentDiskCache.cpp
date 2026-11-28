#include "AttachmentDiskCache.h"

#include "AttachmentMimeUtils.h"
#include "FileIo.h"
#include "FileTimeSeconds.h"
#include "Commands/PathConfinement.h"
#include "Logger.h"

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <system_error>
#include <utility>
#include <vector>

namespace smatchet {
namespace attachments {

namespace {

namespace fs = ghc::filesystem;

constexpr std::uintmax_t kMaxEntryMetaBytes = 16u * 1024u;
constexpr const char* kDefaultMime = "application/octet-stream";

using smatchet::filetime::ToEpochSeconds;

std::int64_t NowEpochSeconds() { return smatchet::filetime::NowEpochSeconds<fs::file_time_type::clock>(); }

// Atomic temp-then-rename through the shared writer (FileIo.h): a reader never sees a half-written file, the
// temp is a unique exclusive sibling created owner-only, and a failure removes it. No fsync: a cache entry
// can be fetched again.
bool WriteCacheFile(const std::string& path, const std::string& bytes, bool isAttachmentData, std::string& errOut) {
    // An entry's file never changes for a URL, so identical bytes already on disk are the same attachment.
    // Skipping the write keeps a file a viewer holds open (Windows refuses to replace it) untouched.
    if (isAttachmentData && smatchet::fileio::FileContentEquals(path, bytes.data(), bytes.size())) {
        return true;
    }
    smatchet::fileio::AtomicWriteOptions options;
    options.OwnerOnly = true;
    options.Durable = false;
    if (smatchet::fileio::WriteFileAtomically(path, bytes.data(), bytes.size(), options, errOut)) {
        return true;
    }
    // The replace can still fail on Windows when a viewer opened the file between the compare and the move; an
    // identical file is then still the right outcome. Metadata can change and must be replaced successfully.
    if (isAttachmentData && smatchet::fileio::FileContentEquals(path, bytes.data(), bytes.size())) {
        errOut.clear();
        return true;
    }
    return false;
}

bool ReadSmallFile(const std::string& path, std::uintmax_t maxBytes, std::string& out) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(fs::path(path), ec);
    if (ec || size > maxBytes) {
        return false;
    }
    fs::ifstream in(fs::path(path), std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    out.assign(static_cast<std::size_t>(size), '\0');
    in.read(&out[0], static_cast<std::streamsize>(size));
    return static_cast<std::uintmax_t>(in.gcount()) == size;
}

// Size, last use, completeness and orphaned temp files of one entry directory (its files are never nested).
CacheEntryInfo DescribeEntry(const fs::path& dir, const std::string& key, std::int64_t nowEpochSec,
                             std::int64_t graceSeconds) {
    CacheEntryInfo info;
    info.Key = key;
    info.Complete = false;
    std::error_code ec;
    info.LastUsedEpochSec = ToEpochSeconds(fs::last_write_time(dir, ec));
    std::vector<EntryFileInfo> files;
    std::vector<std::uint64_t> sizes;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) {
            continue;
        }
        EntryFileInfo file;
        file.Name = it->path().filename().string();
        file.LastModifiedEpochSec = ToEpochSeconds(it->last_write_time(fileEc));
        if (fileEc) {
            file.LastModifiedEpochSec = nowEpochSec; // unknown age: treat as fresh so it is never deleted
        }
        std::error_code sizeEc;
        const std::uintmax_t size = it->file_size(sizeEc);
        files.push_back(std::move(file));
        sizes.push_back(sizeEc ? 0u : static_cast<std::uint64_t>(size));
    }
    // Only an entry whose entry.json parses has a known live data file; for any other (an interrupted
    // download, a damaged entry.json) nothing is swept here and SelectEvictions deals with the whole entry.
    std::string metaJson;
    EntryMeta meta;
    if (ReadSmallFile((dir / kEntryMetaFileName).string(), kMaxEntryMetaBytes, metaJson) &&
        ParseEntryMeta(metaJson, meta)) {
        info.StaleFiles = SelectStaleEntryFiles(files, meta.FileName, nowEpochSec, graceSeconds);
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (std::binary_search(info.StaleFiles.begin(), info.StaleFiles.end(), files[i].Name)) {
            continue;
        }
        info.SizeBytes += sizes[i];
        if (files[i].LastModifiedEpochSec > info.LastUsedEpochSec) {
            info.LastUsedEpochSec = files[i].LastModifiedEpochSec;
        }
        if (files[i].Name == kEntryMetaFileName) {
            info.Complete = true;
        }
    }
    return info;
}

} // namespace

AttachmentDiskCache::AttachmentDiskCache(std::string userDataDir, std::uint64_t capBytes, std::int64_t graceSeconds)
    : userDataDir_(std::move(userDataDir)), capBytes_(capBytes), graceSeconds_(graceSeconds) {}

bool AttachmentDiskCache::ResolveEntryPath(const std::string& relativePath, std::string& out) const {
    std::string err;
    if (!smatchet::cmd::ConfinePathUnderSubdir(userDataDir_, kCacheSubdir, relativePath, out, err)) {
        LOG_WARN("AttachmentDiskCache: cache path rejected: %s", err.c_str());
        return false;
    }
    return true;
}

bool AttachmentDiskCache::Lookup(const std::string& url, LocalAttachment& out) {
    if (url.empty() || userDataDir_.empty()) {
        return false;
    }
    try {
        const std::string key = EntryKeyForUrl(url);
        std::lock_guard<std::mutex> lock(mutex_);
        std::string metaPath;
        std::string metaJson;
        EntryMeta meta;
        if (!ResolveEntryPath(key + "/" + kEntryMetaFileName, metaPath) ||
            !ReadSmallFile(metaPath, kMaxEntryMetaBytes, metaJson) || !ParseEntryMeta(metaJson, meta) ||
            meta.Url != url) {
            return false;
        }
        std::string dataPath;
        std::error_code ec;
        if (!ResolveEntryPath(key + "/" + meta.FileName, dataPath) || !fs::is_regular_file(fs::path(dataPath), ec)) {
            return false;
        }
        // Mark it used (LRU is by modification time); a failure only makes it evict a little sooner.
        fs::last_write_time(fs::path(dataPath), fs::file_time_type::clock::now(), ec);
        out.FilePath = dataPath;
        out.Mime = meta.Mime.empty() ? std::string(kDefaultMime) : meta.Mime;
        return true;
    } catch (const std::exception& ex) {
        LOG_WARN("AttachmentDiskCache::Lookup failed: %s", ex.what());
        return false;
    }
}

Result<LocalAttachment, TrackerError> AttachmentDiskCache::Store(const std::string& url, const std::string& fileName,
                                                                 const FetchedAttachment& fetched) {
    using StoreResult = Result<LocalAttachment, TrackerError>;
    if (url.empty() || userDataDir_.empty()) {
        return StoreResult::Err(TrackerErrorUnknown("The attachment cache is not available."));
    }
    try {
        const std::string mime = fetched.Mime.empty() ? std::string(kDefaultMime) : fetched.Mime;
        EntryMeta meta;
        meta.Url = url;
        meta.Mime = mime;
        meta.FileName = StoredFileName(fileName, ExtensionFromMime(mime));
        const std::string metaJson = SerializeEntryMeta(meta);
        const std::string key = EntryKeyForUrl(url);
        std::string dataPath;
        std::string metaPath;
        if (metaJson.empty() || !ResolveEntryPath(key + "/" + meta.FileName, dataPath) ||
            !ResolveEntryPath(key + "/" + kEntryMetaFileName, metaPath)) {
            return StoreResult::Err(TrackerErrorUnknown("The attachment could not be saved to the cache."));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        std::error_code ec;
        fs::create_directories(fs::path(dataPath).parent_path(), ec);
        std::string writeErr;
        // The file first, entry.json last: an entry without entry.json never counts as a hit.
        if (ec || !WriteCacheFile(dataPath, fetched.Bytes, true, writeErr) ||
            !WriteCacheFile(metaPath, metaJson, false, writeErr)) {
            return StoreResult::Err(TrackerErrorUnknown("The attachment could not be saved to the cache: " +
                                                        (ec ? ec.message() : writeErr)));
        }
        LocalAttachment stored;
        stored.FilePath = dataPath;
        stored.Mime = mime;
        return StoreResult::Ok(std::move(stored));
    } catch (const std::exception& ex) {
        return StoreResult::Err(
            TrackerErrorUnknown(std::string("The attachment could not be saved to the cache: ") + ex.what()));
    }
}

void AttachmentDiskCache::Prune() {
    if (userDataDir_.empty()) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const fs::path root = fs::path(userDataDir_) / kCacheSubdir;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            return;
        }
        const std::int64_t now = NowEpochSeconds();
        std::vector<CacheEntryInfo> entries;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code dirEc;
            const std::string name = it->path().filename().string();
            // Only entry directories are ever deleted; anything else in the folder is left alone.
            if (it->is_directory(dirEc) && !dirEc && IsEntryKey(name)) {
                entries.push_back(DescribeEntry(it->path(), name, now, graceSeconds_));
            }
        }
        // Orphaned temps first (an evicted entry goes whole, but sweeping it first costs nothing and keeps the
        // order simple); their size is already outside each entry's total.
        for (const CacheEntryInfo& entry : entries) {
            if (entry.StaleFiles.empty()) {
                continue;
            }
            // Removing a file bumps the directory's mtime, the LRU age of an entry holding no files. Put it
            // back, so a sweep never makes an old entry look recently used.
            const fs::path entryDir = root / entry.Key;
            std::error_code timeEc;
            const fs::file_time_type dirTime = fs::last_write_time(entryDir, timeEc);
            for (const std::string& staleName : entry.StaleFiles) {
                std::error_code rmEc;
                fs::remove(entryDir / staleName, rmEc);
                if (rmEc) {
                    LOG_WARN("AttachmentDiskCache::Prune: could not delete stale temp %s/%s: %s", entry.Key.c_str(),
                             staleName.c_str(), rmEc.message().c_str());
                }
            }
            if (!timeEc) {
                fs::last_write_time(entryDir, dirTime, timeEc);
            }
        }
        for (const std::string& key : SelectEvictions(std::move(entries), capBytes_, now, graceSeconds_)) {
            std::error_code rmEc;
            fs::remove_all(root / key, rmEc);
            if (rmEc) {
                LOG_WARN("AttachmentDiskCache::Prune: could not delete entry %s: %s", key.c_str(),
                         rmEc.message().c_str());
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("AttachmentDiskCache::Prune failed: %s", ex.what());
    }
}

Result<LocalAttachment, TrackerError> ResolveAttachment(AttachmentDiskCache& cache, const AttachmentRequest& request,
                                                        const AttachmentFetchFn& fetch,
                                                        const UncachedWriteFn& writeUncached) {
    using ResolveResult = Result<LocalAttachment, TrackerError>;
    LocalAttachment hit;
    if (cache.Lookup(request.Url, hit)) {
        return ResolveResult::Ok(std::move(hit));
    }
    if (request.Offline) {
        return ResolveResult::Err(TrackerErrorTransport(request.OfflineMissMessage));
    }
    if (!fetch || !writeUncached) {
        return ResolveResult::Err(TrackerErrorInvalidRequest("No attachment downloader is configured."));
    }
    Result<FetchedAttachment, TrackerError> fetched =
        Result<FetchedAttachment, TrackerError>::Err(TrackerErrorUnknown("The attachment download failed."));
    try {
        fetched = fetch();
    } catch (const std::exception& ex) {
        return ResolveResult::Err(TrackerErrorUnknown(std::string("The attachment download failed: ") + ex.what()));
    }
    if (!fetched.has_value()) {
        if (fetched.error().Kind == TrackerErrorKind::Transport) {
            LOG_WARN("ResolveAttachment: the tracker could not be reached: %s", fetched.error().Detail.c_str());
            return ResolveResult::Err(TrackerErrorTransport(request.OfflineMissMessage));
        }
        return ResolveResult::Err(fetched.error());
    }
    ResolveResult stored = cache.Store(request.Url, request.FileName, fetched.value());
    if (stored.has_value()) {
        cache.Prune();
        return stored;
    }
    LOG_WARN("ResolveAttachment: %s; opening an uncached copy.", stored.error().Detail.c_str());
    return writeUncached(fetched.value());
}

} // namespace attachments
} // namespace smatchet
