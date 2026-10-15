#pragma once

// RefusalIsolatingFetchPure - the key-batch download that survives one unreadable ticket. Shared by the
// delta sync's hierarchy / delta hops and the change monitor's roster body fetch
// (docs/plans/active/delta-sync.md § Phase 3, § Phase 6). Header-only, C++14, no platform headers.

#include "CachedTicketTypes.h"
#include "SmatchetResult.h"
// SMATCHET_DEVIATION(rule=include-cycle; reason=TrackerError is the plain error value the pure fetch helper returns inside Result<>; relocating it to a lower-layer leaf is a repo-wide layering change owned by the core-include-dag plan; owner=offline-sync; revisit=2027-10-14)
#include "TrackerError.h"

#include <cstddef>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace sync {

using KeyBatchFetch = std::function<Result<std::vector<CachedTicket>, TrackerError>(const std::vector<std::string>&)>;

/// Refused keys one hop may isolate before it gives up and keeps the cached rows (see
/// FetchKeysIsolatingRefusals).
constexpr int kMaxIsolatedRefusalsPerHop = 4;

/// Extra requests a fetch over `keyCount` keys may spend isolating refusals. Each split costs two requests,
/// and isolating k refused keys among n takes at most k * ceil(log2 n) splits, so this covers
/// kMaxIsolatedRefusalsPerHop refused keys wherever they sit in the batch.
inline int RefusalSplitBudget(std::size_t keyCount) {
    int depth = 0;
    for (std::size_t span = 1; span < keyCount; span *= 2) {
        ++depth;
    }
    return 2 * depth * kMaxIsolatedRefusalsPerHop;
}

/// A refusal says the tracker will not serve those keys (400 on a `key in (...)` batch, or 404).
inline bool IsTrackerRefusal(const TrackerError& error) {
    return error.Kind == TrackerErrorKind::InvalidRequest || error.Kind == TrackerErrorKind::NotFound;
}

/// Fetches `keys`. A refusal of a multi-key request says nothing about the individual keys: Jira rejects
/// a whole `key in (...)` batch with a 400 when ONE key does not exist or cannot be browsed. So the halves
/// are retried until the refused keys are isolated; a single refused key is a definitive answer about that
/// ticket and goes into `refusedKeys`, and the rest load normally. Any other failure, or a refusal once
/// `budget` extra requests are spent, is returned as an error (the caller then keeps the cached rows).
/// Once `cancelled` reports true no further request is sent and a Cancelled error is returned.
inline Result<std::vector<CachedTicket>, TrackerError>
FetchKeysIsolatingRefusals(const KeyBatchFetch& fetch, const std::function<bool()>& cancelled,
                           const std::vector<std::string>& keys, int& budget, std::vector<std::string>& refusedKeys) {
    using R = Result<std::vector<CachedTicket>, TrackerError>;
    if (cancelled()) {
        return R::Err(TrackerErrorCancelled());
    }
    R fetched = fetch(keys);
    if (fetched.has_value() || !IsTrackerRefusal(fetched.error())) {
        return fetched;
    }
    if (keys.size() == 1) {
        refusedKeys.push_back(keys.front());
        return R::Ok(std::vector<CachedTicket>());
    }
    if (budget < 2) {
        return fetched;
    }
    budget -= 2;
    const auto mid = keys.begin() + static_cast<std::ptrdiff_t>(keys.size() / 2);
    R first =
        FetchKeysIsolatingRefusals(fetch, cancelled, std::vector<std::string>(keys.begin(), mid), budget, refusedKeys);
    if (!first.has_value()) {
        return first;
    }
    R second =
        FetchKeysIsolatingRefusals(fetch, cancelled, std::vector<std::string>(mid, keys.end()), budget, refusedKeys);
    if (!second.has_value()) {
        return second;
    }
    std::vector<CachedTicket> merged = std::move(first.value());
    for (CachedTicket& ticket : second.value()) {
        merged.push_back(std::move(ticket));
    }
    return R::Ok(std::move(merged));
}

} // namespace sync
} // namespace smatchet
