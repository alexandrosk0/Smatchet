#include "AttachmentDiskCache.h"

#include "AttachmentMimeUtils.h"
#include "Commands/PathConfinement.h"
#include "Logger.h"

#include <ghc/filesystem.hpp>

#include <atomic>
#include <chrono>
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

constexpr std::uintmax_t kMaxEntryMetaBytes = 16u * 1024u;
constexpr const char* kDefaultMime = "application/octet-stream";

std::int64_t ToEpochSeconds(const fs::file_time_type& t) {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

std::int64_t NowEpochSeconds() { return ToEpochSeconds(fs::file_time_type::clock::now()); }

// A sibling temp name no other writer (thread or process) picks: time, thread and a counter.
std::string UniqueTempSuffix() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t tid = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return ".part-" + std::to_string(now ^ (tid << 16)) + "-" + std::to_string(counter.fetch_add(1));
}

// Temp file then rename, so a reader never sees a half-written file. ghc streams take UTF-8 paths on
// Windows too.
bool WriteFileAtomically(const std::string& path, const std::string& bytes, std::string& errOut) {
    const std::string temp = path + UniqueTempSuffix();
    {
        fs::ofstream out(fs::path(temp), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            errOut = "could not create the cache file";
            return false;
        }
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::error_code rmEc;
            fs::remove(fs::path(temp), rmEc);
            errOut = "could not write the cache file";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(fs::path(temp), fs::path(path), ec);
    if (!ec) {
        return true;
    }
    std::error_code rmEc;
    fs::remove(fs::path(temp), rmEc);
    // Windows refuses to replace a file a viewer holds open. The entry's content never changes for a
    // URL, so an existing file of the same size is the same attachment.
    std::error_code sizeEc;
    if (fs::is_regular_file(fs::path(path), sizeEc) &&
        fs::file_size(fs::path(path), sizeEc) == static_cast<std::uintmax_t>(bytes.size()) && !sizeEc) {
        return true;
    }
    errOut = "could not move the cache file into place: " + ec.message();
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

// Size, last use and completeness of one entry directory (its files are never nested).
CacheEntryInfo DescribeEntry(const fs::path& dir, const std::string& key) {
    CacheEntryInfo info;
    info.Key = key;
    info.Complete = false;
    std::error_code ec;
    info.LastUsedEpochSec = ToEpochSeconds(fs::last_write_time(dir, ec));
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) {
            continue;
        }
        info.SizeBytes += static_cast<std::uint64_t>(it->file_size(fileEc));
        const std::int64_t used = ToEpochSeconds(it->last_write_time(fileEc));
        if (!fileEc && used > info.LastUsedEpochSec) {
            info.LastUsedEpochSec = used;
        }
        if (it->path().filename().string() == kEntryMetaFileName) {
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
        if (ec || !WriteFileAtomically(dataPath, fetched.Bytes, writeErr) ||
            !WriteFileAtomically(metaPath, metaJson, writeErr)) {
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
        std::vector<CacheEntryInfo> entries;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code dirEc;
            const std::string name = it->path().filename().string();
            // Only entry directories are ever deleted; anything else in the folder is left alone.
            if (it->is_directory(dirEc) && !dirEc && IsEntryKey(name)) {
                entries.push_back(DescribeEntry(it->path(), name));
            }
        }
        for (const std::string& key :
             SelectEvictions(std::move(entries), capBytes_, NowEpochSeconds(), graceSeconds_)) {
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
