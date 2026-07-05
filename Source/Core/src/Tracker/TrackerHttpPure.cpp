#include "TrackerHttpPure.h"

#include "AiErrorRedact.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace TrackerHttpPure {

namespace {

// Lowercase + trim surrounding whitespace. C++14, ASCII-only.
std::string Trimmed(const std::string& in) {
    std::size_t b = 0;
    std::size_t e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) {
        --e;
    }
    return in.substr(b, e - b);
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Strip scheme, userinfo, path/query, and an optional :port from a base URL, leaving the
// bare host. Handles bracketed IPv6 (`[::1]:443`). Returns lowercased host.
std::string HostFromBase(const std::string& rawBase) {
    std::string s = Trimmed(rawBase);
    const std::size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        s = s.substr(scheme + 3);
    }
    // Cut at the first path/query/fragment delimiter.
    const std::size_t pathCut = s.find_first_of("/?#");
    if (pathCut != std::string::npos) {
        s = s.substr(0, pathCut);
    }
    // Drop userinfo (user:pass@host).
    const std::size_t at = s.find('@');
    if (at != std::string::npos) {
        s = s.substr(at + 1);
    }
    return ToLower(s);
}

} // namespace

SslConfig ResolveSslConfig(const std::string& caBundlePath) {
    SslConfig out;
    if (caBundlePath.empty()) {
        return out; // {"", false} — keep libcurl's default CAINFO (desktop / system store).
    }
    out.caInfoPath = caBundlePath;
    out.attach = true;
    return out;
}

namespace {
// Set once at boot before the first tracker HTTP request, then read-only for the process
// lifetime. Function-local static so init order is well-defined across TUs; not synchronized
// (set-once-before-use contract — see header).
std::string& CaBundlePathStorage() {
    static std::string path;
    return path;
}
} // namespace

void SetCaBundlePath(const std::string& path) { CaBundlePathStorage() = path; }

const std::string& GetCaBundlePath() { return CaBundlePathStorage(); }

bool IsLoopbackHost(const std::string& host) {
    std::string h = HostFromBase(host); // also handles a bare host with no scheme.
    if (h.empty()) {
        return false;
    }
    // Bracketed IPv6 — strip brackets and an optional :port outside them.
    if (h.front() == '[') {
        const std::size_t close = h.find(']');
        if (close != std::string::npos) {
            h = h.substr(1, close - 1);
        }
    } else {
        // Strip a trailing port suffix (IPv4 or hostname). A bare IPv6 loopback has colons
        // but no port, so treat the last colon as a port separator only when there is one.
        const std::size_t firstColon = h.find(':');
        const std::size_t lastColon = h.rfind(':');
        if (firstColon != std::string::npos && firstColon == lastColon) {
            h = h.substr(0, firstColon);
        }
    }
    if (h == "localhost" || h == "::1") {
        return true;
    }
    // 127.0.0.0/8 — require a dotted-quad of digits so a hostname like "127.example.com"
    // (which merely starts with "127.") is NOT mistaken for the loopback block.
    if (h.compare(0, 4, "127.") == 0) {
        int dots = 0;
        for (char c : h) {
            if (c == '.') {
                ++dots;
            } else if (c < '0' || c > '9') {
                return false; // a non-digit, non-dot char => it's a hostname, not 127.x.x.x.
            }
        }
        return dots == 3;
    }
    return false;
}

bool ShouldUpgradeCleartextBase(const std::string& rawBase) {
    const std::string s = Trimmed(rawBase);
    // Case-insensitive "http://" prefix check; https:// and anything else => no upgrade.
    if (ToLower(s).compare(0, 7, "http://") != 0) {
        return false;
    }
    return !IsLoopbackHost(s);
}

} // namespace TrackerHttpPure

// Global free function (declared in TrackerHttpPure.h). Pure string classification, no cpr —
// kept out of TrackerHttpUtils.cpp so cpr-free consumers (Sync, the TSan threading subset) link
// it without dragging in cpr. Behaviour preserved byte-for-byte from the original location.
bool IsTrackerTransportErrorText(const std::string& error) {
    if (error.empty()) {
        return false;
    }
    const std::string s = ToLowerAsciiCopy(error);

    // Client/config/auth/validation — never treat as transport.
    static const char* kHard[] = {
        "missing tracker domain",
        "missing tracker",
        "api token",
        "tracker backend is not initialized",
        "http 400",
        "http 401",
        "http 402",
        "http 403",
        "http 404",
        "http 405",
        "http 406",
        "http 409",
        "http 410",
        "http 422",
        "invalid credentials",
        "bad request",
        "unprocessable",
        // Plane config errors
        "plane is not configured",
        "plane api key is missing",
    };
    for (const char* h : kHard) {
        if (s.find(h) != std::string::npos) {
            return false;
        }
    }

    static const char* kTransport[] = {
        "http 0",
        "http 500",
        "http 502",
        "http 503",
        "http 504",
        "timeout",
        "timed out",
        "operation timed out",
        "could not resolve host",
        "couldn't resolve host",
        "name or service not known",
        "failed to connect",
        "connection refused",
        "connection reset",
        "connection aborted",
        "network is unreachable",
        "host unreachable",
        "ssl connect error",
        "couldn't connect to server",
        "eof occurred",
        "offline",
        "network error",
        "resolve host",
        "resolve proxy",
        "connection closed",
        "stream error",
        "certificate verify failed",
        "ssl peer certificate",
        "schannel",
        // Broad connectivity hints (aligned with legacy offline-create detection).
        "network",
        "connection",
    };
    for (const char* t : kTransport) {
        if (s.find(t) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string RedactHttpBodyForLog(const std::string& body) {
    // Strip reflected tokens (Bearer / api_key / Authorization / sk- / ghp_ …) via the
    // shared cpr-free redactor, which also caps length to kMaxProviderErrorBodyChars.
    return smatchet::ai::pure::RedactProviderErrorBody(body);
}
