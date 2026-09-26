#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Persisted read-side lookups for offline use (Pillar 6). Rows are keyed by backend namespace + kind + key.
// Call only from workers.
struct LookupCacheRow {
    std::string CacheKey;
    std::string PayloadJson;
    std::int64_t UpdatedAtEpochSec = 0;
};

class ILookupCache {
  public:
    virtual ~ILookupCache() = default;
    virtual bool UpsertLookup(const std::string& backendKey, const std::string& kind,
                               const std::string& cacheKey, const std::string& payloadJson) = 0;
    virtual bool TryGetLookup(const std::string& backendKey, const std::string& kind,
                               const std::string& cacheKey, LookupCacheRow& out) = 0;
    virtual std::vector<LookupCacheRow> LoadLookups(const std::string& backendKey,
                                                     const std::string& kind) = 0;
    virtual bool DeleteLookup(const std::string& backendKey, const std::string& kind,
                               const std::string& cacheKey) = 0;
};
