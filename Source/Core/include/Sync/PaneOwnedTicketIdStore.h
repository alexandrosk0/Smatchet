#pragma once

// PaneOwnedTicketIdStore — the per-pane owned-ticket-id book-keeping behind AppController's
// multi-pane cache scoping (pane-stale-delete-collision F4). The SQLite cache is ONE
// backend-keyed namespace shared by every pane (ADR-0018 decision 4), so a namespace-wide
// `GetAllTickets(backendKey)` has to be filtered back down to the ids the asking pane's own sync
// recorded, and a pane's rows must survive a sibling pane's stale-deletion sweep. Extracted out of
// AppController so those two invariants — plus the three states an entry can be in (never
// recorded / recorded / forgotten) — are unit-testable without constructing the controller.
//
// Entry states, which the callers MUST keep distinct (issue #2063):
//   * absent   — this pane has never completed a sync. The whole namespace is the best available
//                seed, so the caller falls back to an unfiltered read (the bootstrap refresh).
//   * present  — the pane's last sync recorded exactly these ids; filter the read to them. An
//                EMPTY present entry is a tombstone (the pane was retired and revived) and means
//                "render nothing yet", NOT "fall back to the sibling union".
// The store's own mutex is the INNERMOST lock in AppController's hierarchy — no other lock may be
// taken while it is held, which is why every method here is self-contained.

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace smatchet {

class PaneOwnedTicketIdStore {
  public:
    /// Composite map key. '\n' cannot occur in a backend key (a tracker-type token) or a pane id
    /// (a ConfigManager_Panes identifier), so the join is unambiguous in both directions — it also
    /// keeps the map ordered by backend key first, which is what lets the retention sweep
    /// prefix-scan one namespace with lower_bound instead of walking every pane in the process.
    static std::string MakeKey(const std::string& backendKey, const std::string& paneId) {
        return backendKey + '\n' + paneId;
    }

    /// Record the ids a pane's completed sync kept (replaces any previous set).
    void Set(const std::string& backendKey, const std::string& paneId, const std::vector<std::string>& ids) {
        if (paneId.empty()) {
            return; // context not registered in gridContexts_ yet — nothing to key on
        }
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[MakeKey(backendKey, paneId)] = ids;
    }

    /// Read a pane's recorded set. Returns false when the pane has NEVER recorded one (absent) —
    /// distinct from a present-but-empty tombstone, which returns true with an empty `outIds`.
    bool TryGet(const std::string& backendKey, const std::string& paneId, std::vector<std::string>& outIds) const {
        outIds.clear();
        if (paneId.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, std::vector<std::string>>::const_iterator it = entries_.find(MakeKey(backendKey, paneId));
        if (it == entries_.end()) {
            return false;
        }
        outIds = it->second;
        return true;
    }

    /// Append ONE id to a pane's recorded set, atomically (issue #2050): the entry is re-read
    /// under the lock, so a `Set` publishing a fresh sync roster into the window between a
    /// caller's read and this write is extended, never clobbered. An absent entry is left absent
    /// (a pane that has never synced has no set to admit into) — returns false in that case, and
    /// false too when the id was already recorded.
    bool Add(const std::string& backendKey, const std::string& paneId, const std::string& id) {
        if (paneId.empty() || id.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, std::vector<std::string>>::iterator it = entries_.find(MakeKey(backendKey, paneId));
        if (it == entries_.end()) {
            return false;
        }
        if (std::find(it->second.begin(), it->second.end(), id) != it->second.end()) {
            return false;
        }
        it->second.push_back(id);
        return true;
    }

    /// Subtract specific ids from one pane's recorded set without disturbing the rest — used when
    /// a 404 reconcile purges cache rows, so the set stops whitelisting (and pinning) dead ids.
    void Drop(const std::string& backendKey, const std::string& paneId, const std::vector<std::string>& ids) {
        if (paneId.empty() || ids.empty()) {
            return;
        }
        const std::set<std::string> drop(ids.begin(), ids.end());
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, std::vector<std::string>>::iterator it = entries_.find(MakeKey(backendKey, paneId));
        if (it == entries_.end()) {
            return;
        }
        std::vector<std::string>& owned = it->second;
        owned.erase(std::remove_if(owned.begin(), owned.end(),
                                   [&drop](const std::string& id) { return drop.find(id) != drop.end(); }),
                    owned.end());
    }

    /// Retirement: the pane keeps nothing pinned, but is NOT forgotten. Keyed by pane id ACROSS
    /// every backend key (issue #2049): a pane that switched tracker kind filed its set under the
    /// key that was live when its sync published, and erasing only the key that happens to be live
    /// at retirement leaves an orphan that pins tickets_v2 rows against every sibling's stale
    /// sweep forever. Each match is emptied rather than erased so the entry stays a tombstone
    /// (issue #2063) — a revived pane must render empty until its own sync lands, not inherit the
    /// namespace-wide cold-start fallback and re-leak every sibling pane's rows into its grid.
    void Forget(const std::string& paneId) {
        if (paneId.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::map<std::string, std::vector<std::string>>::iterator it = entries_.begin(); it != entries_.end();
             ++it) {
            if (KeyBelongsToPane(it->first, paneId)) {
                std::vector<std::string>().swap(it->second); // tombstone: present, pins nothing
            }
        }
    }

    /// Erase every trace of `paneId`, tombstone included. This is NOT the retirement path — that
    /// is `Forget`, which deliberately leaves a tombstone so a revived pane renders empty instead
    /// of inheriting the namespace-wide cold-start seed.
    /// This is the pane-CREATION path, and it exists because pane ids are RECYCLED:
    /// `GenerateUniquePaneId` walks `pane-2, pane-3, …` and hands back the lowest unused id, so
    /// closing `pane-2` and clicking "+" mints `pane-2` again. Without this, the dead pane's
    /// tombstone would shadow the brand-new pane — `TryGet` would answer "recorded, empty", the
    /// new pane would be denied its cold-start seed, and offline (or against a down tracker, or
    /// across a backend swap that discards the kick) nothing would ever clear it, because a
    /// non-empty completed full sync is the only writer that does. A new pane must start with no
    /// history at all, which is exactly what erasing gives it.
    void Erase(const std::string& paneId) {
        if (paneId.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::map<std::string, std::vector<std::string>>::iterator it = entries_.begin(); it != entries_.end();) {
            if (KeyBelongsToPane(it->first, paneId)) {
                entries_.erase(it++);
            } else {
                ++it;
            }
        }
    }

    /// Union of the ids recorded by every OTHER pane in `selfBackendKey`'s namespace. Backs the
    /// retention half of a full sync's stale-deletion: the recorded sets outlive the hidden-pane
    /// LRU swapping a pane's in-memory roster away, so relying on live rosters alone would let one
    /// pane's sync delete exactly the rows another is about to re-seed from.
    std::vector<std::string> CollectRetainedByOtherPanes(const std::string& selfBackendKey,
                                                         const std::string& selfPaneId) const {
        const std::string prefix = MakeKey(selfBackendKey, std::string());
        std::vector<std::string> ids;
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::map<std::string, std::vector<std::string>>::const_iterator it = entries_.lower_bound(prefix);
             it != entries_.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
            if (it->first.compare(prefix.size(), std::string::npos, selfPaneId) == 0) {
                continue; // this pane's own set — never self-retaining
            }
            ids.insert(ids.end(), it->second.begin(), it->second.end());
        }
        return ids;
    }

  private:
    /// True when `key` is `<any backend key>\n<paneId>`. Exact-suffix (not a plain `ends_with`):
    /// the '\n' separator must be present, or pane "b" would match pane "ab"'s entry.
    static bool KeyBelongsToPane(const std::string& key, const std::string& paneId) {
        if (key.size() < paneId.size() + 1) {
            return false;
        }
        const std::size_t start = key.size() - paneId.size();
        return key[start - 1] == '\n' && key.compare(start, std::string::npos, paneId) == 0;
    }

    /// Guards `entries_` only. Written on the UI thread (sync finalize / retirement), read from
    /// the UI thread AND from the worker-invoked refresh path. INNERMOST — never take another
    /// lock while holding it.
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<std::string>> entries_;
};

} // namespace smatchet
