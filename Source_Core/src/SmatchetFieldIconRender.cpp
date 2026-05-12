#include "SmatchetFieldIconRender.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "JsonParseUtil.h"
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
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

namespace fs = ghc::filesystem;

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

std::string JoinDomainAndPath(std::string domain, const std::string& path) {
    domain = TrimCopyAsciiWhitespace(domain);
    while (!domain.empty() && (domain.back() == '/' || domain.back() == '\\')) {
        domain.pop_back();
    }
    if (path.empty()) {
        return domain;
    }
    if (path[0] == '/') {
        return domain + path;
    }
    return domain + "/" + path;
}

bool SlugIsKnown(const std::string& s) {
    static const std::unordered_set<std::string> kKnown = {"blocker", "critical",   "high",    "highest", "low",
                                                             "lowest",  "major",      "medium",  "minor",   "trivial"};
    return kKnown.find(s) != kKnown.end();
}

std::string NormalizeSlugFromLabel(std::string label) {
    label = ToLowerAsciiCopy(TrimCopyAsciiWhitespace(label));
    std::replace_if(label.begin(), label.end(), [](char c) {
        return c == ' ' || c == '-' || c == '_';
    }, ' ');
    // collapse spaces -> single space then replace with nothing for "aggregate" style? use first token only
    std::string compact;
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] == ' ') {
            continue;
        }
        compact.push_back(label[i]);
    }
    if (SlugIsKnown(compact)) {
        return compact;
    }
    // try original spaced lower e.g. "wont fix" not in list — try label as single token lower
    label = ToLowerAsciiCopy(TrimCopyAsciiWhitespace(label));
    std::replace(label.begin(), label.end(), ' ', '_');
    if (SlugIsKnown(label)) {
        return label;
    }
    return {};
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

/** Absolute URL from Jira `iconUrl`, or relative path joined with Jira site domain. */
std::string ResolveJiraIconUrlReference(const std::string& iconUrl, const std::string& jiraDomain) {
    const std::string t = TrimCopyAsciiWhitespace(iconUrl);
    if (t.empty()) {
        return std::string();
    }
    if (t.rfind("https://", 0) == 0 || t.rfind("http://", 0) == 0) {
        return t;
    }
    if (t.rfind("//", 0) == 0) {
        return std::string("https:") + t;
    }
    if (!t.empty() && t[0] == '/') {
        return JoinDomainAndPath(jiraDomain, t);
    }
    return std::string();
}

bool ParsePriorityJson(const std::string& raw, std::string& outIconUrl, std::string& outLabel, std::string& outSlug) {
    outIconUrl.clear();
    outLabel.clear();
    outSlug.clear();
    const std::string trimmed = TrimCopyAsciiWhitespace(raw);
    if (trimmed.empty()) {
        return false;
    }
    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j) || !j.is_object()) {
        outLabel = trimmed;
        outSlug = NormalizeSlugFromLabel(outLabel);
        return !outSlug.empty() || !outLabel.empty();
    }
    if (j.contains("iconUrl") && j["iconUrl"].is_string()) {
        outIconUrl = j["iconUrl"].get<std::string>();
    }
    if (j.contains("name") && j["name"].is_string()) {
        outLabel = j["name"].get<std::string>();
    } else if (j.contains("value") && j["value"].is_string()) {
        outLabel = j["value"].get<std::string>();
    }
    if (!outLabel.empty()) {
        outSlug = NormalizeSlugFromLabel(outLabel);
    }
    return !outIconUrl.empty() || !outSlug.empty() || !outLabel.empty();
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

std::string UrlToCacheFileName(const std::string& url) {
    const std::size_t h = std::hash<std::string>{}(url);
    std::ostringstream oss;
    oss << "icon_" << std::hex << h << ".bin";
    return oss.str();
}

bool HttpGetBinary(const std::string& url, std::vector<unsigned char>& out, std::string& outError) {
    out.clear();
    outError.clear();
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

bool LoadOrFetchUrlImage(const std::string& url, SmatchetLoadedIconTexture& out, std::string& outError) {
    out = {};
    outError.clear();
    // Canonical cache key for URL-loaded icons. All paths through this function MUST
    // end up storing the texture under this key — otherwise the caller's memo (which
    // derives its key from the URL candidate) will look it up and miss, falling back
    // to the slow path every frame. This was a bug pre-PR: when the disk cache was
    // hit, the texture got stored under a "file:" key instead of "url:", causing
    // ~6ms / frame of wasted re-resolution work on a 18-cell priority column.
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
    // If we have the bytes on disk already, decode them and stash the texture under
    // the URL key (not a "file:" key) so subsequent lookups via the URL hit.
    std::error_code ec;
    if (fs::exists(fs::path(diskPath), ec) && fs::file_size(fs::path(diskPath), ec) > 0) {
        std::ifstream ifs(diskPath.c_str(), std::ios::binary);
        if (ifs) {
            std::vector<unsigned char> buf((std::istreambuf_iterator<char>(ifs)),
                                            std::istreambuf_iterator<char>());
            if (!buf.empty()) {
                return SmatchetImageTextureCache::GetOrLoadFromMemory(
                    urlKey, buf.data(), buf.size(), out, outError);
            }
        }
    }
    // Fetch from network and persist both bytes (disk) and texture (cache under url:).
    std::vector<unsigned char> bytes;
    if (!HttpGetBinary(url, bytes, outError)) {
        return false;
    }
    std::ofstream ofs(diskPath.c_str(), std::ios::binary | std::ios::trunc);
    if (ofs) {
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return SmatchetImageTextureCache::GetOrLoadFromMemory(urlKey, bytes.data(), bytes.size(), out, outError);
}

bool LoadTextureForResolvedPath(const std::string& resolved, SmatchetLoadedIconTexture& out, std::string& outError) {
    out = {};
    outError.clear();
    if (resolved.empty()) {
        outError = "Empty path.";
        return false;
    }
    if (resolved.rfind("https://", 0) == 0 || resolved.rfind("http://", 0) == 0) {
        return LoadOrFetchUrlImage(resolved, out, outError);
    }
    return SmatchetImageTextureCache::GetOrLoadFromFile(std::string("file:") + resolved, resolved, out, outError);
}

/**
 * Per-frame fast path: maps a raw priority value (Jira priority JSON blob) to the texture-cache key
 * that resolved successfully on a prior frame. Lets the grid hot path skip JSON parse, slug
 * normalization, candidate building and disk stat — collapses to one hashmap lookup + the locked
 * cache lookup. Capped to bound memory if a customer has many distinct values.
 */
constexpr size_t kPriorityResolveMemoCap = 256;
struct PriorityResolveMemo {
    std::string CacheKey;       ///< Texture-cache key (empty iff Negative).
    std::string Label;          ///< Parsed friendly name; preserves tooltip text on the fast path.
    bool        Negative = false; ///< True when the load failed and we want to suppress future retries.
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
    if (rawValue.empty()) return;
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
bool LoadPriorityIconWithFallbacks(const std::string& iconUrl, const std::string& slug, SmatchetLoadedIconTexture& out,
                                   std::string& outResolvedCacheKey, std::string& outLastError) {
    out = {};
    outLastError.clear();
    outResolvedCacheKey.clear();
    const std::string domain = GetCachedJiraDomainForPriorityIcons();
    std::vector<std::string> candidates;
    auto pushUnique = [&](const std::string& s) {
        if (s.empty()) {
            return;
        }
        if (std::any_of(candidates.begin(), candidates.end(), [&](const auto& existing) {
            return existing == s;
        })) {
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
        if (LoadTextureForResolvedPath(c, out, err)) {
            const bool isUrl = c.rfind("https://", 0) == 0 || c.rfind("http://", 0) == 0;
            outResolvedCacheKey = (isUrl ? std::string("url:") : std::string("file:")) + c;
            return true;
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
    if (!LoadTextureForResolvedPath(resolved, icon, err)) {
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
        if (LoadTextureForResolvedPath(loadPath, outIcon, outError)) {
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
    if (!LoadPriorityIconWithFallbacks(iconUrl, slug, outIcon, resolvedKey, outError)) {
        // Load failed across all fallback paths — remember the failure so subsequent frames
        // short-circuit at the memo lookup above instead of repeating the full work.
        RememberNegativePriorityResolution(rawValue, label);
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
    if (!allowCellEdits &&
        app.TryGetFieldIconMapTarget(fieldId, field, rawValue, pathOrUrl)) {
        const std::string resolved = app.ResolveFieldIconAssetPath(pathOrUrl);
        const std::string& loadPath = resolved.empty() ? pathOrUrl : resolved;
        std::string err;
        SmatchetLoadedIconTexture icon;
        if (LoadTextureForResolvedPath(loadPath, icon, err)) {
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
        if (it != memo.end() && SmatchetImageTextureCache::TryGetCached(it->second.CacheKey, icon)) {
            const float maxEdge = ImGui::GetFrameHeight();
            const std::string& memoLabel = it->second.Label;
            DrawLoadedIconWithLazyTooltip(icon, maxEdge, tooltipsEnabled, [&]() {
                return field ? DisplayValueForTrackerDateField(fieldId, field, rawValue)
                             : (memoLabel.empty() ? rawValue : memoLabel);
            });
            return true;
        }
    }

    std::string iconUrl;
    std::string label;
    std::string slug;
    if (!ParsePriorityJson(rawValue, iconUrl, label, slug)) {
        return false;
    }

    std::string err;
    std::string resolvedKey;
    if (!LoadPriorityIconWithFallbacks(iconUrl, slug, icon, resolvedKey, err)) {
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





