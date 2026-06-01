#pragma once

// CacheEvictionPolicy — the pure entry-count + aggregate-byte cap decision shared by the icon
// LRU cache in SmatchetImageTextureCache and the AI plan FIFO cache in SmatchetAiAssistantUi.
// Plan: docs/plans/shipped/memory-budget-and-lifetime-hardening.md § Phase 4. Pure so it is
// unit-testable without a GPU context or an ImGui frame.

#include <cstddef>

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

} // namespace smatchet
