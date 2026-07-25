#include "TrackerHttpPure.h"

#include "AiErrorRedact.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace TrackerHttpPure {

namespace {

// Trim surrounding whitespace. C++14, ASCII-only.
std::string Trimmed(const std::string& in) {
    std::size_t b = 0;
    // Kept duplicated rather than folded onto Core's TrimCopyAsciiWhitespace: that helper trims only
    // {space,\t,\n,\r} while this uses std::isspace (also \v,\f), so folding would NARROW the trim on a
    // host-allowlisting input (HostFromBase) — a behaviour change out of scope for a dead-code sweep.
    // This body is byte-identical to smatchet::linear::Trim, and ALREADY was: deleting this TU's ToLower
    // (now the shared ToLowerAsciiCopy) merely removed the divergence that had kept the maximal token run
    // under MIN_CLONE_TOKENS. The TrimCopyAsciiWhitespace clone family is deferred to a follow-up
    // burn-down driven by small_helper_audit.py's first real run (gate-blind-spot-sweep § Out of scope).
    //
    // The marker MUST stay the last line before `std::size_t e` and MUST stay on one line: the gate
    // matches a maximal token run that starts mid-body, and dup_audit._suppressed reads the nearest
    // non-blank line above that start. Wrapping it would push `rule=duplication` off that line.
    // SMATCHET_DEVIATION(rule=duplication; reason=see above; owner=build-doctor; revisit=2026-12-31)
    std::size_t e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) {
        --e;
    }
    return in.substr(b, e - b);
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
    return ToLowerAsciiCopy(s);
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
    if (ToLowerAsciiCopy(s).compare(0, 7, "http://") != 0) {
        return false;
    }
    return !IsLoopbackHost(s);
}

long ParseRetryAfterSeconds(const std::string& value) {
    const std::string s = Trimmed(value);
    // Delta-seconds form only: 1-6 digits (999999 s ≈ 11.5 days is already far past any
    // real throttle window; longer runs are garbage and would overflow smaller longs).
    if (s.empty() || s.size() > 6) {
        return -1;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return -1; // HTTP-date form or garbage — callers fall back to plain backoff
        }
    }
    return std::stol(s);
}

long ComputeTrackerRetryDelayMs(int attempt, long baseMs, long expCapMs, long retryAfterSec, long honorCapMs) {
    long delayMs = baseMs;
    for (int i = 1; i < attempt; ++i) {
        delayMs *= 2;
        if (delayMs >= expCapMs) {
            delayMs = expCapMs;
            break;
        }
    }
    if (retryAfterSec >= 0) {
        long honoredMs = (retryAfterSec > honorCapMs / 1000) ? honorCapMs : retryAfterSec * 1000;
        if (honoredMs > delayMs) {
            delayMs = honoredMs;
        }
    }
    return delayMs;
}

} // namespace TrackerHttpPure

// IsTrackerTransportErrorText was deleted here in N12 slice 3 — see TrackerHttpPure.h.

std::string RedactHttpBodyForLog(const std::string& body) {
    // Strip reflected tokens (Bearer / api_key / Authorization / sk- / ghp_ …) via the
    // shared cpr-free redactor, which also caps length to kMaxProviderErrorBodyChars.
    return smatchet::ai::pure::RedactProviderErrorBody(body);
}
