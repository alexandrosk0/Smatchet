#pragma once

// Pure, ImGui/cpr-free resolution helpers extracted from SmatchetFieldIconRender.cpp
// (gap-map Tier 5 `_detail` pattern). This is the untrusted-input half of the priority-icon
// pipeline: tracker-supplied priority JSON parsing, slug normalisation, and the
// CPP_CODE_AUDIT.md #14 same-origin confinement that keeps a hostile `iconUrl` from steering
// the icon HTTP GET at an arbitrary host. The I/O shell (texture cache, cpr fetch, ImGui
// draw) stays in SmatchetFieldIconRender.cpp.

#include <string>

namespace SmatchetFieldIconRender {
namespace detail {

/** `domain` with trailing slashes/backslashes stripped, joined with `path` by exactly one '/'. */
std::string JoinDomainAndPath(std::string domain, const std::string& path);

/** True when `s` is one of the known Jira priority slugs (blocker, critical, high, …). */
bool SlugIsKnown(const std::string& s);

/** Known priority slug derived from a human label ("Won't Fix", "high", "In-Progress" style
 *  separators tolerated), or empty when the label maps to no known slug. */
std::string NormalizeSlugFromLabel(std::string label);

/** Lowercased host (no scheme, no port, no path) from an absolute "scheme://host[:port]/..."
 *  URL. Empty on a schemeless/malformed input. CPP_CODE_AUDIT.md #14 — same-origin comparison
 *  only; not a general URL parser. */
std::string ExtractUrlHost(const std::string& url);

/** True when `absoluteUrl`'s host matches `jiraDomain`'s host. CPP_CODE_AUDIT.md #14: a
 *  malicious/compromised tracker's priority `iconUrl` must not be able to drive the icon
 *  fetch at an arbitrary host (e.g. the cloud-metadata IP). */
bool IconUrlHostAllowed(const std::string& absoluteUrl, const std::string& jiraDomain);

/** Absolute URL from a Jira `iconUrl` (same-origin enforced via IconUrlHostAllowed, including
 *  the protocol-relative `//host/...` shape), or a `/...` reference joined with the Jira site
 *  domain. Empty when the reference is empty, malformed, or off-origin. */
std::string ResolveJiraIconUrlReference(const std::string& iconUrl, const std::string& jiraDomain);

/** Parse a tracker-supplied priority cell value: a (possibly double-encoded) JSON object with
 *  `iconUrl` + `name`/`value`, or a bare label. Returns false only when nothing usable was
 *  extracted; a non-JSON value degrades to label(+slug). */
bool ParsePriorityJson(const std::string& raw, std::string& outIconUrl, std::string& outLabel, std::string& outSlug);

/** Stable on-disk cache filename for a fetched icon URL ("icon_<hash-hex>.bin"). */
std::string UrlToCacheFileName(const std::string& url);

} // namespace detail
} // namespace SmatchetFieldIconRender
