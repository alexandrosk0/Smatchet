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
    std::string cacheDir;
    if (!EnsureFieldIconsCacheDir(cacheDir, outError)) {
        return false;
    }
    const std::string fileName = UrlToCacheFileName(url);
    const std::string diskPath = cacheDir + fileName;
    std::error_code ec;
    if (fs::exists(fs::path(diskPath), ec) && fs::file_size(fs::path(diskPath), ec) > 0) {
        return SmatchetImageTextureCache::GetOrLoadFromFile(std::string("file:") + diskPath, diskPath, out, outError);
    }
    std::vector<unsigned char> bytes;
    if (!HttpGetBinary(url, bytes, outError)) {
        return false;
    }
    std::ofstream ofs(diskPath.c_str(), std::ios::binary | std::ios::trunc);
    if (ofs) {
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return SmatchetImageTextureCache::GetOrLoadFromMemory(std::string("url:") + url, bytes.data(), bytes.size(), out,
                                                          outError);
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
 * Load priority icon: Jira `iconUrl` (absolute or `/...` on your site), then same slugs as the admin
 * priorities page (`<domain>/images/icons/priorities/<slug>.png`), then bundled offline fallback.
 */
bool LoadPriorityIconWithFallbacks(const std::string& iconUrl, const std::string& slug, SmatchetLoadedIconTexture& out,
                                   std::string& outLastError) {
    out = {};
    outLastError.clear();
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

void DrawLoadedIconOnly(const SmatchetLoadedIconTexture& icon, float maxEdge, bool tooltipsEnabled,
                        const std::string& tooltip) {
    DrawLoadedIconSized(icon, maxEdge);
    if (tooltipsEnabled && !tooltip.empty() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
        ImGui::TextUnformatted(tooltip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
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
    std::string iconUrl;
    std::string label;
    std::string slug;
    if (!ParsePriorityJson(rawValue, iconUrl, label, slug)) {
        return false;
    }
    return LoadPriorityIconWithFallbacks(iconUrl, slug, outIcon, outError);
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
            const std::string display =
                field ? DisplayValueForTrackerDateField(fieldId, field, rawValue) : std::string(rawValue);
            DrawLoadedIconOnly(icon, maxEdge, tooltipsEnabled, display);
            return true;
        }
    }
#endif

    if (fieldId != "priority" && !(field != nullptr && field->Id == "priority")) {
        return false;
    }

    std::string iconUrl;
    std::string label;
    std::string slug;
    if (!ParsePriorityJson(rawValue, iconUrl, label, slug)) {
        return false;
    }

    std::string err;
    SmatchetLoadedIconTexture icon;
    if (!LoadPriorityIconWithFallbacks(iconUrl, slug, icon, err)) {
        return false;
    }

    const float maxEdge = ImGui::GetFrameHeight();
    const std::string display =
        field ? DisplayValueForTrackerDateField(fieldId, field, rawValue) : std::string(label.empty() ? rawValue : label);
    DrawLoadedIconOnly(icon, maxEdge, tooltipsEnabled, display);
    return true;
}

} // namespace SmatchetFieldIconRender





