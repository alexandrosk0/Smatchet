#pragma once

#include "ILookupCache.h"

#include <map>
#include <mutex>
#include <string>

// In-memory ILookupCache for testing. Keyed by backend + '\x1f' + kind + '\x1f' + key.
class FakeLookupCache : public ILookupCache {
  public:
    FakeLookupCache() = default;

    bool UpsertLookup(const std::string& backendKey, const std::string& kind,
                      const std::string& cacheKey, const std::string& payloadJson) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = backendKey + '\x1f' + kind + '\x1f' + cacheKey;
        data_[key] = {cacheKey, payloadJson, 0};
        ++UpsertCalls;
        return true;
    }

    bool TryGetLookup(const std::string& backendKey, const std::string& kind,
                      const std::string& cacheKey, LookupCacheRow& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = backendKey + '\x1f' + kind + '\x1f' + cacheKey;
        const auto it = data_.find(key);
        if (it != data_.end()) {
            out = it->second;
            return true;
        }
        return false;
    }

    std::vector<LookupCacheRow> LoadLookups(const std::string& backendKey,
                                            const std::string& kind) override {
        std::vector<LookupCacheRow> result;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string prefix = backendKey + '\x1f' + kind + '\x1f';
        for (const auto& kv : data_) {
            if (kv.first.find(prefix) == 0) {
                result.push_back(kv.second);
            }
        }
        return result;
    }

    bool DeleteLookup(const std::string& backendKey, const std::string& kind,
                      const std::string& cacheKey) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = backendKey + '\x1f' + kind + '\x1f' + cacheKey;
        data_.erase(key);
        return true;
    }

    int UpsertCalls = 0;

  private:
    std::mutex mutex_;
    std::map<std::string, LookupCacheRow> data_;
};
