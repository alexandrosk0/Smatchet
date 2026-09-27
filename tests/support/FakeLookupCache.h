#pragma once

// FakeLookupCache — in-memory ILookupCache for SQLite-free service tests (the ADR-0020 pure-sync set
// bans LocalCacheManager). Rows are keyed backend + '\x1f' + kind + '\x1f' + cache key; a mutex makes
// it safe for the real-thread (TSan) cases. UpsertCalls counts every upsert.

#include "ILookupCache.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class FakeLookupCache : public ILookupCache {
  public:
    bool UpsertLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey,
                      const std::string& payloadJson) override {
        std::lock_guard<std::mutex> lock(mutex_);
        LookupCacheRow row;
        row.CacheKey = cacheKey;
        row.PayloadJson = payloadJson;
        data_[RowKey(backendKey, kind, cacheKey)] = row;
        ++UpsertCalls;
        return true;
    }

    bool TryGetLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey,
                      LookupCacheRow& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = data_.find(RowKey(backendKey, kind, cacheKey));
        if (it == data_.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    std::vector<LookupCacheRow> LoadLookups(const std::string& backendKey, const std::string& kind) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string prefix = backendKey + '\x1f' + kind + '\x1f';
        std::vector<LookupCacheRow> rows;
        for (const auto& kv : data_) {
            if (kv.first.compare(0, prefix.size(), prefix) == 0) {
                rows.push_back(kv.second);
            }
        }
        return rows;
    }

    bool DeleteLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey) override {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.erase(RowKey(backendKey, kind, cacheKey));
        return true;
    }

    std::atomic<int> UpsertCalls{0};

  private:
    static std::string RowKey(const std::string& backendKey, const std::string& kind, const std::string& cacheKey) {
        return backendKey + '\x1f' + kind + '\x1f' + cacheKey;
    }

    std::mutex mutex_;
    std::map<std::string, LookupCacheRow> data_;
};
