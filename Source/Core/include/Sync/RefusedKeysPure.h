#pragma once

// The keys a tracker refused to serve on a delta attempt (docs/plans/active/delta-sync.md, Phase 3). A key the
// roster lists but whose body request is refused sends the attempt down the full-fetch fallback; remembering the
// key keeps every later attempt from doing the same. The memory must not grow without bound and must not outlive
// the reason for it, so it is bounded (the oldest key is dropped past a cap), it expires (a refusal is retried
// after a while, the same age the cached rows go stale at), and a key is forgotten as soon as a fetch serves its
// body at the version it was refused for (or a newer one). Header-only, no I/O; the tracker's stamps are only
// compared through RemoteVersionPure.h.

#include "Sync/RemoteVersionPure.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace smatchet {
namespace sync {

class RefusedKeySet {
  public:
    /// Most keys remembered at once; past it the oldest go. A function, not a `static constexpr` member: at C++14
    /// such a member needs an out-of-line definition once it is bound by reference.
    static std::size_t MaxKeys() { return 256; }

    bool Empty() const { return entries_.empty(); }
    std::size_t Size() const { return entries_.size(); }
    bool Contains(const std::string& key) const { return entries_.find(key) != entries_.end(); }

    /// True when the download of `key` should be skipped: it is remembered AND the roster does not list a provably
    /// newer version than the one it was refused for. A ticket edited since the refusal may well be servable now, so
    /// it is asked for again (an unparseable or missing stamp proves nothing: the refusal stands).
    bool ShouldSkip(const std::string& key, const std::string& rosterVersion) const {
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return false;
        }
        return CompareRemoteUpdated(rosterVersion, it->second.Version) != RemoteVersionOrder::Newer;
    }

    /// Remembers that `key` was refused at tracker version `version` (the roster's stamp; may be empty) at `nowSec`.
    /// A key already held is refreshed. Past MaxKeys() the oldest keys are dropped.
    void Insert(const std::string& key, const std::string& version, std::int64_t nowSec) {
        Entry& entry = entries_[key];
        entry.Version = version;
        entry.AtSec = nowSec;
        entry.Seq = ++nextSeq_;
        while (entries_.size() > MaxKeys()) {
            DropOldest();
        }
    }

    /// A fetch served `key`'s body, stamped `bodyUpdated`. The refusal is forgotten when that is the version the key
    /// was refused for or provably a newer one; an older body (the streamed fetch lagging behind the roster) proves
    /// nothing about the keyed request, so the key stays remembered. True when the key was forgotten.
    bool NoteServed(const std::string& key, const std::string& bodyUpdated) {
        const auto it = entries_.find(key);
        if (it == entries_.end()) {
            return false;
        }
        const std::string& refusedAt = it->second.Version;
        if (IsSameRemoteVersion(bodyUpdated, refusedAt) ||
            CompareRemoteUpdated(bodyUpdated, refusedAt) == RemoteVersionOrder::Newer) {
            entries_.erase(it);
            return true;
        }
        return false;
    }

    /// Forgets the keys remembered for more than `maxAgeSec` (so the next attempt asks again). `maxAgeSec` <= 0
    /// means they never expire. Returns how many were dropped.
    std::size_t ExpireOlderThan(std::int64_t nowSec, std::int64_t maxAgeSec) {
        if (maxAgeSec <= 0) {
            return 0;
        }
        std::size_t dropped = 0;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (nowSec - it->second.AtSec > maxAgeSec) {
                it = entries_.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        return dropped;
    }

    void Clear() { entries_.clear(); }

  private:
    struct Entry {
        std::string Version;
        std::int64_t AtSec = 0;
        std::uint64_t Seq = 0; ///< insertion order: the smallest is the oldest
    };

    void DropOldest() {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.Seq < oldest->second.Seq) {
                oldest = it;
            }
        }
        entries_.erase(oldest);
    }

    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t nextSeq_ = 0;
};

} // namespace sync
} // namespace smatchet
