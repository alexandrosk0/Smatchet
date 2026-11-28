#pragma once

// CacheEvictionPolicy — the pure entry-count + aggregate-byte cap decision shared by the icon
// LRU cache in SmatchetImageTextureCache and the AI plan FIFO cache in SmatchetAiAssistantUi.
// Plan: docs/plans/shipped/memory-budget-and-lifetime-hardening.md § Phase 4. Pure so it is
// unit-testable without a GPU context or an ImGui frame.

#include <cstddef>
#include <cstdint>

namespace smatchet {

/// True when a cache holding `count` entries totalling `bytes` is over either the count cap or
/// the byte cap and still has something to drop. False at `count == 0`, so a lone item bigger
/// than `maxBytes` is admitted instead of looping against an empty cache (the per-item read cap
/// already bounds it). Evict-before-insert callers pass post-insert figures: `count + 1`,
/// `bytes + incoming`. Each call site loops `while (CacheOverCap(...))` dropping its own victim.
inline bool CacheOverCap(std::size_t count, std::size_t bytes, std::size_t maxCount, std::size_t maxBytes) {
    if (count == 0) {
        return false;
    }
    return count > maxCount || bytes > maxBytes;
}

/// Hysteresis for a map that is swept rather than LRU-trimmed (the Lua field render cache, which is read on
/// every cell of every frame and so cannot afford per-hit list maintenance). A sweep is due only when the map
/// is over `cap` AND at least `minSpacingTicks` ticks (frames) passed since the last sweep, so a grid that
/// legitimately keeps more than `cap` cells live does not re-walk the whole map every insert. A tick counter
/// that went backwards counts as due.
inline bool SweepDue(std::size_t size, std::size_t cap, std::uint64_t nowTick, std::uint64_t lastSweepTick,
                     std::uint64_t minSpacingTicks) {
    if (size <= cap) {
        return false;
    }
    return nowTick < lastSweepTick || nowTick - lastSweepTick >= minSpacingTicks;
}

/// Erases every entry of `map` whose last-use tick (`lastUsedOf(value)`) is older than `cutoffTick`; returns
/// how many were erased. Entries used at or after the cutoff are kept.
template <class Map, class LastUsedFn>
std::size_t EraseUnusedSince(Map& map, std::uint64_t cutoffTick, LastUsedFn lastUsedOf) {
    std::size_t erased = 0;
    for (auto it = map.begin(); it != map.end();) {
        if (lastUsedOf(it->second) < cutoffTick) {
            it = map.erase(it);
            ++erased;
        } else {
            ++it;
        }
    }
    return erased;
}

} // namespace smatchet
