#include "SmatchetImageTextureCache.h"

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

struct CacheValue {
    ImTextureData* Texture = nullptr;
    int Width = 0;
    int Height = 0;
    std::list<std::string>::iterator LruIt; ///< O(1) splice via stored iterator (§4.8 item 47).
};

static std::mutex g_mutex;
static std::list<std::string> g_lru;
static std::unordered_map<std::string, CacheValue> g_map;

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

static void EvictOneUnlocked() {
    while (g_lru.size() >= kMaxCacheEntries) {
        const std::string victim = g_lru.back();
        g_lru.pop_back();
        const auto it = g_map.find(victim);
        if (it != g_map.end()) {
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
    unsigned char* pix =
        stbi_load_from_memory(bytes, static_cast<int>(byteCount), &w, &h, &channels, STBI_rgb_alpha);
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

bool GetOrLoadFromMemory(const std::string& cacheKey, const unsigned char* bytes, size_t byteCount,
                         SmatchetLoadedIconTexture& out, std::string& outError) {
    out = {};
    outError.clear();
    if (cacheKey.empty() || bytes == nullptr || byteCount == 0) {
        outError = "Invalid cache key or empty image bytes.";
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) == 0) {
        outError = "Renderer does not support textures.";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto itExisting = g_map.find(cacheKey);
    if (itExisting != g_map.end() && itExisting->second.Texture != nullptr) {
        TouchLruUnlocked(cacheKey);
        out.Texture = itExisting->second.Texture;
        out.Width = itExisting->second.Width;
        out.Height = itExisting->second.Height;
        return true;
    }
    if (itExisting != g_map.end()) {
        g_map.erase(itExisting);
        const auto lit = std::find(g_lru.begin(), g_lru.end(), cacheKey);
        if (lit != g_lru.end()) {
            g_lru.erase(lit);
        }
    }

    std::vector<unsigned char> rgba;
    int w = 0;
    int h = 0;
    if (!DecodeWithStb(bytes, byteCount, rgba, w, h, outError)) {
        return false;
    }

    EvictOneUnlocked();

    ImTextureData* tex = nullptr;
    if (!CreateTextureFromRgba(rgba, w, h, tex, outError)) {
        return false;
    }

    g_lru.push_front(cacheKey);
    g_map[cacheKey] = CacheValue{tex, w, h, g_lru.begin()};
    // LruIt is set; TouchLruUnlocked will use it for O(1) splice on subsequent accesses.
    out.Texture = tex;
    out.Width = w;
    out.Height = h;
    return true;
}

bool GetOrLoadFromFile(const std::string& cacheKey, const std::string& absolutePath, SmatchetLoadedIconTexture& out,
                       std::string& outError) {
    out = {};
    outError.clear();
    if (absolutePath.empty()) {
        outError = "Empty path.";
        return false;
    }
    if (TryGetCached(cacheKey, out)) {
        return true;
    }
    std::ifstream ifs(absolutePath.c_str(), std::ios::binary);
    if (!ifs) {
        outError = "Failed to open file.";
        return false;
    }
    ifs.seekg(0, std::ios::end);
    const std::streamoff len = ifs.tellg();
    if (len <= 0 || static_cast<size_t>(len) > kMaxFileReadBytes) {
        outError = "File too large or empty.";
        return false;
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<unsigned char> buf(static_cast<size_t>(len));
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(len));
    if (!ifs) {
        outError = "Failed to read file.";
        return false;
    }
    return GetOrLoadFromMemory(cacheKey, buf.data(), buf.size(), out, outError);
}

void EvictCacheKey(const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_map.find(cacheKey);
    if (it == g_map.end()) {
        return;
    }
    QueueTextureDestroy(it->second.Texture);
    g_map.erase(it);
    const auto lit = std::find(g_lru.begin(), g_lru.end(), cacheKey);
    if (lit != g_lru.end()) {
        g_lru.erase(lit);
    }
}

std::size_t IconCacheEntryCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_map.size();
}

std::size_t IconCacheApproxBytes() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::size_t total = 0;
    for (const auto& kv : g_map) {
        // Estimate: RGBA32 pixel storage. Ignores driver padding / mips — a gauge, not RSS.
        total += static_cast<std::size_t>(kv.second.Width) * static_cast<std::size_t>(kv.second.Height) * 4u;
    }
    return total;
}

} // namespace SmatchetImageTextureCache






