#include "SmatchetFieldIconRender_detail.h"

#include "JsonParseUtil.h"
#include "StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <sstream>
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

std::string UrlToCacheFileName(const std::string& url) {
    const std::size_t h = std::hash<std::string>{}(url);
    std::ostringstream oss;
    oss << "icon_" << std::hex << h << ".bin";
    return oss.str();
}

} // namespace detail
} // namespace SmatchetFieldIconRender
