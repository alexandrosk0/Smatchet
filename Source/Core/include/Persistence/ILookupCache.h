#pragma once

// ILookupCache — persisted read-side lookups for offline use (Quality Pillar 6): the workflow
// remembered from earlier online use, and later other tracker lookups. A row is keyed by backend
// namespace + kind + cache key and holds one JSON payload. SQLite-free so services and their fakes
// link it bare. Implementations are thread-safe; call only from workers (the reads and writes hit disk).

#include <cstdint>
#include <string>
#include <vector>

struct LookupCacheRow {
    std::string CacheKey;
    std::string PayloadJson;
    std::int64_t UpdatedAtEpochSec = 0;
};

class ILookupCache {
  public:
    virtual ~ILookupCache() = default;
    /// Insert or replace one row. False when the write failed (logged by the implementation).
    virtual bool UpsertLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey,
                              const std::string& payloadJson) = 0;
    /// True and `out` filled when the row exists.
    virtual bool TryGetLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey,
                              LookupCacheRow& out) = 0;
    /// Every row of one kind for one backend; empty on a read failure.
    virtual std::vector<LookupCacheRow> LoadLookups(const std::string& backendKey, const std::string& kind) = 0;
    /// Remove one row. True when the statement ran (whether or not a row existed).
    virtual bool DeleteLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey) = 0;
};
