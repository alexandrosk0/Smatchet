#include "ModelDownloadPolicy.h"

#include "StringUtil.h"

#include <algorithm>
#include <cctype>

namespace smatchet {
namespace whisper {
namespace policy {

namespace {

// Suffix match on a dotted-domain boundary: `host` equals `suffix` OR ends with
// `"." + suffix`. Rejects `evilhuggingface.co` against `huggingface.co`.
bool HostMatchesDomain(const std::string& host, const std::string& suffix) {
    if (host == suffix) {
        return true;
    }
    if (host.size() <= suffix.size()) {
        return false;
    }
    const std::size_t off = host.size() - suffix.size();
    return host[off - 1] == '.' && host.compare(off, suffix.size(), suffix) == 0;
}

} // namespace

std::string ExtractHost(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return std::string();
    }
    std::string rest = url.substr(scheme + 3);
    // Cut at the first path/query/fragment delimiter.
    const std::size_t pathCut = rest.find_first_of("/?#");
    if (pathCut != std::string::npos) {
        rest = rest.substr(0, pathCut);
    }
    // Drop userinfo.
    const std::size_t at = rest.find('@');
    if (at != std::string::npos) {
        rest = rest.substr(at + 1);
    }
    // Drop a trailing :port (catalog hosts are not bracketed IPv6).
    const std::size_t colon = rest.rfind(':');
    if (colon != std::string::npos && rest.find(']') == std::string::npos) {
        rest = rest.substr(0, colon);
    }
    return ToLowerAsciiCopy(rest);
}

bool IsAllowedModelHost(const std::string& host) {
    if (host.empty()) {
        return false;
    }
    const std::string h = ToLowerAsciiCopy(host);
    return HostMatchesDomain(h, "huggingface.co") || HostMatchesDomain(h, "hf.co");
}

bool IsAllowedModelUrl(const std::string& url) {
    // https-only: a redirect that downgrades to cleartext is refused.
    if (ToLowerAsciiCopy(url).compare(0, 8, "https://") != 0) {
        return false;
    }
    return IsAllowedModelHost(ExtractHost(url));
}

bool ExceedsModelSizeCap(std::uint64_t bytes) { return bytes > kMaxModelDownloadBytes; }

} // namespace policy
} // namespace whisper
} // namespace smatchet
