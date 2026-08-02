#include "SmatchetImageTextureCache.h"

#include "CacheEvictionPolicy.h"
#include "Logger.h"
#include "Persistence/IconDimensionsPolicy.h"
#include "imgui_internal.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_STDIO
#include <stb/stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// Bumped from 96 → 384 to cover busy grids: a typical view shows 20+ rows × 4-6 icon-bearing
// columns (priority, status, type, assignee avatar, attachment) = 80-120 distinct textures;
// with the old cap, scrolling caused constant eviction → priority/status memo entries became
// stale → slow-path re-resolution every frame. Each entry holds an `ImTextureData*` whose
// pixel storage is ~16-64 KB; 384 × 64 KB ≈ 24 MB worst-case GPU memory — negligible.
constexpr size_t kMaxCacheEntries = 384;
constexpr size_t kMaxFileReadBytes = 4u * 1024u * 1024u;
constexpr int kMaxIconDimension = 512;
// Aggregate-pixel guard for the pre-decode dimension check: RGBA32 byte estimate at the
// dimension ceiling (512 x 512 x 4 = 1 MiB). With both side caps already at kMaxIconDimension
// this is the same bound, but it keeps the pre-check honest if the side cap ever grows faster
// than intended and guards against a pathological aspect ratio.
constexpr size_t kMaxIconPixelBytes =
    static_cast<size_t>(kMaxIconDimension) * static_cast<size_t>(kMaxIconDimension) * 4u;
// Aggregate gauge-byte cap (Phase 4). The 384-entry cap alone bounds memory only if every
// entry is small; 384 entries at the 512×512 dimension ceiling would be ~384 MB. This caps the
// summed Width·Height·4 estimate so a run of large-but-under-dimension icons can't balloon the
// resident set. Eviction stays LRU-tail; this just adds a second condition to the evict loop.
constexpr size_t kMaxCacheBytes = 96u * 1024u * 1024u;

struct CacheValue {
    ImTextureData* Texture = nullptr;
    int Width = 0;
    int Height = 0;
    std::list<std::string>::iterator LruIt; ///< O(1) splice via stored iterator (§4.8 item 47).
};

static std::mutex g_mutex;
static std::list<std::string> g_lru;
static std::unordered_map<std::string, CacheValue> g_map;
// Running sum of EntryBytes over g_map, maintained on every insert/evict/erase under g_mutex so
// the byte cap and the perf.memory gauge are O(1) instead of an O(entries) walk. Single source
// of truth for the IconCacheApproxBytes gauge.
static std::size_t g_totalBytes = 0;

static std::size_t EntryBytes(int width, int height) {
    // RGBA32 estimate, matching the gauge formula. Ignores driver padding / mips.
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
}

static std::vector<ImTextureData*>& PendingDestroyTextures() {
    static std::vector<ImTextureData*> list;
    return list;
}

static void QueueTextureDestroy(ImTextureData* tex) {
    if (tex == nullptr) {
        return;
    }
    tex->Status = ImTextureStatus_WantDestroy;
    tex->WantDestroyNextFrame = true;
    PendingDestroyTextures().push_back(tex);
}

static bool CreateTextureFromRgba(const std::vector<unsigned char>& pixels, int width, int height,
                                  ImTextureData*& outTextureData, std::string& outError) {
    outTextureData = nullptr;
    outError.clear();
    if (pixels.empty() || width <= 0 || height <= 0) {
        outError = "Image pixels are empty.";
        return false;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (pixels.size() < expected) {
        outError = "Image pixel buffer smaller than width*height*4.";
        return false;
    }
    ImTextureData* tex = IM_NEW(ImTextureData)();
    if (!tex) {
        outError = "Failed to allocate ImTextureData.";
        return false;
    }
    tex->Create(ImTextureFormat_RGBA32, width, height);
    if (tex->Pixels == nullptr) {
        IM_DELETE(tex);
        outError = "ImTextureData::Create failed to allocate pixel storage.";
        return false;
    }
    std::memcpy(tex->Pixels, pixels.data(), expected);
    tex->UseColors = true;
    tex->Status = ImTextureStatus_WantCreate;
    ImGui::RegisterUserTexture(tex);
    outTextureData = tex;
    return true;
}

// Evict LRU-tail entries until admitting one more entry of `incomingBytes` keeps the cache
// within both the entry-count and aggregate-byte caps. Pass the simulated post-insert figures
// to the pure policy so the loop also makes room when the *new* entry is what tips a cap. The
// `!g_lru.empty()` guard stops a single oversized entry from looping against an empty cache.
static void EvictToFitUnlocked(std::size_t incomingBytes) {
    while (!g_lru.empty() &&
           smatchet::CacheOverCap(g_lru.size() + 1, g_totalBytes + incomingBytes, kMaxCacheEntries, kMaxCacheBytes)) {
        const std::string victim = g_lru.back();
        g_lru.pop_back();
        const auto it = g_map.find(victim);
        if (it != g_map.end()) {
            g_totalBytes -= EntryBytes(it->second.Width, it->second.Height);
            QueueTextureDestroy(it->second.Texture);
            g_map.erase(it);
        }
    }
}

static void TouchLruUnlocked(const std::string& key) {
    const auto mapIt = g_map.find(key);
    if (mapIt != g_map.end()) {
        // O(1) splice — iterator remains valid after splice, still points to the front element.
        g_lru.splice(g_lru.begin(), g_lru, mapIt->second.LruIt);
        mapIt->second.LruIt = g_lru.begin();
    } else {
        g_lru.push_front(key);
    }
}

static bool DecodeWithStb(const unsigned char* bytes, size_t byteCount, std::vector<unsigned char>& outRgba, int& outW,
                          int& outH, std::string& outError) {
    outRgba.clear();
    outW = 0;
    outH = 0;
    outError.clear();
    int w = 0;
    int h = 0;
    int channels = 0;
    // Pre-validate dimensions from the header ONLY (stbi_info reads no pixel data and allocates
    // nothing) so a malicious oversized image is rejected before the full stbi_load decode/alloc
    // (memory-pressure DoS, SECURITY_AUDIT_2026-06-13 synthesis #9). Overflow-safe pixel-budget
    // math lives in the pure IconDimensionsWithinCap helper.
    if (stbi_info_from_memory(bytes, static_cast<int>(byteCount), &w, &h, &channels) != 0) {
        if (!smatchet::IconDimensionsWithinCap(w, h, kMaxIconDimension, kMaxIconPixelBytes)) {
            LOG_WARN("Image rejected before decode: dimensions %dx%d exceed icon limit (%d) or pixel budget.", w, h,
                     kMaxIconDimension);
            outError = "Image dimensions exceed icon limit.";
            return false;
        }
    }
    w = 0;
    h = 0;
    channels = 0;
    unsigned char* pix = stbi_load_from_memory(bytes, static_cast<int>(byteCount), &w, &h, &channels, STBI_rgb_alpha);
    if (pix == nullptr || w <= 0 || h <= 0) {
        if (pix) {
            stbi_image_free(pix);
        }
        outError = "stb_image: decode failed.";
        return false;
    }
    if (w > kMaxIconDimension || h > kMaxIconDimension) {
        stbi_image_free(pix);
        outError = "Image dimensions exceed icon limit.";
        return false;
    }
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    outRgba.assign(pix, pix + n);
    stbi_image_free(pix);
    outW = w;
    outH = h;
    return true;
}

} // namespace

namespace SmatchetImageTextureCache {

void TickPendingDestroys() {
    auto& pending = PendingDestroyTextures();
    if (pending.empty()) {
        return;
    }
    for (auto it = pending.begin(); it != pending.end();) {
        ImTextureData* tex = *it;
        if (tex && tex->Status == ImTextureStatus_Destroyed) {
            ImGui::UnregisterUserTexture(tex);
            IM_DELETE(tex);
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
}

bool TryGetCached(const std::string& cacheKey, SmatchetLoadedIconTexture& out) {
    out = {};
    if (cacheKey.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_map.find(cacheKey);
    if (it == g_map.end() || it->second.Texture == nullptr) {
        return false;
    }
    TouchLruUnlocked(cacheKey);
    out.Texture = it->second.Texture;
    out.Width = it->second.Width;
    out.Height = it->second.Height;
    return true;
}

Result<SmatchetLoadedIconTexture> GetOrLoadFromMemory(const std::string& cacheKey, const unsigned char* bytes,
                                                      size_t byteCount) {
    using R = Result<SmatchetLoadedIconTexture>;
    if (cacheKey.empty() || bytes == nullptr || byteCount == 0) {
        return R::Err("Invalid cache key or empty image bytes.");
    }
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) == 0) {
        return R::Err("Renderer does not support textures.");
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto itExisting = g_map.find(cacheKey);
    if (itExisting != g_map.end() && itExisting->second.Texture != nullptr) {
        TouchLruUnlocked(cacheKey);
        SmatchetLoadedIconTexture out;
        out.Texture = itExisting->second.Texture;
        out.Width = itExisting->second.Width;
        out.Height = itExisting->second.Height;
        return R::Ok(out);
    }
    if (itExisting != g_map.end()) {
        g_totalBytes -= EntryBytes(itExisting->second.Width, itExisting->second.Height);
        g_map.erase(itExisting);
        const auto lit = std::find(g_lru.begin(), g_lru.end(), cacheKey);
        if (lit != g_lru.end()) {
            g_lru.erase(lit);
        }
    }

    std::vector<unsigned char> rgba;
    int w = 0;
    int h = 0;
    std::string decodeError;
    if (!DecodeWithStb(bytes, byteCount, rgba, w, h, decodeError)) {
        return R::Err(std::move(decodeError));
    }

    EvictToFitUnlocked(EntryBytes(w, h));

    ImTextureData* tex = nullptr;
    std::string createError;
    if (!CreateTextureFromRgba(rgba, w, h, tex, createError)) {
        return R::Err(std::move(createError));
    }

    g_lru.push_front(cacheKey);
    g_map[cacheKey] = CacheValue{tex, w, h, g_lru.begin()};
    g_totalBytes += EntryBytes(w, h);
    // LruIt is set; TouchLruUnlocked will use it for O(1) splice on subsequent accesses.
    SmatchetLoadedIconTexture out;
    out.Texture = tex;
    out.Width = w;
    out.Height = h;
    return R::Ok(out);
}

Result<SmatchetLoadedIconTexture> GetOrLoadFromFile(const std::string& cacheKey, const std::string& absolutePath) {
    using R = Result<SmatchetLoadedIconTexture>;
    if (absolutePath.empty()) {
        return R::Err("Empty path.");
    }
    SmatchetLoadedIconTexture cached;
    if (TryGetCached(cacheKey, cached)) {
        return R::Ok(cached);
    }
    std::ifstream ifs(absolutePath.c_str(), std::ios::binary);
    if (!ifs) {
        return R::Err("Failed to open file.");
    }
    ifs.seekg(0, std::ios::end);
    const std::streamoff len = ifs.tellg();
    if (len <= 0 || static_cast<size_t>(len) > kMaxFileReadBytes) {
        return R::Err("File too large or empty.");
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<unsigned char> buf(static_cast<size_t>(len));
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(len));
    if (!ifs) {
        return R::Err("Failed to read file.");
    }
    return GetOrLoadFromMemory(cacheKey, buf.data(), buf.size());
}

std::size_t IconCacheEntryCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_map.size();
}

std::size_t IconCacheApproxBytes() {
    // O(1): g_totalBytes is the maintained sum of EntryBytes, updated under g_mutex on every
    // insert/evict/erase. The byte cap and this gauge read the same source of truth.
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_totalBytes;
}

} // namespace SmatchetImageTextureCache
