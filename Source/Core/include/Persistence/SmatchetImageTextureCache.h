#pragma once

#include "SmatchetResult.h"
#include "imgui.h"

#include <cstdint>
#include <string>
#include <vector>

struct ImTextureData;

/** LRU GPU texture cache for small PNG/JPEG icons (grid cells, Lua `imgui.image`). */
struct SmatchetLoadedIconTexture {
    ImTextureData* Texture = nullptr;
    int Width = 0;
    int Height = 0;
};

namespace SmatchetImageTextureCache {

/** Call once per UI frame (before/after draw) to finalize destroyed textures. */
void TickPendingDestroys();

/**
 * Cache-only lookup: returns true and populates `out` on hit (and touches LRU). Performs no I/O.
 * Intended as a fast path for hot per-frame call sites (grid icon cells) so they skip disk reads
 * and decode work when the texture is already resident.
 */
bool TryGetCached(const std::string& cacheKey, SmatchetLoadedIconTexture& out);

/**
 * Decode image bytes (PNG/JPEG/WebP/etc. via stb) and return a registered ImTextureData.
 * @param cacheKey stable key (e.g. `file:` + path or `url:` + url) for LRU.
 */
// Ok(texture) on a decode+upload success (or an LRU cache hit); Err(reason) on empty
// input, a renderer without texture support, a decode failure, or a GPU-upload failure.
Result<SmatchetLoadedIconTexture> GetOrLoadFromMemory(const std::string& cacheKey, const unsigned char* bytes,
                                                      size_t byteCount);

Result<SmatchetLoadedIconTexture> GetOrLoadFromFile(const std::string& cacheKey, const std::string& absolutePath);

/**
 * Upload already-decoded RGBA32 pixels (tightly packed, top-down, `width * height * 4` bytes).
 * Same cache/LRU/eviction contract as GetOrLoadFromMemory, minus the stb decode step — for images
 * stb rejects, such as the baked-in 32bpp BMP-in-ICO app icon that SmatchetIcoDecode unpacks.
 * Err(reason) on an empty key, a short pixel buffer, non-positive dimensions, a renderer without
 * texture support, or a GPU-upload failure.
 */
Result<SmatchetLoadedIconTexture> GetOrCreateFromRgba(const std::string& cacheKey,
                                                      const std::vector<unsigned char>& rgba, int width, int height);

/** Snapshot-only memory gauges for `perf.memory` (docs/plans/shipped/memory-budget-and-lifetime-hardening.md § S3).
 *  Both take the cache's `g_mutex`; cheap (O(1) and O(entries) respectively). */
std::size_t IconCacheEntryCount();
/** Approximate resident GPU bytes: Σ Width·Height·4 over cached entries. An estimate
 *  (ignores driver padding / mips) — a gauge, not authoritative RSS. */
std::size_t IconCacheApproxBytes();

} // namespace SmatchetImageTextureCache
