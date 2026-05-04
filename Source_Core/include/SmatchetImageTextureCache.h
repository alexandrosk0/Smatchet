#pragma once

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
 * Decode image bytes (PNG/JPEG/WebP/etc. via stb) and return a registered ImTextureData.
 * @param cacheKey stable key (e.g. `file:` + path or `url:` + url) for LRU.
 */
bool GetOrLoadFromMemory(const std::string& cacheKey, const unsigned char* bytes, size_t byteCount,
                         SmatchetLoadedIconTexture& out, std::string& outError);

bool GetOrLoadFromFile(const std::string& cacheKey, const std::string& absolutePath, SmatchetLoadedIconTexture& out,
                       std::string& outError);

void EvictCacheKey(const std::string& cacheKey);

} // namespace SmatchetImageTextureCache
