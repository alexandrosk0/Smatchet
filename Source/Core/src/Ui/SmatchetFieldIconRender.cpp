#include "SmatchetFieldIconRender.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetFieldIconRender_detail.h"
#include "SmatchetImageTextureCache.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "TrackerFieldValueUtils.h"

#include "imgui.h"

#include <algorithm>

#include <cpr/cpr.h>

#include <ghc/filesystem.hpp>

#include <chrono>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

namespace fs = ghc::filesystem;

// Pure resolution/parsing half — extracted to SmatchetFieldIconRender_detail.cpp (gap-map
// Tier 5 `_detail` pattern) so the untrusted-input logic is doctest-covered without this
// TU's cpr/ImGui/texture-cache closure.
using SmatchetFieldIconRender::detail::JoinDomainAndPath;
using SmatchetFieldIconRender::detail::ParsePriorityJson;
using SmatchetFieldIconRender::detail::ResolveJiraIconUrlReference;
using SmatchetFieldIconRender::detail::SlugIsKnown;
using SmatchetFieldIconRender::detail::UrlToCacheFileName;

std::mutex g_jiraDomainForPriorityIconsMutex;
std::string g_jiraDomainForPriorityIconsCache;
std::chrono::steady_clock::time_point g_jiraDomainForPriorityIconsCacheAt{};

/** Priority icon path resolution only needs domain; avoid ConfigManager::Load on every grid cell. */
std::string GetCachedJiraDomainForPriorityIcons() {
    constexpr auto kTtl = std::chrono::seconds(60);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_jiraDomainForPriorityIconsMutex);
    if (!g_jiraDomainForPriorityIconsCache.empty() && now - g_jiraDomainForPriorityIconsCacheAt < kTtl) {
        return g_jiraDomainForPriorityIconsCache;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    g_jiraDomainForPriorityIconsCache = TrimCopyAsciiWhitespace(cfg.Domain);
    g_jiraDomainForPriorityIconsCacheAt = now;
    return g_jiraDomainForPriorityIconsCache;
}

/** Shipped under repo `Scripts/art/priority/<slug>.png` (copied next to exe with Scripts/). */
std::string TryBundledPriorityPngPath(const std::string& slug) {
    if (slug.empty() || !SlugIsKnown(slug)) {
        return std::string();
    }
    const std::string base = ConfigManager::GetRuntimeAssetDirectory();
    if (base.empty()) {
        return std::string();
    }
    const std::string p = base + "Scripts/art/priority/" + slug + ".png";
    std::error_code ec;
    const fs::path fp(p);
    if (fs::exists(fp, ec) && fs::is_regular_file(fp, ec)) {
        return p;
    }
    return std::string();
}

bool EnsureFieldIconsCacheDir(std::string& outDir, std::string& outError) {
    outError.clear();
    const std::string base = ConfigManager::GetUserDataDirectory();
    if (base.empty()) {
        outError = "Files base directory is empty.";
        return false;
    }
    outDir = base + "cache/field_icons/";
    std::error_code ec;
    fs::create_directories(fs::path(outDir), ec);
    if (ec) {
        outError = ec.message();
        return false;
    }
    return true;
}

bool HttpGetBinary(const std::string& url, std::vector<unsigned char>& out, std::string& outError) {
    out.clear();
    outError.clear();
    /* PILLAR2_WORKER_ONLY */ // est-latency: ~3000ms — sole caller (line 348) inside app.LaunchBackgroundTask lambda.
    cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Timeout{3000});
    if (r.error.code != cpr::ErrorCode::OK) {
        outError = r.error.message;
        return false;
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        outError = "HTTP status " + std::to_string(r.status_code);
        return false;
    }
    if (r.text.size() > 512u * 1024u) {
        outError = "Response too large.";
        return false;
    }
    out.assign(r.text.begin(), r.text.end());
    return true;
}

// In-flight URL fetch set. Suppresses re-issue of identical HTTP requests across many frames
// while a worker is still downloading the icon. UI-thread reads + worker-thread writes go
// through the mutex (Pillar 2 — finding #1).
std::mutex& IconUrlFetchInFlightMutex() {
    static std::mutex m;
    return m;
}
std::unordered_set<std::string>& IconUrlFetchInFlightSet() {
    static std::unordered_set<std::string> s;
    return s;
}

bool IconUrlFetchInFlight(const std::string& url) {
    std::lock_guard<std::mutex> lock(IconUrlFetchInFlightMutex());
    return IconUrlFetchInFlightSet().count(url) > 0;
}

bool MarkIconUrlFetchInFlight(const std::string& url) {
    std::lock_guard<std::mutex> lock(IconUrlFetchInFlightMutex());
    auto& s = IconUrlFetchInFlightSet();
    if (s.count(url) > 0) {
        return false;
    }
    s.insert(url);
    return true;
}

void ClearIconUrlFetchInFlight(const std::string& url) {
    std::lock_guard<std::mutex> lock(IconUrlFetchInFlightMutex());
    IconUrlFetchInFlightSet().erase(url);
}

// DR28 — URL-level negative fetch cache with time-based backoff. A worker whose HTTP GET fails
// records the steady-clock time of failure here; LoadOrFetchUrlImage consults it before deferring
// another (up to 3s) cpr GET so an unresolvable icon URL backs off instead of re-fetching (and the
// grid cell re-parsing JSON + re-stat'ing disk) every single frame for the whole session. Keyed by
// URL; capped so a tracker returning many distinct bad URLs cannot grow it without bound.
constexpr auto kIconFetchNegativeBackoff = std::chrono::minutes(5);
constexpr size_t kIconFetchNegativeCap = 256;

std::mutex& IconUrlFetchNegativeMutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, std::chrono::steady_clock::time_point>& IconUrlFetchNegativeMap() {
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> m;
    return m;
}

long long SteadyMillis(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

// True when `url` failed a fetch recently enough to still be inside the backoff window.
bool IconUrlFetchInBackoff(const std::string& url) {
    const auto now = std::chrono::steady_clock::now();
    const long long backoffMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(kIconFetchNegativeBackoff).count();
    std::lock_guard<std::mutex> lock(IconUrlFetchNegativeMutex());
    const auto& m = IconUrlFetchNegativeMap();
    const auto it = m.find(url);
    const bool have = it != m.end();
    const long long lastMs = have ? SteadyMillis(it->second) : 0;
    return !SmatchetFieldIconRender::ShouldAttemptIconResolve(have, lastMs, SteadyMillis(now), backoffMs);
}

void RecordIconUrlFetchFailure(const std::string& url) {
    if (url.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(IconUrlFetchNegativeMutex());
    auto& m = IconUrlFetchNegativeMap();
    if (m.size() >= kIconFetchNegativeCap) {
        m.clear();
    }
    m[url] = now;
}

void ClearIconUrlFetchFailure(const std::string& url) {
    std::lock_guard<std::mutex> lock(IconUrlFetchNegativeMutex());
    IconUrlFetchNegativeMap().erase(url);
}

// In-flight file-read set. Mirrors the URL fetch set shape — the URL-disk-cache-hit branch and
// the file-path branch both defer their ifstream off the UI thread; the worker's PostToMainThread
// invokes GetOrLoadFromMemory on the bytes (GPU texture upload requires UI thread). Keyed by the
// canonical texture-cache key (`url:` + url for cached URLs, `file:` + path for local files) so
// each call site uses a stable handle.
std::mutex& IconFileReadInFlightMutex() {
    static std::mutex m;
    return m;
}
std::unordered_set<std::string>& IconFileReadInFlightSet() {
    static std::unordered_set<std::string> s;
    return s;
}

bool IconFileReadInFlight(const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(IconFileReadInFlightMutex());
    return IconFileReadInFlightSet().count(cacheKey) > 0;
}

bool MarkIconFileReadInFlight(const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(IconFileReadInFlightMutex());
    auto& s = IconFileReadInFlightSet();
    if (s.count(cacheKey) > 0) {
        return false;
    }
    s.insert(cacheKey);
    return true;
}

void ClearIconFileReadInFlight(const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(IconFileReadInFlightMutex());
    IconFileReadInFlightSet().erase(cacheKey);
}

/// Loads the URL-keyed icon texture. Returns:
///   - `true` + populated `out` when the texture is in the cache or on disk (decoded inline).
///   - `false` + `outDeferred=true` when a background fetch was scheduled (or one is already
///     in-flight); the caller MUST NOT negative-memo this result — the URL will be cached on a
///     subsequent frame and the next memo lookup will hit.
///   - `false` + `outDeferred=false` when the load genuinely failed (bad path, disk error).
bool LoadOrFetchUrlImage(AppController& app, const std::string& url, SmatchetLoadedIconTexture& out,
                         std::string& outError, bool& outDeferred) {
    out = {};
    outError.clear();
    outDeferred = false;
    // Canonical cache key for URL-loaded icons. All paths through this function MUST
    // end up storing the texture under this key — otherwise the caller's memo (which
    // derives its key from the URL candidate) will look it up and miss, falling back
    // to the slow path every frame.
    const std::string urlKey = std::string("url:") + url;
    if (SmatchetImageTextureCache::TryGetCached(urlKey, out)) {
        return true;
    }
    std::string cacheDir;
    if (!EnsureFieldIconsCacheDir(cacheDir, outError)) {
        return false;
    }
    const std::string fileName = UrlToCacheFileName(url);
    const std::string diskPath = cacheDir + fileName;
    // If the bytes are on disk, defer the read off the UI thread (Pillar 2 — disk-cache-hit was
    // still ifstream-on-UI). Worker reads the file; PostToMainThread invokes GetOrLoadFromMemory
    // which uploads the GPU texture (UI-thread-only). Caller sees outDeferred=true on this frame;
    // the next frame's TryGetCached hits.
    std::error_code ec;
    if (fs::exists(fs::path(diskPath), ec) && fs::file_size(fs::path(diskPath), ec) > 0) {
        if (IconFileReadInFlight(urlKey)) {
            outDeferred = true;
            return false;
        }
        if (!MarkIconFileReadInFlight(urlKey)) {
            outDeferred = true;
            return false;
        }
        const std::string capturedDiskPath = diskPath;
        const std::string capturedUrlKey = urlKey;
        app.LaunchBackgroundTask([&app, capturedDiskPath, capturedUrlKey]() {
            std::vector<unsigned char> bytes;
            /* PILLAR2_WORKER_ONLY */ // est-latency: ~10ms — read of cached icon file (typically < 100 KB).
            std::ifstream ifs(capturedDiskPath.c_str(), std::ios::binary);
            if (ifs) {
                bytes.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            }
            app.PostToMainThread([capturedUrlKey, bytes]() {
                ClearIconFileReadInFlight(capturedUrlKey);
                if (bytes.empty()) {
                    return;
                }
                SmatchetLoadedIconTexture loaded;
                std::string err;
                (void)SmatchetImageTextureCache::GetOrLoadFromMemory(capturedUrlKey, bytes.data(), bytes.size(), loaded,
                                                                     err);
            });
        });
        outDeferred = true;
        return false;
    }
    // No cache, no disk → defer HTTP fetch to a worker thread (Pillar 2 — finding #1).
    // The grid-render path was blocking up to 3s per priority icon on first frame.
    // DR28: if this URL failed to fetch recently, report a genuine (non-deferred) failure so the
    // caller negative-memos the cell instead of re-issuing the 3s cpr GET every frame. Backoff
    // expiry lets a transient outage recover on a later frame.
    if (IconUrlFetchInBackoff(url)) {
        outError = "Icon fetch in backoff after a prior failure.";
        outDeferred = false;
        return false;
    }
    if (IconUrlFetchInFlight(url)) {
        outDeferred = true;
        return false;
    }
    if (!MarkIconUrlFetchInFlight(url)) {
        // Lost a race; another thread is now fetching. Treat as deferred.
        outDeferred = true;
        return false;
    }
    const std::string capturedUrl = url;
    const std::string capturedDiskPath = diskPath;
    const std::string capturedUrlKey = urlKey;
    app.LaunchBackgroundTask([&app, capturedUrl, capturedDiskPath, capturedUrlKey]() {
        std::vector<unsigned char> bytes;
        std::string fetchError;
        const bool fetchOk = HttpGetBinary(capturedUrl, bytes, fetchError);
        if (fetchOk) {
            std::ofstream ofs(capturedDiskPath.c_str(), std::ios::binary | std::ios::trunc);
            if (ofs) {
                ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
        }
        // Post texture upload + in-flight-set release back to the UI thread. The image-texture
        // cache requires UI-thread invocation (GL / DX12 resource creation).
        app.PostToMainThread([capturedUrl, capturedUrlKey, bytes, fetchOk, fetchError]() {
            ClearIconUrlFetchInFlight(capturedUrl);
            if (!fetchOk || bytes.empty()) {
                // DR28: memoise the failure so subsequent frames back off (see IconUrlFetchInBackoff)
                // rather than re-fetching this URL every frame for the rest of the session.
                (void)fetchError;
                RecordIconUrlFetchFailure(capturedUrl);
                return;
            }
            ClearIconUrlFetchFailure(capturedUrl);
            SmatchetLoadedIconTexture loaded;
            std::string err;
            (void)SmatchetImageTextureCache::GetOrLoadFromMemory(capturedUrlKey, bytes.data(), bytes.size(), loaded,
                                                                 err);
        });
    });
    outDeferred = true;
    return false;
}

bool LoadTextureForResolvedPath(AppController& app, const std::string& resolved, SmatchetLoadedIconTexture& out,
                                std::string& outError, bool& outDeferred) {
    out = {};
    outError.clear();
    outDeferred = false;
    if (resolved.empty()) {
        outError = "Empty path.";
        return false;
    }
    if (resolved.rfind("https://", 0) == 0 || resolved.rfind("http://", 0) == 0) {
        return LoadOrFetchUrlImage(app, resolved, out, outError, outDeferred);
    }
    // Pillar 2 — local-file branch: GetOrLoadFromFile internally ifstreams + decodes + uploads.
    // Fast path: cache hit is cheap (no I/O). Slow path on cache miss is deferred to a worker —
    // worker reads the bytes, posts back to UI thread for GPU texture upload via GetOrLoadFromMemory.
    const std::string cacheKey = std::string("file:") + resolved;
    if (SmatchetImageTextureCache::TryGetCached(cacheKey, out)) {
        return true;
    }
    if (IconFileReadInFlight(cacheKey)) {
        outDeferred = true;
        return false;
    }
    if (!MarkIconFileReadInFlight(cacheKey)) {
        outDeferred = true;
        return false;
    }
    const std::string capturedResolved = resolved;
    const std::string capturedCacheKey = cacheKey;
    app.LaunchBackgroundTask([&app, capturedResolved, capturedCacheKey]() {
        std::vector<unsigned char> bytes;
        /* PILLAR2_WORKER_ONLY */ // est-latency: ~10ms — read of cached icon file (typically < 100 KB).
        std::ifstream ifs(capturedResolved.c_str(), std::ios::binary);
        if (ifs) {
            bytes.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }
        app.PostToMainThread([capturedCacheKey, bytes]() {
            ClearIconFileReadInFlight(capturedCacheKey);
            if (bytes.empty()) {
                return;
            }
            SmatchetLoadedIconTexture loaded;
            std::string err;
            (void)SmatchetImageTextureCache::GetOrLoadFromMemory(capturedCacheKey, bytes.data(), bytes.size(), loaded,
                                                                 err);
        });
    });
    outDeferred = true;
    return false;
}

// Backwards-compat wrapper for the const-AppController call sites that don't need the deferred
// distinction (e.g. preview-only/non-priority paths). HTTP fetch still defers; caller sees
// `false` while the fetch is in flight.
bool LoadTextureForResolvedPath(const AppController& app, const std::string& resolved, SmatchetLoadedIconTexture& out,
                                std::string& outError) {
    bool deferred = false;
    // `LaunchBackgroundTask` is non-const; cast away const to dispatch. This matches the rest
    // of the AppController API (`mainThreadDispatcher` is a mutable member; the worker pattern
    // doesn't mutate observable AppController state).
    return LoadTextureForResolvedPath(const_cast<AppController&>(app), resolved, out, outError, deferred);
}

/**
 * Per-frame fast path: maps a raw priority value (Jira priority JSON blob) to the texture-cache key
 * that resolved successfully on a prior frame. Lets the grid hot path skip JSON parse, slug
 * normalization, candidate building and disk stat — collapses to one hashmap lookup + the locked
 * cache lookup. Capped to bound memory if a customer has many distinct values.
 */
constexpr size_t kPriorityResolveMemoCap = 256;
struct PriorityResolveMemo {
    std::string CacheKey;  ///< Texture-cache key (empty iff Negative).
    std::string Label;     ///< Parsed friendly name; preserves tooltip text on the fast path.
    bool Negative = false; ///< True when the load failed and we want to suppress future retries.
};
std::unordered_map<std::string, PriorityResolveMemo>& PriorityRawValueToCacheKey() {
    static std::unordered_map<std::string, PriorityResolveMemo> m;
    return m;
}

void RememberPriorityResolution(const std::string& rawValue, const std::string& cacheKey, const std::string& label) {
    if (rawValue.empty() || cacheKey.empty()) {
        return;
    }
    auto& m = PriorityRawValueToCacheKey();
    if (m.size() >= kPriorityResolveMemoCap) {
        m.clear();
    }
    PriorityResolveMemo entry;
    entry.CacheKey = cacheKey;
    entry.Label = label;
    entry.Negative = false;
    m[rawValue] = entry;
}

/// Memoise a failed-load: keeps the slow path from being re-entered every frame for cells whose
/// icon URL/file genuinely cannot be resolved (offline, missing bundle, malformed JSON). Without
/// this, ParsePriorityJson + LoadPriorityIconWithFallbacks would run on every frame for every
/// such cell — dominating the FPS budget at typical grid sizes.
void RememberNegativePriorityResolution(const std::string& rawValue, const std::string& label) {
    if (rawValue.empty())
        return;
    auto& m = PriorityRawValueToCacheKey();
    if (m.size() >= kPriorityResolveMemoCap) {
        m.clear();
    }
    PriorityResolveMemo entry;
    entry.Label = label;
    entry.Negative = true;
    m[rawValue] = entry;
}

/**
 * Load priority icon: Jira `iconUrl` (absolute or `/...` on your site), then same slugs as the admin
 * priorities page (`<domain>/images/icons/priorities/<slug>.png`), then bundled offline fallback.
 *
 * On success, populates `outResolvedCacheKey` with the texture-cache key that resolved so the caller
 * can memoise it for subsequent frames.
 */
bool LoadPriorityIconWithFallbacks(AppController& app, const std::string& iconUrl, const std::string& slug,
                                   SmatchetLoadedIconTexture& out, std::string& outResolvedCacheKey,
                                   std::string& outLastError, bool& outAnyDeferred) {
    out = {};
    outLastError.clear();
    outResolvedCacheKey.clear();
    outAnyDeferred = false;
    const std::string domain = GetCachedJiraDomainForPriorityIcons();
    std::vector<std::string> candidates;
    auto pushUnique = [&](const std::string& s) {
        if (s.empty()) {
            return;
        }
        if (std::any_of(candidates.begin(), candidates.end(), [&](const auto& existing) { return existing == s; })) {
            return;
        }
        candidates.push_back(s);
    };
    pushUnique(ResolveJiraIconUrlReference(iconUrl, domain));
    if (!slug.empty() && !domain.empty()) {
        pushUnique(JoinDomainAndPath(domain, std::string("/images/icons/priorities/") + slug + ".png"));
    }
    pushUnique(TryBundledPriorityPngPath(slug));
    if (candidates.empty()) {
        outLastError = "No priority icon source.";
        return false;
    }
    for (const auto& c : candidates) {
        std::string err;
        bool deferred = false;
        if (LoadTextureForResolvedPath(app, c, out, err, deferred)) {
            const bool isUrl = c.rfind("https://", 0) == 0 || c.rfind("http://", 0) == 0;
            outResolvedCacheKey = (isUrl ? std::string("url:") : std::string("file:")) + c;
            return true;
        }
        if (deferred) {
            outAnyDeferred = true;
        }
        outLastError = err;
    }
    return false;
}

void DrawLoadedIconSized(const SmatchetLoadedIconTexture& icon, float maxEdge) {
    if (icon.Texture == nullptr || icon.Width <= 0 || icon.Height <= 0) {
        return;
    }
    const float w = static_cast<float>(icon.Width);
    const float h = static_cast<float>(icon.Height);
    const float scale = maxEdge / (std::max)(w, h);
    const float drawW = w * scale;
    const float drawH = h * scale;
    ImGui::Image(icon.Texture->GetTexRef(), ImVec2(drawW, drawH));
}

/// Lazy-tooltip variant: builds the tooltip string only when ImGui reports the icon is hovered.
/// Saves a per-cell DisplayValueForTrackerDateField call (which JSON-parses the value) when the
/// vast majority of cells aren't being hovered.
template <typename TooltipFn>
void DrawLoadedIconWithLazyTooltip(const SmatchetLoadedIconTexture& icon, float maxEdge, bool tooltipsEnabled,
                                   TooltipFn&& tooltipBuilder) {
    DrawLoadedIconSized(icon, maxEdge);
    if (!tooltipsEnabled || !ImGui::IsItemHovered()) {
        return;
    }
    const std::string tooltip = tooltipBuilder();
    if (tooltip.empty()) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
    ImGui::TextUnformatted(tooltip.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace

namespace SmatchetFieldIconRender {

bool DrawImagePathOrUrl(AppController& app, const std::string& pathOrUrl, float width, float height) {
    const std::string resolved = app.ResolveFieldIconAssetPath(pathOrUrl);
    if (resolved.empty()) {
        return false;
    }
    std::string err;
    SmatchetLoadedIconTexture icon;
    if (!LoadTextureForResolvedPath(app, resolved, icon, err)) {
        return false;
    }
    // CPP_CODE_AUDIT.md #22: a cache entry can report success with a null Texture
    // (a failed/zero-dim decode). Guard once here so both the zero-size and
    // explicit-size branches below agree on the return value for that failure.
    if (icon.Texture == nullptr) {
        return false;
    }
    float drawW = width;
    float drawH = height;
    if (drawW <= 0.0f || drawH <= 0.0f) {
        const float maxEdge = ImGui::GetFrameHeight();
        DrawLoadedIconSized(icon, maxEdge);
        return true;
    }
    ImGui::Image(icon.Texture->GetTexRef(), ImVec2(drawW, drawH));
    return true;
}

bool TryGetInlineFieldIconTexture(const AppController& app, const TrackerField& field, const std::string& rawValue,
                                  SmatchetLoadedIconTexture& outIcon, std::string& outError) {
    outIcon = {};
    outError.clear();
#if !defined(SMATCHET_WITH_LUA_AUTOMATION)
    (void)app;
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    std::string mapPath;
    if (app.TryGetFieldIconMapTarget(field.Id, &field, rawValue, mapPath)) {
        const std::string resolved = app.ResolveFieldIconAssetPath(mapPath);
        const std::string& loadPath = resolved.empty() ? mapPath : resolved;
        if (LoadTextureForResolvedPath(app, loadPath, outIcon, outError)) {
            return true;
        }
        return false;
    }
#endif
    if (field.Id != "priority") {
        return false;
    }
    // Empty priority values: no JSON to parse and no icon to render. Skipping the slow path
    // entirely is the main per-frame saving for cells with no priority assigned — at typical
    // grid sizes the unconditional ParsePriorityJson + LoadPriorityIconWithFallbacks pair
    // was dominating the FPS budget.
    if (rawValue.empty()) {
        return false;
    }
    {
        const auto& memo = PriorityRawValueToCacheKey();
        const auto it = memo.find(rawValue);
        if (it != memo.end()) {
            // Negative entry: load failed previously — return false without re-attempting.
            if (it->second.Negative) {
                return false;
            }
            if (SmatchetImageTextureCache::TryGetCached(it->second.CacheKey, outIcon)) {
                return true;
            }
            // Texture evicted — fall through to slow path which will re-load and refresh the memo.
        }
    }
    std::string iconUrl;
    std::string label;
    std::string slug;
    if (!ParsePriorityJson(rawValue, iconUrl, label, slug)) {
        // Malformed / empty priority JSON: negative-cache so we don't keep parsing every frame.
        RememberNegativePriorityResolution(rawValue, label);
        return false;
    }
    std::string resolvedKey;
    bool anyDeferred = false;
    if (!LoadPriorityIconWithFallbacks(const_cast<AppController&>(app), iconUrl, slug, outIcon, resolvedKey, outError,
                                       anyDeferred)) {
        // Don't negative-memo when a candidate is mid-download — next frame will hit the cache.
        if (!anyDeferred) {
            RememberNegativePriorityResolution(rawValue, label);
        }
        return false;
    }
    RememberPriorityResolution(rawValue, resolvedKey, label);
    return true;
}

bool DrawInlineFieldIconIfAny(const AppController& app, const TrackerField& field, const std::string& rawValue) {
    SmatchetLoadedIconTexture icon;
    std::string err;
    if (!TryGetInlineFieldIconTexture(app, field, rawValue, icon, err)) {
        return false;
    }
    DrawLoadedIconSized(icon, ImGui::GetFrameHeight());
    ImGui::SameLine(0.0f, 6.0f);
    return true;
}

bool TryDrawFieldValueIcon(const AppController& app, const std::string& fieldId, const TrackerField* field,
                           const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool allowCellEdits) {
    (void)availWidth;
#if !defined(SMATCHET_WITH_LUA_AUTOMATION)
    (void)app;
#endif
    if (allowCellEdits && field != nullptr && field->Id == "priority") {
        return false;
    }

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    std::string pathOrUrl;
    // Icon maps replace the whole cell; never steal focus from editable SingleSelect / other editors.
    if (!allowCellEdits && app.TryGetFieldIconMapTarget(fieldId, field, rawValue, pathOrUrl)) {
        const std::string resolved = app.ResolveFieldIconAssetPath(pathOrUrl);
        const std::string& loadPath = resolved.empty() ? pathOrUrl : resolved;
        std::string err;
        SmatchetLoadedIconTexture icon;
        if (LoadTextureForResolvedPath(app, loadPath, icon, err)) {
            const float maxEdge = ImGui::GetFrameHeight();
            DrawLoadedIconWithLazyTooltip(icon, maxEdge, tooltipsEnabled, [&]() {
                return field ? DisplayValueForTrackerDateField(fieldId, field, rawValue) : std::string(rawValue);
            });
            return true;
        }
    }
#endif

    if (fieldId != "priority" && !(field != nullptr && field->Id == "priority")) {
        return false;
    }

    SmatchetLoadedIconTexture icon;
    {
        const auto& memo = PriorityRawValueToCacheKey();
        const auto it = memo.find(rawValue);
        if (it != memo.end()) {
            // DR28: a known-negative cell short-circuits — no re-parse, no re-fetch this frame.
            if (it->second.Negative) {
                return false;
            }
            if (SmatchetImageTextureCache::TryGetCached(it->second.CacheKey, icon)) {
                const float maxEdge = ImGui::GetFrameHeight();
                const std::string& memoLabel = it->second.Label;
                DrawLoadedIconWithLazyTooltip(icon, maxEdge, tooltipsEnabled, [&]() {
                    return field ? DisplayValueForTrackerDateField(fieldId, field, rawValue)
                                 : (memoLabel.empty() ? rawValue : memoLabel);
                });
                return true;
            }
            // Texture evicted — fall through to the slow path, which re-loads and refreshes the memo.
        }
    }

    std::string iconUrl;
    std::string label;
    std::string slug;
    if (!ParsePriorityJson(rawValue, iconUrl, label, slug)) {
        // DR28: malformed / unparseable priority JSON — negative-memo so we stop re-parsing it.
        RememberNegativePriorityResolution(rawValue, label);
        return false;
    }

    std::string err;
    std::string resolvedKey;
    bool anyDeferred = false;
    if (!LoadPriorityIconWithFallbacks(const_cast<AppController&>(app), iconUrl, slug, icon, resolvedKey, err,
                                       anyDeferred)) {
        // DR28: record a negative only when nothing is mid-download — a deferred candidate will hit
        // the cache on a later frame, so negative-memoing it would suppress a load that is coming.
        if (!anyDeferred) {
            RememberNegativePriorityResolution(rawValue, label);
        }
        return false;
    }
    RememberPriorityResolution(rawValue, resolvedKey, label);

    const float maxEdge = ImGui::GetFrameHeight();
    DrawLoadedIconWithLazyTooltip(icon, maxEdge, tooltipsEnabled, [&]() {
        return field ? DisplayValueForTrackerDateField(fieldId, field, rawValue)
                     : std::string(label.empty() ? rawValue : label);
    });
    return true;
}

} // namespace SmatchetFieldIconRender
