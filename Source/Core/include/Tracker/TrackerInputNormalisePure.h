#pragma once

// Tolerant reading of the connection and tool fields in Preferences: a tracker site pasted as a full
// browser URL, a GitHub repository URL dropped into Owner, an executable path copied with quotes, a
// chat URL without its scheme (INPUT_UX_AUDIT.md #80, #82, #128). Each function returns the value to
// store, whether that differs from what was typed (so the field can show "Using …"), and a
// ParseMessage when the text cannot be used. Pure: no I/O, no ImGui.

#include "TrackerFieldValueParseExplain.h"

#include <string>

namespace TrackerInputNormalise {

/// Which Preferences field the text was typed into.
enum class SiteField : unsigned char {
    JiraDomain,    ///< a host ("acme.atlassian.net"), optionally a server context path; no https://
    PlaneUrl,      ///< an API base URL ("https://api.plane.so")
    GitHubBaseUrl, ///< an API base URL ("https://api.github.com", "https://ghe.corp/api/v3")
    GitHubOwner,   ///< an owner name; "owner/repo" or a repository URL also fills Repo
};

struct SiteUrlParse {
    bool Ok = false;
    bool Rewritten = false;    ///< Value differs from the trimmed input
    std::string Value;         ///< what to store in the field ("" when nothing was typed)
    std::string Owner;         ///< GitHub owner found in a pasted URL (may be empty)
    std::string Repo;          ///< GitHub repository found in a pasted URL (may be empty)
    std::string WorkspaceSlug; ///< Plane workspace found in a pasted URL (may be empty)
    TrackerParseExplain::ParseMessage Message;
};

/// Normalise a site field. For a Jira domain, a browser URL is cut back to its host: the whole path on
/// `*.atlassian.net`, and on a server only from the first Jira route (`/browse/`, `/secure/`, `/rest/`,
/// `/projects/`, `/issues/`, `/plugins/`, `/servicedesk/`, `/jira/software`, …), so a context path such as
/// `/jira` stays. "https://" is dropped (it is the default); an explicit "http://" is kept. For Plane,
/// `app.plane.so/<workspace>/…` becomes `https://api.plane.so` plus the workspace. For GitHub, a
/// `github.com/<owner>/<repo>` URL becomes `https://api.github.com` plus owner and repo; a GitHub Enterprise
/// repository URL becomes `<host>/api/v3`. An empty input is Ok with an empty Value.
SiteUrlParse NormaliseSiteField(const std::string& input, SiteField field);

struct PathParse {
    bool Ok = false;
    bool Rewritten = false;
    std::string Value;
    TrackerParseExplain::ParseMessage Message;
};

/// Trim an executable path and strip the quotes "Copy as path" adds. An empty path is Ok (the tool is then
/// looked up on PATH). A path ending in a separator names a folder and is refused, as is a line break.
PathParse NormaliseExecutablePath(const std::string& input);

/// An optional http(s) URL (the annotate "AI chat URL"). Empty is Ok. A missing or non-http(s) scheme and
/// whitespace are refused, because the link opener would refuse them later without a word.
PathParse ValidateHttpUrl(const std::string& input);

} // namespace TrackerInputNormalise
