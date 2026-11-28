#include "SmatchetFieldIconRender_detail.h"

#include "JsonParseUtil.h"
#include "Sha256.h"
#include "StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <unordered_set>

namespace SmatchetFieldIconRender {
namespace detail {

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
    static const std::unordered_set<std::string> kKnown = {"blocker", "critical", "high",   "highest", "low",
                                                           "lowest",  "major",    "medium", "minor",   "trivial"};
    return kKnown.find(s) != kKnown.end();
}

std::string NormalizeSlugFromLabel(std::string label) {
    label = ToLowerAsciiCopy(TrimCopyAsciiWhitespace(label));
    std::replace_if(label.begin(), label.end(), [](char c) { return c == ' ' || c == '-' || c == '_'; }, ' ');
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

std::string ExtractUrlHost(const std::string& url) {
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        return std::string();
    }
    const std::size_t hostStart = schemeEnd + 3;
    std::size_t hostEnd = url.find_first_of("/?#", hostStart);
    if (hostEnd == std::string::npos) {
        hostEnd = url.size();
    }
    std::string hostPort = url.substr(hostStart, hostEnd - hostStart);
    // Strip userinfo (everything through the last '@') BEFORE the port handling below —
    // otherwise "https://jira.example.com:443@evil.com/p.png" is truncated at the
    // userinfo's ':' to the tracker's own host and the fetch escapes to evil.com
    // (SSRF bypass of the audit-#14 same-origin confinement).
    const std::size_t atSign = hostPort.rfind('@');
    if (atSign != std::string::npos) {
        hostPort = hostPort.substr(atSign + 1);
    }
    if (!hostPort.empty() && hostPort.front() == '[') {
        // Bracketed IPv6 literal — keep the brackets, drop any trailing ":port".
        const std::size_t close = hostPort.find(']');
        if (close != std::string::npos) {
            hostPort = hostPort.substr(0, close + 1);
        }
    } else {
        const std::size_t colon = hostPort.rfind(':');
        if (colon != std::string::npos) {
            hostPort = hostPort.substr(0, colon);
        }
    }
    return ToLowerAsciiCopy(std::move(hostPort));
}

bool IconUrlHostAllowed(const std::string& absoluteUrl, const std::string& jiraDomain) {
    const std::string urlHost = ExtractUrlHost(absoluteUrl);
    if (urlHost.empty()) {
        return false; // not a well-formed absolute URL — reject rather than guess
    }
    const std::string domainWithScheme =
        jiraDomain.find("://") != std::string::npos ? jiraDomain : "https://" + jiraDomain;
    const std::string domainHost = ExtractUrlHost(domainWithScheme);
    return !domainHost.empty() && urlHost == domainHost;
}

std::string ResolveJiraIconUrlReference(const std::string& iconUrl, const std::string& jiraDomain) {
    const std::string t = TrimCopyAsciiWhitespace(iconUrl);
    if (t.empty()) {
        return std::string();
    }
    if (t.rfind("https://", 0) == 0 || t.rfind("http://", 0) == 0) {
        return IconUrlHostAllowed(t, jiraDomain) ? t : std::string();
    }
    if (t.rfind("//", 0) == 0) {
        const std::string absolute = std::string("https:") + t;
        return IconUrlHostAllowed(absolute, jiraDomain) ? absolute : std::string();
    }
    if (!t.empty() && t[0] == '/') {
        return JoinDomainAndPath(jiraDomain, t); // relative — already same-origin by construction
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

std::string UrlToCacheFileName(const std::string& url) { return "icon_" + smatchet::hashing::Sha256Hex(url) + ".bin"; }

namespace {

bool IsLowerHexRun(const std::string& s, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// "icon_" + `hexLen` lowercase hex digits + ".bin"; `minHex`..`maxHex` bounds the digit count.
bool MatchesIconName(const std::string& name, std::size_t minHex, std::size_t maxHex) {
    constexpr std::size_t kPrefix = 5; // "icon_"
    constexpr std::size_t kSuffix = 4; // ".bin"
    if (name.size() < kPrefix + minHex + kSuffix || name.size() > kPrefix + maxHex + kSuffix ||
        name.compare(0, kPrefix, "icon_") != 0 || name.compare(name.size() - kSuffix, kSuffix, ".bin") != 0) {
        return false;
    }
    return IsLowerHexRun(name, kPrefix, name.size() - kSuffix);
}

} // namespace

bool IsIconCacheFileName(const std::string& name) { return MatchesIconName(name, 64, 64); }

bool IsLegacyIconCacheFileName(const std::string& name) { return MatchesIconName(name, 1, 16); }

} // namespace detail
} // namespace SmatchetFieldIconRender
