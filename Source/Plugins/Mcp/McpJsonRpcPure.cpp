#include "McpJsonRpcPure.h"

#include "SmatchetDefaults.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <utility> // std::move (ToLowerAscii forwarder)

namespace smatchet {
namespace mcp {
namespace pure {

namespace detail {

// The duplication deviation that used to sit here is RETIRED (gate-blind-spot-sweep Slice 2):
// the body now delegates to the shared Core helper instead of re-rolling it, which was the
// deviation's whole subject. The NAME stays a published seam — declared in McpJsonRpcPure.h and
// unit-tested directly by tests/Plugins/Mcp/McpDispatch.test.cpp — so this is a forwarder, not a
// deletion. Purity pre-check: StringUtil.h is header-only and stdlib-only, so it adds no link
// dependency to this deliberately dependency-light *Pure TU (which already includes the Core
// header SmatchetDefaults.h, so Source/Core/include is on this TU's path in every rig).
std::string ToLowerAscii(std::string value) { return ToLowerAsciiCopy(std::move(value)); }

std::string TrimAsciiWhitespace(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string BasenameForDisplay(const std::string& path) {
    if (path.empty()) {
        return std::string();
    }
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

void AppendAllowlistedArgKvs(std::ostringstream& oss, const nlohmann::json& obj) {
    if (!obj.is_object()) {
        return;
    }
    static const char* kKeys[] = {"issue_key", "key", "id", "priority"};
    for (const char* k : kKeys) {
        if (!obj.contains(k)) {
            continue;
        }
        const nlohmann::json& v = obj.at(k);
        oss << ' ' << k << '=';
        if (v.is_string()) {
            oss << TruncateOneLine(v.get<std::string>(), 80);
        } else if (v.is_number_integer()) {
            oss << v.get<std::int64_t>();
        } else if (v.is_number_unsigned()) {
            oss << v.get<std::uint64_t>();
        } else if (v.is_number_float()) {
            oss << v.get<double>();
        } else if (v.is_boolean()) {
            oss << (v.get<bool>() ? "true" : "false");
        } else {
            try {
                oss << TruncateOneLine(v.dump(), 80);
            } catch (...) { // catch-all-ok: dump for logging
                oss << "?";
            }
        }
    }
}

} // namespace detail

// Bring the promoted detail helpers into pure-namespace lookup so the existing
// call sites below (NormalizeDomain, ExtractHostFromUrl, IsAllowedMcpOrigin,
// BuildRunLuaSummary, BuildToolCallSummary, …) resolve them unqualified exactly
// as they did when these were file-local anonymous-namespace functions.
using detail::AppendAllowlistedArgKvs;
using detail::BasenameForDisplay;
using detail::ToLowerAscii;
using detail::TrimAsciiWhitespace;

std::string TruncateOneLine(const std::string& s, std::size_t maxChars) {
    if (s.size() <= maxChars) {
        return s;
    }
    return s.substr(0, maxChars) + "...";
}

bool LooksLikeHttpUrl(const std::string& url) { return url.find("http://") == 0 || url.find("https://") == 0; }

std::string NormalizeDomain(const std::string& rawDomain) {
    std::string value = TrimAsciiWhitespace(rawDomain);
    if (value.find("://") != std::string::npos) {
        const size_t schemeSep = value.find("://");
        value = value.substr(schemeSep + 3);
    }
    const size_t slashPos = value.find('/');
    if (slashPos != std::string::npos) {
        value.resize(slashPos);
    }
    const size_t atPos = value.rfind('@');
    if (atPos != std::string::npos) {
        value = value.substr(atPos + 1);
    }
    const size_t colonPos = value.find(':');
    if (colonPos != std::string::npos) {
        value.resize(colonPos);
    }
    return ToLowerAscii(value);
}

std::string ExtractHostFromUrl(const std::string& url) {
    const size_t schemeSep = url.find("://");
    if (schemeSep == std::string::npos) {
        return std::string();
    }
    size_t hostStart = schemeSep + 3;
    if (hostStart >= url.size()) {
        return std::string();
    }
    size_t hostEnd = url.find_first_of("/?#", hostStart);
    std::string hostAndPort = url.substr(hostStart, hostEnd - hostStart);
    // Strip a leading userinfo segment ending at '@' -- curl/cpr connect to the
    // host AFTER the last '@', so the real connect-host is what must face the
    // allow-list. The attachment proxy already rejects userinfo outright via
    // UrlHasUserinfo, but stripping here keeps this helper honest for any other
    // caller, so a userinfo-then-host authority cannot be read as the userinfo.
    const size_t atPos = hostAndPort.rfind('@');
    if (atPos != std::string::npos) {
        hostAndPort = hostAndPort.substr(atPos + 1);
    }
    if (hostAndPort.empty()) {
        return std::string();
    }
    if (hostAndPort.front() == '[') {
        const size_t closeBracket = hostAndPort.find(']');
        if (closeBracket == std::string::npos) {
            return std::string();
        }
        return ToLowerAscii(hostAndPort.substr(1, closeBracket - 1));
    }
    const size_t colonPos = hostAndPort.find(':');
    if (colonPos != std::string::npos) {
        return ToLowerAscii(hostAndPort.substr(0, colonPos));
    }
    return ToLowerAscii(hostAndPort);
}

bool UrlHasUserinfo(const std::string& url) {
    const size_t schemeSep = url.find("://");
    if (schemeSep == std::string::npos) {
        return false;
    }
    const size_t authStart = schemeSep + 3;
    const size_t authEnd = url.find_first_of("/?#", authStart);
    const std::string authority =
        (authEnd == std::string::npos) ? url.substr(authStart) : url.substr(authStart, authEnd - authStart);
    return authority.find('@') != std::string::npos;
}

bool IsLoopbackAddress(const std::string& remoteAddr) {
    const std::string lowered = ToLowerAscii(TrimAsciiWhitespace(remoteAddr));
    return lowered == SmatchetDefaults::Mcp::kBindLocalhost || lowered == "::1" || lowered == "localhost" ||
           lowered == "::ffff:127.0.0.1";
}

namespace {

// Extract the bare host from a `Host:`-style authority (`host`, `host:port`,
// `[ipv6]`, `[ipv6]:port`). Lowercased + whitespace-trimmed. Returns "" on a
// malformed bracketed value (unterminated `[`) so the caller fails closed.
std::string HostFromAuthority(const std::string& authority) {
    const std::string trimmed = TrimAsciiWhitespace(authority);
    if (trimmed.empty()) {
        return std::string();
    }
    if (trimmed.front() == '[') {
        const size_t closeBracket = trimmed.find(']');
        if (closeBracket == std::string::npos) {
            return std::string();
        }
        return ToLowerAscii(trimmed.substr(1, closeBracket - 1));
    }
    const size_t colonPos = trimmed.find(':');
    if (colonPos != std::string::npos) {
        return ToLowerAscii(trimmed.substr(0, colonPos));
    }
    return ToLowerAscii(trimmed);
}

// True iff `bareHost` (already lowercased, no port/brackets) is a loopback
// literal a legitimate local client would send. Rejects a trailing dot so a
// FQDN-style `127.0.0.1.` / `localhost.` cannot slip past.
bool IsLoopbackHostLiteral(const std::string& bareHost) {
    if (bareHost.empty() || bareHost.back() == '.') {
        return false;
    }
    return bareHost == SmatchetDefaults::Mcp::kBindLocalhost || bareHost == "localhost" || bareHost == "::1";
}

} // namespace

bool IsLoopbackHostHeader(const std::string& hostHeader) {
    return IsLoopbackHostLiteral(HostFromAuthority(hostHeader));
}

bool IsAllowedMcpOrigin(const std::string& originHeader) {
    const std::string trimmed = TrimAsciiWhitespace(originHeader);
    if (trimmed.empty()) {
        return true; // no Origin -- legitimate local MCP client / same-origin tooling.
    }
    const std::string lowered = ToLowerAscii(trimmed);
    if (lowered == "null") {
        return true; // sandboxed / file:// origins serialize to the literal "null".
    }
    const size_t schemeSep = lowered.find("://");
    if (schemeSep == std::string::npos) {
        return false; // not a serialized http/https origin -- reject.
    }
    const std::string scheme = lowered.substr(0, schemeSep);
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    // An Origin has no path; the authority runs to the end of the string.
    const std::string authority = lowered.substr(schemeSep + 3);
    return IsLoopbackHostLiteral(HostFromAuthority(authority));
}

bool IsMcpHostOriginAllowed(const std::string& hostHeader, const std::string& originHeader, std::string& reason) {
    if (!IsLoopbackHostHeader(hostHeader)) {
        reason = "bad_host";
        return false;
    }
    if (!IsAllowedMcpOrigin(originHeader)) {
        reason = "bad_origin";
        return false;
    }
    return true;
}

bool ConstantTimeStringEquals(const std::string& a, const std::string& b) {
    const size_t n = std::max(a.size(), b.size());
    unsigned int diff = static_cast<unsigned int>(a.size() ^ b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = (i < a.size()) ? static_cast<unsigned char>(a[i]) : 0u;
        const unsigned char cb = (i < b.size()) ? static_cast<unsigned char>(b[i]) : 0u;
        diff |= static_cast<unsigned int>(ca ^ cb);
    }
    return diff == 0u;
}

bool IsAllowedAttachmentHost(const std::string& host, const std::string& trackerDomain) {
    if (host.empty() || trackerDomain.empty()) {
        return false;
    }
    if (host == trackerDomain) {
        return true;
    }
    const std::string TrackerSuffix = "." + trackerDomain;
    if (host.size() > TrackerSuffix.size() &&
        host.compare(host.size() - TrackerSuffix.size(), TrackerSuffix.size(), TrackerSuffix) == 0) {
        return true;
    }
    return host == "api.media.atlassian.com";
}

bool CanAcceptSseConnection(int currentActive) {
    if (currentActive < 0) {
        return false; // fail closed on a corrupted counter.
    }
    return currentActive < kMaxConcurrentSseConnections;
}

std::string ResolveSseCorsOrigin(const std::string& requestOrigin, bool loopbackBind) {
    // Re-vet instead of trusting the caller's earlier Authorize pass: the grant must stay
    // loopback-only even if a future route wires this up without the Host/Origin gate.
    if (requestOrigin.empty() || !loopbackBind || !IsAllowedMcpOrigin(requestOrigin)) {
        return std::string();
    }
    return requestOrigin;
}

nlohmann::json BuildRunLuaToolEntry() {
    return {{"name", "run_lua"},
            {"description", "Run a Lua snippet or Scripts/*.lua file with optional args."},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"mode", {{"type", "string"}, {"enum", {"snippet", "script"}}}},
                {"code", {{"type", "string"}}},
                {"script", {{"type", "string"}}},
                {"args", {{"type", "object"}}}}},
              {"required", {"mode"}}}}};
}

std::string BuildRunLuaSummary(const nlohmann::json& arguments) {
    if (!arguments.is_object()) {
        return std::string();
    }
    std::ostringstream oss;
    const std::string mode = arguments.value("mode", "");
    oss << "mode=" << mode;
    if (mode == "snippet") {
        const std::string code = arguments.value("code", std::string());
        oss << " code_len=" << code.size();
    } else if (mode == "script") {
        oss << " script=" << TruncateOneLine(BasenameForDisplay(arguments.value("script", std::string())), 48);
    }
    AppendAllowlistedArgKvs(oss, arguments.value("args", nlohmann::json::object()));
    return oss.str();
}

std::string BuildToolCallSummary(const std::string& toolName, const nlohmann::json& arguments) {
    std::ostringstream oss;
    oss << "tool=" << toolName;
    if (!arguments.is_object()) {
        return oss.str();
    }
    oss << " arg_keys=[";
    bool first = true;
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!first) {
            oss << ',';
        }
        first = false;
        oss << it.key();
    }
    oss << ']';
    AppendAllowlistedArgKvs(oss, arguments);
    return oss.str();
}

std::string ExtractJsonRpcErrorMessage(const nlohmann::json& jres, std::size_t maxLen) {
    if (!jres.contains("error")) {
        return std::string();
    }
    const nlohmann::json& e = jres["error"];
    if (!e.is_object()) {
        return TruncateOneLine(e.dump(), maxLen);
    }
    if (e.contains("message") && e["message"].is_string()) {
        return TruncateOneLine(e["message"].get<std::string>(), maxLen);
    }
    try {
        return TruncateOneLine(e.dump(), maxLen);
    } catch (...) { // catch-all-ok: dump for error summary logging
        return "error";
    }
}

} // namespace pure
} // namespace mcp
} // namespace smatchet
