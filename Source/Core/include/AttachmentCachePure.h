#pragma once

// AttachmentCachePure — the decisions behind the attachment disk cache (Quality Pillar 6): where an
// attachment is stored, which entries to evict, and the entry metadata codec. Pure (no I/O, no Logger.h)
// so every rule is unit-tested bare; AttachmentDiskCache does the file work.
//
// Layout: <user data>/attachment_cache/<sha-256 of the URL>/<sanitized file name> plus entry.json. One
// directory per attachment URL; the URL names the tracker host, so two sites never share an entry.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace attachments {

constexpr const char* kCacheSubdir = "attachment_cache";
constexpr const char* kEntryMetaFileName = "entry.json";
/// Total size the cache is pruned back to after each download.
constexpr std::uint64_t kCacheCapBytes = 256ull * 1024ull * 1024ull;
/// An entry used within this window is never evicted: a preview window may still be reading the file.
constexpr std::int64_t kEvictionGraceSeconds = 10 * 60;
/// Longest stored file name, in bytes (the extension is kept when a longer name is shortened).
constexpr std::size_t kMaxFileNameBytes = 120;

/// Lowercase-hex SHA-256 of `url`: the entry directory name.
std::string EntryKeyForUrl(const std::string& url);

/// True when `name` has the shape EntryKeyForUrl produces (64 lowercase hex digits). Pruning only ever
/// deletes directories with this shape.
bool IsEntryKey(const std::string& name);

/// A file name that is safe as one path component on Windows, macOS and Linux: path separators and
/// reserved characters become '_', control characters are dropped, leading dots and spaces and trailing
/// dots and spaces are removed, a reserved device name (CON, NUL, COM1, ...) gets a '_' prefix, and the
/// result is at most kMaxFileNameBytes (cut on a UTF-8 boundary, extension kept). Never empty.
std::string SanitizeFileName(const std::string& name);

/// The sanitized `filename`, with `extension` (".png") appended when the name has none.
std::string StoredFileName(const std::string& filename, const std::string& extension);

struct CacheEntryInfo {
    std::string Key;
    std::uint64_t SizeBytes = 0;
    std::int64_t LastUsedEpochSec = 0;
    bool Complete = true; ///< false when entry.json is missing: an interrupted download
};

/// The entry keys to delete, least recently used first. An interrupted entry older than `graceSeconds`
/// is always deleted. The rest are deleted oldest first until the total fits `capBytes`, except that an
/// entry used within `graceSeconds` of `nowEpochSec` is never deleted (the cache may then stay over its
/// cap until those entries age).
std::vector<std::string> SelectEvictions(std::vector<CacheEntryInfo> entries, std::uint64_t capBytes,
                                         std::int64_t nowEpochSec, std::int64_t graceSeconds);

/// What entry.json records: the URL the file came from (a lookup must match it exactly), its MIME type,
/// and the stored file name.
struct EntryMeta {
    std::string Url;
    std::string Mime;
    std::string FileName;
};

/// "" when the metadata cannot be serialized.
std::string SerializeEntryMeta(const EntryMeta& meta);

/// False on malformed JSON, an empty URL, or a file name that is not already a sanitized single path
/// component (a tampered entry.json can never point outside its entry directory).
bool ParseEntryMeta(const std::string& json, EntryMeta& out);

} // namespace attachments
} // namespace smatchet
