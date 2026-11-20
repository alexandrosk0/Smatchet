#include "TrackerInputNormalisePure.h"

#include "StringUtil.h"

#include <cstddef>
#include <string>
#include <vector>

namespace TrackerInputNormalise {

namespace {

using TrackerParseExplain::ParseMessage;
using TrackerParseExplain::ParseMessageId;

ParseMessage Message(ParseMessageId id, const std::string& arg) {
    return TrackerParseExplain::MakeParseMessage(id, arg, 0, arg.size());
}

bool HasWhitespace(const std::string& s) {
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            return true;
        }
    }
    return false;
}

/// Trims, then strips one pair of surrounding quotes or angle brackets (a pasted "<https://…>").
std::string Unwrap(const std::string& input) {
    std::string s = TrimCopyAsciiWhitespace(input);
    if (s.size() >= 2) {
        const char a = s.front();
        const char b = s.back();
        if ((a == '"' && b == '"') || (a == '\'' && b == '\'') || (a == '<' && b == '>')) {
            s = TrimCopyAsciiWhitespace(s.substr(1, s.size() - 2));
        }
    }
    return s;
}

bool IsHostChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':' || c == '_' ||
           c == '[' || c == ']';
}

bool ValidHost(const std::string& host) {
    if (host.empty() || host.front() == '.' || host.front() == '-') {
        return false;
    }
    for (char c : host) {
        if (!IsHostChar(c)) {
            return false;
        }
    }
    return true;
}

/// A path segment where Jira's own pages start; everything from here on is not part of the site.
bool IsJiraRouteSegment(const std::vector<std::string>& seg, std::size_t i) {
    static const char* const kRoutes[] = {"browse",  "secure",      "rest",  "projects",  "issues",
                                          "plugins", "servicedesk", "issue", "login.jsp", "dashboard.jsp"};
    static const char* const kJiraApps[] = {"software",   "core",     "servicemanagement", "servicedesk", "your-work",
                                            "dashboards", "projects", "for-you",           "people",      "plans",
                                            "filters",    "apps",     "settings",          "boards",      "issues"};
    const std::string s = ToLowerAsciiCopy(seg[i]);
    for (const char* r : kRoutes) {
        if (s == r) {
            return true;
        }
    }
    if (s == "jira" && i + 1 < seg.size()) {
        const std::string next = ToLowerAsciiCopy(seg[i + 1]);
        for (const char* a : kJiraApps) {
            if (next == a) {
                return true;
            }
        }
    }
    return EndsWithAscii(s, ".jsp") || EndsWithAscii(s, ".jspa");
}

SiteUrlParse Failure(ParseMessageId id, const std::string& arg) {
    SiteUrlParse r;
    r.Message = Message(id, arg);
    return r;
}

/// The checks every site field shares. Returns false (with `out` set) when the text cannot be a site.
bool CheckSite(const std::string& s, TrackerParseExplain::UrlParts& site, SiteUrlParse& out) {
    if (HasWhitespace(s)) {
        out = Failure(ParseMessageId::SiteUrlHasSpaces, s);
        return false;
    }
    site = TrackerParseExplain::SplitUrl(s);
    if (!site.Scheme.empty() && site.Scheme != "http" && site.Scheme != "https") {
        out = Failure(ParseMessageId::SiteUrlNotHttp, site.Scheme);
        return false;
    }
    if (!ValidHost(site.Host)) {
        out = Failure(ParseMessageId::SiteUrlNoHost, s);
        return false;
    }
    return true;
}

SiteUrlParse Success(const std::string& typed, const std::string& value) {
    SiteUrlParse r;
    r.Ok = true;
    r.Value = value;
    r.Rewritten = value != typed;
    return r;
}

SiteUrlParse NormaliseJiraDomain(const std::string& s, const TrackerParseExplain::UrlParts& site) {
    std::string value = site.Scheme == "http" ? "http://" + site.Host : site.Host;
    if (!EndsWithAscii(site.Host, ".atlassian.net") && !EndsWithAscii(site.Host, ".jira.com")) {
        for (std::size_t i = 0; i < site.Segments.size() && !IsJiraRouteSegment(site.Segments, i); ++i) {
            value += "/" + site.Segments[i]; // a server context path, such as /jira
        }
    }
    return Success(s, value);
}

std::string SchemeOrHttps(const TrackerParseExplain::UrlParts& site) {
    return site.Scheme.empty() ? std::string("https") : site.Scheme;
}

// First path segments of the Plane web app that name a screen, not a workspace.
bool IsPlaneRouteSegment(const std::string& segment) {
    static const char* const kRoutes[] = {"projects", "issues",   "cycles",    "modules", "views",
                                          "pages",    "settings", "analytics", "profile", "workspace-views"};
    const std::string lower = ToLowerAsciiCopy(segment);
    for (const char* route : kRoutes) {
        if (lower == route) {
            return true;
        }
    }
    return false;
}

// Plane cloud keeps the workspace in the first path segment and serves its API from api.plane.so. Any other host
// is a self-hosted install: its path is kept (it may be a reverse-proxy prefix such as /plane), and the web
// app's screen route marks where the workspace sits (".../<workspace>/projects/...").
SiteUrlParse NormalisePlaneUrl(const std::string& s, const TrackerParseExplain::UrlParts& site) {
    const std::vector<std::string>& seg = site.Segments;
    if (site.Host == "app.plane.so" || site.Host == "api.plane.so") {
        SiteUrlParse r = Success(s, "https://api.plane.so");
        if (site.Host == "app.plane.so" && !seg.empty()) {
            r.WorkspaceSlug = seg[0];
        }
        return r;
    }
    std::size_t route = seg.size();
    for (std::size_t i = 0; i < seg.size(); ++i) {
        if (IsPlaneRouteSegment(seg[i])) {
            route = i;
            break;
        }
    }
    const std::size_t keep = route < seg.size() ? (route > 0 ? route - 1 : 0) : seg.size();
    std::string base = SchemeOrHttps(site) + "://" + site.Host;
    for (std::size_t i = 0; i < keep; ++i) {
        base += "/" + seg[i];
    }
    SiteUrlParse r = Success(s, base);
    if (route < seg.size() && route > 0) {
        r.WorkspaceSlug = seg[route - 1];
    }
    return r;
}

std::string StripGitSuffix(const std::string& repo) {
    return EndsWithAscii(repo, ".git") ? repo.substr(0, repo.size() - 4) : repo;
}

SiteUrlParse NormaliseGitHubBase(const std::string& s, const TrackerParseExplain::UrlParts& site) {
    const bool dotCom = site.Host == "github.com" || site.Host == "www.github.com" || site.Host == "api.github.com";
    const std::vector<std::string>& seg = site.Segments;
    const bool apiPath = seg.size() >= 2 && seg[0] == "api" && seg[1] == "v3";
    const bool repoPath = seg.size() >= 2 && seg[0] != "api" && site.Host != "api.github.com";
    std::string base;
    if (dotCom) {
        base = "https://api.github.com";
    } else if (apiPath || repoPath) {
        base = SchemeOrHttps(site) + "://" + site.Host + "/api/v3";
    } else {
        base = (site.Scheme.empty() ? std::string() : site.Scheme + "://") + site.Host;
        for (const std::string& part : seg) {
            base += "/" + part;
        }
    }
    SiteUrlParse r = Success(s, base);
    if (repoPath) {
        r.Owner = seg[0];
        r.Repo = StripGitSuffix(seg[1]);
    }
    return r;
}

bool IsOwnerChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.';
}

bool ValidOwnerOrRepo(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (!IsOwnerChar(c)) {
            return false;
        }
    }
    return true;
}

SiteUrlParse NormaliseGitHubOwner(const std::string& s) {
    if (HasWhitespace(s)) {
        return Failure(ParseMessageId::OwnerRepoShape, s);
    }
    const bool url = TrackerParseExplain::LooksLikeUrl(s);
    // "owner" and "owner/repo" reuse the URL segment split behind a dummy host.
    const std::vector<std::string> parts =
        url ? TrackerParseExplain::SplitUrl(s).Segments : TrackerParseExplain::SplitUrl("x/" + s).Segments;
    const bool tooManyParts = !url && parts.size() > 2;
    const bool badRepo = parts.size() >= 2 && !ValidOwnerOrRepo(StripGitSuffix(parts[1]));
    if (parts.empty() || tooManyParts || !ValidOwnerOrRepo(parts[0]) || badRepo) {
        return Failure(ParseMessageId::OwnerRepoShape, s);
    }
    SiteUrlParse r = Success(s, parts[0]);
    r.Owner = parts[0];
    if (parts.size() >= 2) {
        r.Repo = StripGitSuffix(parts[1]);
    }
    return r;
}

} // namespace

SiteUrlParse NormaliseSiteField(const std::string& input, SiteField field) {
    const std::string typed = TrimCopyAsciiWhitespace(input);
    const std::string s = Unwrap(input);
    SiteUrlParse r;
    if (s.empty()) {
        r.Ok = true;
        return r;
    }
    TrackerParseExplain::UrlParts site;
    if (field == SiteField::GitHubOwner) {
        r = NormaliseGitHubOwner(s);
    } else if (!CheckSite(s, site, r)) {
        return r;
    } else if (field == SiteField::JiraDomain) {
        r = NormaliseJiraDomain(s, site);
    } else if (field == SiteField::PlaneUrl) {
        r = NormalisePlaneUrl(s, site);
    } else {
        r = NormaliseGitHubBase(s, site);
    }
    // A change the user should see is any difference from what is in the field, quotes included.
    r.Rewritten = r.Ok && r.Value != typed;
    return r;
}

PathParse NormaliseExecutablePath(const std::string& input) {
    PathParse r;
    const std::string trimmed = TrimCopyAsciiWhitespace(input);
    const std::string s = Unwrap(input);
    if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos) {
        r.Message = Message(ParseMessageId::PathHasLineBreak, std::string());
        return r;
    }
    if (!s.empty() && (s.back() == '/' || s.back() == '\\')) {
        r.Message = Message(ParseMessageId::PathIsFolder, s);
        return r;
    }
    r.Ok = true;
    r.Value = s;
    r.Rewritten = s != trimmed; // quotes stripped; surrounding whitespace alone is not worth a note
    return r;
}

PathParse ValidateHttpUrl(const std::string& input) {
    PathParse r;
    const std::string s = Unwrap(input);
    if (s.empty()) {
        r.Ok = true;
        return r;
    }
    if (HasWhitespace(s)) {
        r.Message = Message(ParseMessageId::SiteUrlHasSpaces, s);
        return r;
    }
    const std::size_t scheme = s.find("://");
    if (scheme == std::string::npos) {
        r.Message = Message(ParseMessageId::HttpUrlMissingScheme, s);
        return r;
    }
    const std::string name = ToLowerAsciiCopy(s.substr(0, scheme));
    if (name != "http" && name != "https") {
        r.Message = Message(ParseMessageId::HttpUrlUnsupportedScheme, name);
        return r;
    }
    if (!ValidHost(TrackerParseExplain::SplitUrl(s).Host)) {
        r.Message = Message(ParseMessageId::SiteUrlNoHost, s);
        return r;
    }
    r.Ok = true;
    r.Value = s;
    r.Rewritten = s != TrimCopyAsciiWhitespace(input);
    return r;
}

} // namespace TrackerInputNormalise
