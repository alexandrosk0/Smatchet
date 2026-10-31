#pragma once

// LocalRefreshGatePure - the ordering gate for a pane's reads of the local cache. A refresh reads the whole
// namespace on a worker and swaps it into the pane; two refreshes in flight (an offline replay, a queued save, the
// sync.refresh_local command) can finish in the other order than they started, and the older read would then
// replace the newer one's rows. Header-only, C++14, no platform headers.

#include "CachedTicketTypes.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace smatchet {
namespace sync {

class LocalRefreshGate {
  public:
    /// Takes the next ticket. Call BEFORE the cache read starts: the ticket orders the reads by start time.
    std::uint64_t Begin() { return issued_.fetch_add(1) + 1; }

    /// True when the read holding `ticket` may replace the roster wholesale: no read that started later has landed
    /// yet. Call under the roster's lock, in the same scope as the replace.
    bool ClaimWholesale(std::uint64_t ticket) {
        if (ticket < applied_) {
            return false;
        }
        applied_ = ticket;
        return true;
    }

  private:
    std::atomic<std::uint64_t> issued_{0};
    std::uint64_t applied_ = 0; // guarded by the caller's roster lock
};

/// A refresh that lost the race still owes the roster the row its caller just saved (`admitId`): copies that row from
/// `read` into `active` when the newer read left it out. Returns whether the roster changed.
inline bool AdmitRowIfMissing(std::vector<CachedTicket>& active, const std::vector<CachedTicket>& read,
                              const std::string& admitId) {
    if (admitId.empty()) {
        return false;
    }
    for (const CachedTicket& row : active) {
        if (row.id == admitId) {
            return false;
        }
    }
    for (const CachedTicket& row : read) {
        if (row.id == admitId) {
            active.push_back(row);
            return true;
        }
    }
    return false;
}

} // namespace sync
} // namespace smatchet
