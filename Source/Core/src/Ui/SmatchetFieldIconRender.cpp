#include "SmatchetFieldIconRender.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "TunableLimits.h"
#include "SmatchetFieldIconDiskCache.h"
#include "SmatchetFieldIconRender_detail.h"
#include "SmatchetImageTextureCache.h"
#include "SmatchetResult.h"
#include "StringUtil.h"
#include "TicketGridModel.h"
#include "TrackerFieldValueUtils.h"

#include "imgui.h"

#include <algorithm>

#include <cpr/cpr.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// Pure resolution/parsing half — extracted to SmatchetFieldIconRender_detail.cpp (gap-map
// Tier 5 `_detail` pattern) so the untrusted-input logic is doctest-covered without this
// TU's cpr/ImGui/texture-cache closure.
using SmatchetFieldIconRender::detail::JoinDomainAndPath;
using SmatchetFieldIconRender::detail::ParsePriorityJson;
using SmatchetFieldIconRender::detail::ResolveJiraIconUrlReference;
using SmatchetFieldIconRender::detail::SlugIsKnown;

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

/** Shipped under repo `Scripts/art/priority/<slug>.png` (copied next to exe with Scripts/). The path is only
 *  built here, never stat'ed: this runs on the UI thread, and a missing file fails on the worker that reads it. */
std::string TryBundledPriorityPngPath(const std::string& slug) {
    if (slug.empty() || !SlugIsKnown(slug)) {
        return std::string();
    }
    const std::string base = ConfigManager::GetRuntimeAssetDirectory();
    if (base.empty()) {
        return std::string();
    }
    return base + "Scripts/art/priority/" + slug + ".png";
}

/** Production icon fetch: one capped GET. Worker only. */
SmatchetFieldIconDiskCache::FetchFn ProductionFetch() {
    return [](const std::string& url) -> Result<SmatchetFieldIconDiskCache::IconBytes> {
        using R = Result<SmatchetFieldIconDiskCache::IconBytes>;
        /* PILLAR2_WORKER_ONLY */ // est-latency: ~3000ms — runs only inside the worker launched by LaunchIconResolve.
        cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Timeout{3000});
        if (r.error.code != cpr::ErrorCode::OK) {
            return R::Err(r.error.message);
        }
        if (r.status_code < 200 || r.status_code >= 300) {
            return R::Err("HTTP status " + std::to_string(r.status_code));
        }
        if (r.text.size() > SmatchetFieldIconDiskCache::kMaxIconFileBytes) {
            return R::Err("Response too large.");
        }
        return R::Ok(SmatchetFieldIconDiskCache::IconBytes(r.text.begin(), r.text.end()));
    };
}

// The fetch workers use. Replaceable so a test can serve bytes (or refuse) without a network.
std::mutex& IconFetchFnMutex() {
    static std::mutex m;
    return m;
}
SmatchetFieldIconDiskCache::FetchFn& IconFetchFnSlot() {
    static SmatchetFieldIconDiskCache::FetchFn fn = ProductionFetch();
    return fn;
}
SmatchetFieldIconDiskCache::FetchFn CurrentIconFetchFn() {
    std::lock_guard<std::mutex> lock(IconFetchFnMutex());
    return IconFetchFnSlot();
}

// In-flight resolve set, keyed by the texture-cache key ("url:" + url, "file:" + path). A key is marked
// BEFORE its worker is launched and released exactly once by InFlightClaim, whichever way the work ends
// (completed, failed, launch threw, or the task was dropped unrun at shutdown). UI-thread reads and
// worker writes go through the mutex.
std::mutex& IconInFlightMutex() {
    static std::mutex m;
    return m;
}
std::unordered_set<std::string>& IconInFlightSet() {
    static std::unordered_set<std::string> s;
    return s;
}

bool IconInFlight(const std::string& key) {
    std::lock_guard<std::mutex> lock(IconInFlightMutex());
    return IconInFlightSet().count(key) > 0;
}

bool MarkIconInFlight(const std::string& key) {
    std::lock_guard<std::mutex> lock(IconInFlightMutex());
    return IconInFlightSet().insert(key).second;
}

void ClearIconInFlight(const std::string& key) {
    std::lock_guard<std::mutex> lock(IconInFlightMutex());
    IconInFlightSet().erase(key);
}

/// Releases a key's in-flight mark once. Held by shared_ptr from the worker and from its completion, so the
/// mark clears when the last holder goes away even if neither ever ran.
class InFlightClaim {
  public:
    explicit InFlightClaim(std::string key) : key_(std::move(key)) {}
    ~InFlightClaim() { Release(); }
    InFlightClaim(const InFlightClaim&) = delete;
    InFlightClaim& operator=(const InFlightClaim&) = delete;
    void Release() {
        if (!released_.exchange(true)) {
            ClearIconInFlight(key_);
        }
    }

  private:
    const std::string key_;
    std::atomic<bool> released_{false};
};

// DR28 — negative resolve cache with time-based backoff, keyed by texture-cache key. A worker whose fetch,
// read or decode fails records the steady-clock time of failure here (the map is mutex-guarded); LoadOrFetchUrlImage
// consults it before deferring another (up to 3s) cpr GET so an unresolvable icon URL backs off instead of re-fetching
// (and the grid cell re-parsing JSON) every single frame for the whole session. Keyed by texture-cache key; capped
// (limits::Get().FieldIconFetchNegativeMax, default 256) so a tracker returning many distinct bad URLs cannot grow it
// without bound.
constexpr auto kIconFetchNegativeBackoff = std::chrono::minutes(5);

std::mutex& IconNegativeMutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, std::chrono::steady_clock::time_point>& IconNegativeMap() {
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> m;
    return m;
}

long long SteadyMillis(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

// True when `key` failed to resolve recently enough to still be inside the backoff window.
bool IconInBackoff(const std::string& key) {
    const auto now = std::chrono::steady_clock::now();
    const long long backoffMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(kIconFetchNegativeBackoff).count();
    std::lock_guard<std::mutex> lock(IconNegativeMutex());
    const auto& m = IconNegativeMap();
    const auto it = m.find(key);
    const bool have = it != m.end();
    const long long lastMs = have ? SteadyMillis(it->second) : 0;
    return !SmatchetFieldIconRender::ShouldAttemptIconResolve(have, lastMs, SteadyMillis(now), backoffMs);
}

void RecordIconFailure(const std::string& key) {
    if (key.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(IconNegativeMutex());
    auto& m = IconNegativeMap();
    if (m.size() >= static_cast<size_t>(smatchet::limits::Get().FieldIconFetchNegativeMax)) {
        m.clear();
    }
    m[key] = now;
}

void ClearIconFailure(const std::string& key) {
    std::lock_guard<std::mutex> lock(IconNegativeMutex());
    IconNegativeMap().erase(key);
}

// The icon cache directory is pruned (legacy names, orphaned temps, the size cap) once per process: the first
// resolve worker to reach a directory does it, every later one skips it. Worker only.
void PruneCacheDirOncePerSession(const std::string& cacheDir) {
    static std::mutex m;
    static std::unordered_set<std::string> done;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!done.insert(cacheDir).second) {
            return;
        }
    }
    const std::uint64_t capBytes =
        static_cast<std::uint64_t>(smatchet::limits::MiB(smatchet::limits::Get().FieldIconDiskCacheMaxMiB));
    (void)SmatchetFieldIconDiskCache::PruneCacheDir(cacheDir, capBytes);
}

// Re-fetches a stale cached icon (RevalidateUrlIcon). A failure backs off like any other resolve failure, under
// a key of its own, so an unreachable server is asked once per backoff window, not once per eviction. One
// revalidation per URL runs at a time. Worker only.
void RevalidateIconOnWorker(const std::string& url) {
    const std::string key = std::string("revalidate:") + url;
    if (IconInBackoff(key) || !MarkIconInFlight(key)) {
        return;
    }
    const InFlightClaim claim(key);
    std::string cacheDir;
    std::string error;
    if (!SmatchetFieldIconDiskCache::EnsureCacheDir(ConfigManager::GetUserDataDirectory(), cacheDir, error)) {
        return;
    }
    const SmatchetFieldIconDiskCache::RevalidateResult result = SmatchetFieldIconDiskCache::RevalidateUrlIcon(
        cacheDir, url, CurrentIconFetchFn(), &SmatchetImageTextureCache::DecodeIconBytesAnyThread);
    if (result == SmatchetFieldIconDiskCache::RevalidateResult::Failed) {
        RecordIconFailure(key);
    } else {
        ClearIconFailure(key);
    }
}

/// Runs `resolve` on a worker and uploads the decoded pixels on the UI thread. UI thread only.
/// The caller has already checked the texture cache, the backoff and the in-flight set. The key is marked
/// in flight BEFORE the launch and released exactly once by InFlightClaim: after the upload, after a failed
/// resolve, if the launch throws, or if the task is destroyed unrun at shutdown. A failure is recorded in
/// the backoff map; a success posts exactly one completion (non-droppable, so the mark always clears).
void LaunchIconResolve(AppController& app, const std::string& key,
                       std::function<SmatchetFieldIconDiskCache::IconResolveOutcome()> resolve,
                       std::function<void(const SmatchetFieldIconDiskCache::IconResolveOutcome&)> afterShown =
                           std::function<void(const SmatchetFieldIconDiskCache::IconResolveOutcome&)>()) {
    if (!MarkIconInFlight(key)) {
        return; // another call won the race; its worker will warm the cache
    }
    const std::shared_ptr<InFlightClaim> claim = std::make_shared<InFlightClaim>(key);
    try {
        app.LaunchBackgroundTask([&app, key, resolve, afterShown, claim]() {
            SmatchetFieldIconDiskCache::IconResolveOutcome outcome;
            try {
                outcome = resolve();
            } catch (const std::exception& ex) {
                outcome.Ok = false;
                outcome.Error = ex.what();
            }
            if (!outcome.Ok) {
                // The key may carry a signed query string: log only the part before it. An optional bundled
                // icon that is simply absent is expected, so it is not a warning.
                const std::string shown = key.substr(0, key.find('?'));
                if (outcome.NotFound) {
                    LOG_DEBUG("FieldIcon: '%s' not found: %s", shown.c_str(), outcome.Error.c_str());
                } else {
                    LOG_WARN("FieldIcon: could not resolve '%s': %s", shown.c_str(), outcome.Error.c_str());
                }
                RecordIconFailure(key);
                claim->Release();
                return;
            }
            // The pixels move to the UI thread; keep what `afterShown` needs (the source) before they go.
            SmatchetFieldIconDiskCache::IconResolveOutcome shown;
            shown.Ok = true;
            shown.Source = outcome.Source;
            const std::shared_ptr<DecodedIcon> icon = std::make_shared<DecodedIcon>(std::move(outcome.Icon));
            app.mainThreadDispatcher.PostCompletionToMainThread([key, icon, claim]() {
                const Result<SmatchetLoadedIconTexture> uploaded =
                    SmatchetImageTextureCache::GetOrCreateFromRgba(key, icon->Rgba, icon->Width, icon->Height);
                if (uploaded.has_value()) {
                    ClearIconFailure(key);
                } else {
                    LOG_WARN("FieldIcon: texture upload failed for '%s': %s", key.substr(0, key.find('?')).c_str(),
                             uploaded.error().c_str());
                    RecordIconFailure(key);
                }
                claim->Release();
            });
            // Housekeeping runs after the icon is on its way, so it never delays the first draw.
            if (afterShown) {
                try {
                    afterShown(shown);
                } catch (const std::exception& ex) {
                    LOG_WARN("FieldIcon: follow-up for '%s' failed: %s", key.substr(0, key.find('?')).c_str(),
                             ex.what());
                }
            }
        });
    } catch (const std::exception& ex) {
        LOG_WARN("FieldIcon: could not start the resolve worker for '%s': %s", key.substr(0, key.find('?')).c_str(),
                 ex.what());
        RecordIconFailure(key);
        claim->Release();
    }
}

/// Loads the URL-keyed icon texture. Returns:
///   - `true` + populated `out` when the texture is already resident.
///   - `false` + `outDeferred=true` when a worker was started (or one is already running) to read the
///     cache file or fetch the icon, decode it and hand the pixels back; the caller MUST NOT negative-memo
///     this result — the texture will be resident on a later frame.
///   - `false` + `outDeferred=false` when the resolve failed recently and is backing off.
/// The UI thread does no file, network or decode work here: only the texture-cache lookup, the backoff and
/// in-flight checks, and the launch.
bool LoadOrFetchUrlImage(AppController& app, const std::string& url, SmatchetLoadedIconTexture& out,
                         std::string& outError, bool& outDeferred) {
    out = {};
    outError.clear();
    outDeferred = false;
    // Canonical cache key for URL-loaded icons: the caller's memo derives its key from the URL candidate.
    const std::string urlKey = std::string("url:") + url;
    if (SmatchetImageTextureCache::TryGetCached(urlKey, out)) {
        return true;
    }
    // DR28: a recent failure is reported as a genuine failure (not deferred), so the caller negative-memos
    // the cell. The backoff expires, so a transient outage recovers on a later frame.
    if (IconInBackoff(urlKey)) {
        outError = "Icon fetch in backoff after a prior failure.";
        return false;
    }
    if (IconInFlight(urlKey)) {
        outDeferred = true;
        return false;
    }
    const std::string capturedUrl = url;
    LaunchIconResolve(
        app, urlKey,
        [capturedUrl]() {
            /* PILLAR2_WORKER_ONLY */ // est-latency: ~10ms cache read, up to ~3000ms fetch — resolve worker.
            SmatchetFieldIconDiskCache::IconResolveOutcome failure;
            std::string cacheDir;
            if (!SmatchetFieldIconDiskCache::EnsureCacheDir(ConfigManager::GetUserDataDirectory(), cacheDir,
                                                            failure.Error)) {
                return failure;
            }
            PruneCacheDirOncePerSession(cacheDir);
            return SmatchetFieldIconDiskCache::ResolveUrlIcon(cacheDir, capturedUrl, CurrentIconFetchFn(),
                                                              &SmatchetImageTextureCache::DecodeIconBytesAnyThread);
        },
        [capturedUrl](const SmatchetFieldIconDiskCache::IconResolveOutcome& shown) {
            // A copy that came from disk may be old: re-fetch it once a week, after it has been drawn.
            if (shown.Source != SmatchetFieldIconDiskCache::IconSource::Disk) {
                return;
            }
            /* PILLAR2_WORKER_ONLY */ // est-latency: ~3000ms — runs on the resolve worker after the upload post.
            RevalidateIconOnWorker(capturedUrl);
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
    // Local-file branch. A resident texture is a cheap lookup; otherwise a worker reads (bounded) and decodes
    // the file and the UI thread uploads the pixels. A file that is missing or does not decode backs off like
    // a failed fetch, so it is not re-read every frame.
    const std::string cacheKey = std::string("file:") + resolved;
    if (SmatchetImageTextureCache::TryGetCached(cacheKey, out)) {
        return true;
    }
    if (IconInBackoff(cacheKey)) {
        outError = "Icon file could not be loaded recently.";
        return false;
    }
    if (IconInFlight(cacheKey)) {
        outDeferred = true;
        return false;
    }
    const std::string capturedResolved = resolved;
    LaunchIconResolve(app, cacheKey, [capturedResolved]() {
        /* PILLAR2_WORKER_ONLY */ // est-latency: ~10ms — bounded read + decode of a small icon file.
        return SmatchetFieldIconDiskCache::ResolveFileIcon(capturedResolved,
                                                           &SmatchetImageTextureCache::DecodeIconBytesAnyThread);
    });
    outDeferred = true;
    return false;
}

// Backwards-compat wrapper for the const-AppController call sites that don't need the deferred
// distinction (e.g. preview-only/non-priority paths). The resolve still defers to a worker; the caller sees
// `false` until the texture is resident.
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
 * cache lookup. Capped (limits::Get().PriorityResolveMemoMax, default 256) to bound memory if a
 * customer has many distinct values.
 */
size_t PriorityResolveMemoCap() { return static_cast<size_t>(smatchet::limits::Get().PriorityResolveMemoMax); }
struct PriorityResolveMemo {
    std::string CacheKey;       ///< Texture-cache key (empty iff Negative).
    std::string Label;          ///< Parsed friendly name; preserves tooltip text on the fast path.
    bool Negative = false;      ///< True when the load failed and we want to suppress future retries.
    long long NegativeAtMs = 0; ///< Steady-clock ms the negative was recorded; it expires on the DR28 window.
};

/// True while a negative memo is still inside the DR28 backoff window. An expired negative falls through to
/// the slow path so a transient failure (offline start, a server that was briefly down) can recover instead
/// of hiding the icon for the whole session.
bool NegativeMemoActive(const PriorityResolveMemo& memo) {
    if (!memo.Negative) {
        return false;
    }
    const long long backoffMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(kIconFetchNegativeBackoff).count();
    return !SmatchetFieldIconRender::ShouldAttemptIconResolve(
        true, memo.NegativeAtMs, SteadyMillis(std::chrono::steady_clock::now()), backoffMs);
}
std::unordered_map<std::string, PriorityResolveMemo>& PriorityRawValueToCacheKey() {
    static std::unordered_map<std::string, PriorityResolveMemo> m;
    return m;
}

void RememberPriorityResolution(const std::string& rawValue, const std::string& cacheKey, const std::string& label) {
    if (rawValue.empty() || cacheKey.empty()) {
        return;
    }
    auto& m = PriorityRawValueToCacheKey();
    if (m.size() >= PriorityResolveMemoCap()) {
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
    if (m.size() >= PriorityResolveMemoCap()) {
        m.clear();
    }
    PriorityResolveMemo entry;
    entry.Label = label;
    entry.Negative = true;
    entry.NegativeAtMs = SteadyMillis(std::chrono::steady_clock::now());
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

void SetIconFetchFnForTest(SmatchetFieldIconDiskCache::FetchFn fn) {
    std::lock_guard<std::mutex> lock(IconFetchFnMutex());
    IconFetchFnSlot() = fn ? std::move(fn) : ProductionFetch();
}

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
            // Negative entry inside its backoff window: return false without re-attempting. An expired
            // one falls through to the slow path.
            if (NegativeMemoActive(it->second)) {
                return false;
            }
            if (!it->second.Negative && SmatchetImageTextureCache::TryGetCached(it->second.CacheKey, outIcon)) {
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
            // DR28: a known-negative cell short-circuits — no re-parse, no re-fetch this frame — until its
            // backoff window ends; then the slow path retries it.
            if (NegativeMemoActive(it->second)) {
                return false;
            }
            if (!it->second.Negative && SmatchetImageTextureCache::TryGetCached(it->second.CacheKey, icon)) {
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
