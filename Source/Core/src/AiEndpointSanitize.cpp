#include "AiEndpointSanitize.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace smatchet {
namespace ai {
namespace pure {

namespace {

// Lower-cases ASCII only — host names are ASCII in practice; non-ASCII would
// require punycode and we do not handle IDNs in the validator.
std::string LowerAscii(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

bool ContainsControlChars(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

bool ExtractScheme(const std::string& url, std::size_t& schemeEnd) {
    const std::size_t colon = url.find(':');
    if (colon == std::string::npos)
        return false;
    // require "://"
    if (colon + 2 >= url.size() || url[colon + 1] != '/' || url[colon + 2] != '/')
        return false;
    schemeEnd = colon;
    return true;
}

std::string ExtractHostPort(const std::string& url, std::size_t schemeEnd) {
    const std::size_t hostStart = schemeEnd + 3; // past "://"
    if (hostStart >= url.size())
        return std::string();
    // host ends at the first '/' or '?' or '#'.
    const auto hostEndIt = std::find_if(url.begin() + static_cast<std::ptrdiff_t>(hostStart), url.end(),
                                        [](char c) { return c == '/' || c == '?' || c == '#'; });
    const std::size_t hostEnd = static_cast<std::size_t>(hostEndIt - url.begin());
    std::string authority = url.substr(hostStart, hostEnd - hostStart);
    // Strip userinfo ("user:pass@") — the real host is everything after the LAST
    // '@'. Without this, "https://api.openai.com:x@evil.com" would validate the
    // fake userinfo host "api.openai.com" (passing the provider host-pin) while
    // curl actually connects to evil.com, and "https://a:b@169.254.169.254/"
    // would bypass the metadata denylist by validating host "a".
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority.erase(0, at + 1);
    return authority;
}

// Strip optional ":port" suffix from a "host[:port]" string. A bracketed IPv6
// literal "[::1]:443" keeps everything inside the brackets (its own colons are
// not a port separator) and yields the bracket-stripped address "::1"; a plain
// "host:port" splits at the single colon. An IPv4/host with no port is returned
// as-is. (Multiple unbracketed colons => treated as a bare IPv6 literal, kept
// whole; the canonicaliser decides whether it is a real address.)
std::string StripPort(const std::string& hostPort) {
    if (!hostPort.empty() && hostPort[0] == '[') {
        const std::size_t close = hostPort.find(']');
        if (close == std::string::npos)
            return hostPort; // malformed; canonicaliser will reject
        return hostPort.substr(1, close - 1);
    }
    const std::size_t first = hostPort.find(':');
    if (first == std::string::npos)
        return hostPort;
    // A second colon means an unbracketed IPv6 literal — keep the whole thing.
    if (hostPort.find(':', first + 1) != std::string::npos)
        return hostPort;
    return hostPort.substr(0, first);
}

// Parse a single numeric IPv4 "part" supporting the three C-library radixes that
// inet_aton / Windows resolvers accept: a leading "0x"/"0X" = hex, a leading "0"
// = octal, otherwise decimal. Writes the value into `out` (0 .. cap). Returns
// false on an empty part, a non-radix digit, or any value > cap — the > cap test
// is the overflow guard, so a denied address can never wrap into an allowed one.
// `cap` is 0xFF for the first three dotted parts and the remainder cap for the
// last (so "169.254.43518" packs 43518 into the final two octets, like inet_aton).
bool ParseIpv4Part(const std::string& s, std::size_t begin, std::size_t end, std::uint64_t cap, std::uint64_t& out) {
    if (begin >= end)
        return false;
    int radix = 10;
    std::size_t i = begin;
    if (s[i] == '0' && i + 1 < end && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        radix = 16;
        i += 2;
        if (i >= end)
            return false; // bare "0x"
    } else if (s[i] == '0' && i + 1 < end) {
        radix = 8;
        ++i;
    }
    std::uint64_t val = 0;
    for (; i < end; ++i) {
        const char c = s[i];
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (radix == 16 && c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (radix == 16 && c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return false;
        if (d >= radix)
            return false; // e.g. '8'/'9' in an octal part
        val = val * static_cast<std::uint64_t>(radix) + static_cast<std::uint64_t>(d);
        if (val > cap)
            return false; // overflow / out-of-range guard
    }
    out = val;
    return true;
}

// Canonicalise any IPv4 literal — classic dotted-quad AND the alternate radix /
// short forms that resolvers (and curl) accept and an SSRF denylist must not
// miss: decimal "2852039166", hex "0xA9FEA9FE", octal "0251.0376.0251.0376",
// and short forms with 1-3 parts (the last part packs the trailing octets, like
// inet_aton: "169.254.43518" -> 169.254.169.254). Returns false if the host is
// not an IPv4 literal in any of these forms. Overflow-safe via ParseIpv4Part.
bool CanonicalizeIpv4(const std::string& host, unsigned char octets[4]) {
    // Hex/octal digits push beyond the dotted-quad alphabet, so screen on the
    // characters a numeric IPv4 literal can contain (digits, dots, x/X, a-f/A-F).
    const bool numericLike = !host.empty() && std::all_of(host.begin(), host.end(), [](char c) {
        return c == '.' || (c >= '0' && c <= '9') || c == 'x' || c == 'X' || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    });
    if (!numericLike)
        return false;

    // Split on '.' into up to 4 parts (reject 5+ or a trailing/empty part).
    std::uint64_t parts[4];
    int nparts = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == '.') {
            if (nparts >= 4)
                return false;                        // too many parts
            const std::uint64_t cap = 0xFFFFFFFFull; // per-part overflow cap; range checked below
            if (!ParseIpv4Part(host, start, i, cap, parts[nparts]))
                return false;
            ++nparts;
            start = i + 1;
        }
    }
    if (nparts == 0)
        return false;

    // inet_aton packing: the LAST part fills the remaining low octets; every
    // leading part must fit in a single octet.
    for (int p = 0; p < nparts - 1; ++p) {
        if (parts[p] > 0xFFull)
            return false;
    }
    const int trailingOctets = 4 - (nparts - 1);
    const std::uint64_t trailingCap = (trailingOctets >= 4) ? 0xFFFFFFFFull : ((1ull << (8 * trailingOctets)) - 1ull);
    if (parts[nparts - 1] > trailingCap)
        return false;

    std::uint32_t addr = 0;
    for (int p = 0; p < nparts - 1; ++p)
        addr |= static_cast<std::uint32_t>(parts[p]) << (8 * (3 - p));
    addr |= static_cast<std::uint32_t>(parts[nparts - 1]);

    octets[0] = static_cast<unsigned char>((addr >> 24) & 0xFF);
    octets[1] = static_cast<unsigned char>((addr >> 16) & 0xFF);
    octets[2] = static_cast<unsigned char>((addr >> 8) & 0xFF);
    octets[3] = static_cast<unsigned char>(addr & 0xFF);
    return true;
}

bool IsCloudMetadataLiteral(const unsigned char o[4]) {
    // AWS / GCP / Azure IMDS — 169.254.169.254 is the canonical metadata IP.
    // Alibaba uses 100.100.100.200; cover that too.
    if (o[0] == 169 && o[1] == 254 && o[2] == 169 && o[3] == 254)
        return true;
    if (o[0] == 100 && o[1] == 100 && o[2] == 100 && o[3] == 200)
        return true;
    return false;
}

bool IsLinkLocalLiteral(const unsigned char o[4]) {
    // 169.254.0.0/16 except for the IMDS literal handled separately above.
    return o[0] == 169 && o[1] == 254;
}

// RFC1918 private ranges: 10/8, 172.16/12, 192.168/16. (Loopback 127/8 is handled
// separately as an allowed-with-consent target, not a denied range.)
bool IsPrivateNetworkLiteral(const unsigned char o[4]) {
    if (o[0] == 10)
        return true;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31)
        return true;
    if (o[0] == 192 && o[1] == 168)
        return true;
    return false;
}

// True when `host`'s first hextet falls in [0xfe80, 0xfebf] — the fe80::/10 link-local
// range's fixed 10-bit prefix (1111111010). A plain string-prefix match ("fe80:") misses
// fe90::/fea0::/febf:: etc., which are the same /10 block. CPP_CODE_AUDIT.md #16.
bool IsIpv6LinkLocalHextet(const std::string& host) {
    const std::size_t firstColon = host.find(':');
    if (firstColon == std::string::npos || firstColon == 0 || firstColon > 4) {
        return false;
    }
    const std::string firstHextet = host.substr(0, firstColon);
    const bool allHex = std::all_of(firstHextet.begin(), firstHextet.end(),
                                    [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    if (!allHex) {
        return false;
    }
    unsigned int hextetValue = 0;
    for (char c : firstHextet) {
        hextetValue = hextetValue * 16u + static_cast<unsigned int>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return hextetValue >= 0xfe80u && hextetValue <= 0xfebfu;
}

// Expand an IPv6 literal (with NO embedded dotted-v4 tail) into its 8 hextets,
// honouring a single "::" zero-run. Returns false on malformed input, more than
// one "::", or a dotted tail (that form is handled by the caller's v4 extractor).
bool ExpandIpv6Hextets(const std::string& host, std::uint16_t out[8]) {
    if (host.find('.') != std::string::npos)
        return false;
    const auto parseGroups = [](const std::string& s, std::uint16_t* dst, int cap, int& n) -> bool {
        n = 0;
        if (s.empty())
            return true;
        std::size_t start = 0;
        while (true) {
            const std::size_t colon = s.find(':', start);
            const std::string g = s.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
            if (g.empty() || g.size() > 4 || n >= cap)
                return false;
            std::uint16_t v = 0;
            for (char c : g) {
                int d;
                if (c >= '0' && c <= '9')
                    d = c - '0';
                else if (c >= 'a' && c <= 'f')
                    d = c - 'a' + 10;
                else
                    return false;
                v = static_cast<std::uint16_t>(v * 16 + d);
            }
            dst[n++] = v;
            if (colon == std::string::npos)
                break;
            start = colon + 1;
        }
        return true;
    };
    const std::size_t dc = host.find("::");
    std::uint16_t head[8], tail[8];
    int nh = 0, nt = 0;
    if (dc == std::string::npos) {
        if (!parseGroups(host, head, 8, nh) || nh != 8)
            return false;
        for (int i = 0; i < 8; ++i)
            out[i] = head[i];
        return true;
    }
    if (host.find("::", dc + 1) != std::string::npos)
        return false; // more than one "::"
    if (!parseGroups(host.substr(0, dc), head, 8, nh))
        return false;
    if (!parseGroups(host.substr(dc + 2), tail, 8, nt))
        return false;
    if (nh + nt > 8)
        return false;
    for (int i = 0; i < 8; ++i)
        out[i] = 0;
    for (int i = 0; i < nh; ++i)
        out[i] = head[i];
    for (int i = 0; i < nt; ++i)
        out[8 - nt + i] = tail[i];
    return true;
}

// Decode the pure-hextet IPv4-mapped (::ffff:HHHH:HHHH) / IPv4-compatible
// (::HHHH:HHHH) forms — which have NO dotted tail — and run the IPv4 denylist.
// Sets `out` and returns true ONLY for a DENIED address, so the caller's pure-IPv6
// handling stays untouched for loopback / public / non-mapped literals.
bool TryClassifyMappedIpv6Hextets(const std::string& host, EndpointVerdict& out) {
    std::uint16_t h[8];
    if (!ExpandIpv6Hextets(host, h))
        return false;
    if (!(h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0 && h[4] == 0 && (h[5] == 0 || h[5] == 0xffff)))
        return false;
    const unsigned char o[4] = {
        static_cast<unsigned char>((h[6] >> 8) & 0xFF), static_cast<unsigned char>(h[6] & 0xFF),
        static_cast<unsigned char>((h[7] >> 8) & 0xFF), static_cast<unsigned char>(h[7] & 0xFF)};
    if (IsCloudMetadataLiteral(o)) {
        out = EndpointVerdict::RejectedCloudMetadata;
        return true;
    }
    if (IsLinkLocalLiteral(o)) {
        out = EndpointVerdict::RejectedLinkLocal;
        return true;
    }
    if (IsPrivateNetworkLiteral(o)) {
        out = EndpointVerdict::RejectedPrivateNetwork;
        return true;
    }
    return false;
}

// Classify a bare (bracket-stripped, lowercased) IPv6 literal against the same
// denylist, mapping each match onto the existing IPv4 verdicts. Returns true and
// sets `out` when the host is an IPv6 literal we recognise (incl. IPv4-mapped /
// IPv4-compatible forms, which delegate to the IPv4 classifier). Returns false
// when the host is not an IPv6 literal at all (a normal hostname).
bool ClassifyIpv6Literal(const std::string& host, EndpointVerdict& out, bool& isLiteral) {
    isLiteral = false;
    // An IPv6 literal has >=2 colons (so a "host:port" leftover never reaches
    // here) and only hex digits, colons, dots (for the embedded-v4 tail).
    if (host.find("::") == std::string::npos && std::count(host.begin(), host.end(), ':') < 2)
        return false;
    const bool ipv6Like = !host.empty() && std::all_of(host.begin(), host.end(), [](char c) {
        return c == ':' || c == '.' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    if (!ipv6Like)
        return false;
    isLiteral = true;

    // IPv4-mapped (::ffff:a.b.c.d) and IPv4-compatible (::a.b.c.d) embed a dotted
    // IPv4 tail — pull it out and run the full IPv4 denylist on the real address.
    const std::size_t lastColon = host.rfind(':');
    if (lastColon != std::string::npos && host.find('.', lastColon) != std::string::npos) {
        const std::string v4 = host.substr(lastColon + 1);
        unsigned char o[4];
        if (CanonicalizeIpv4(v4, o)) {
            if (IsCloudMetadataLiteral(o))
                out = EndpointVerdict::RejectedCloudMetadata;
            else if (IsLinkLocalLiteral(o))
                out = EndpointVerdict::RejectedLinkLocal;
            else if (IsPrivateNetworkLiteral(o))
                out = EndpointVerdict::RejectedPrivateNetwork;
            else
                out = EndpointVerdict::Allowed; // a mapped public v4 — let host-pin decide
            return true;
        }
    }

    // Pure-hextet IPv4-mapped and IPv4-compatible forms embed the v4 address in the
    // last two hextets with no dotted tail, so the dotted extractor above misses the
    // compressed mapped form of a metadata or private address. Only a denied mapped
    // address short-circuits here, so a mapped public address still falls through to
    // the host-pin like the dotted form does.
    if (TryClassifyMappedIpv6Hextets(host, out))
        return true;

    // Pure-IPv6 prefixes: loopback ::1, link-local fe80::/10, ULA fc00::/7.
    if (host == "::1") {
        out = EndpointVerdict::Allowed; // loopback — consent gating handled by caller
        return true;
    }
    if (IsIpv6LinkLocalHextet(host)) {
        out = EndpointVerdict::RejectedLinkLocal;
        return true;
    }
    if (!host.empty() && (host[0] == 'f') && host.size() >= 2 && (host[1] == 'c' || host[1] == 'd')) {
        out = EndpointVerdict::RejectedPrivateNetwork; // fc00::/7 ULA (fc.. / fd..)
        return true;
    }
    out = EndpointVerdict::Allowed; // a public/other IPv6 literal — host-pin decides
    return true;
}

// Loopback = always-safe target for plain http:// (the local-Ollama case).
// Covers "localhost" by name, the whole 127.0.0.0/8 IPv4 block (any encoding),
// and the IPv6 loopback ::1.
bool IsLoopbackHost(const std::string& host) {
    if (host == "localhost" || host == "::1")
        return true;
    unsigned char octets[4];
    return CanonicalizeIpv4(host, octets) && octets[0] == 127;
}

} // namespace

EndpointVerdict SanitizeAiEndpointUrl(const std::string& url, const EndpointPolicy& policy, std::string& out_url) {
    out_url.clear();
    if (url.empty())
        return EndpointVerdict::Allowed; // empty = "use provider default" — the client TU handles that
    if (ContainsControlChars(url))
        return EndpointVerdict::RejectedControlChars;

    std::size_t schemeEnd = 0;
    if (!ExtractScheme(url, schemeEnd))
        return EndpointVerdict::RejectedBadScheme;
    const std::string scheme = LowerAscii(url.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https")
        return EndpointVerdict::RejectedBadScheme;

    const std::string hostPort = ExtractHostPort(url, schemeEnd);
    if (hostPort.empty())
        return EndpointVerdict::RejectedMalformed;
    const std::string host = LowerAscii(StripPort(hostPort));
    if (host.empty())
        return EndpointVerdict::RejectedMalformed;

    // SSRF denylist on the CANONICALISED address — alternate IPv4 encodings
    // (decimal / octal / hex / short-form) and IPv6 literals (incl. IPv4-mapped
    // ::ffff:) all resolve to their real address BEFORE the comparison, so a
    // config-write attacker cannot smuggle a denied IP past the dotted-quad check.
    unsigned char octets[4];
    if (CanonicalizeIpv4(host, octets)) {
        if (IsCloudMetadataLiteral(octets))
            return EndpointVerdict::RejectedCloudMetadata;
        if (IsLinkLocalLiteral(octets))
            return EndpointVerdict::RejectedLinkLocal;
        if (IsPrivateNetworkLiteral(octets))
            return EndpointVerdict::RejectedPrivateNetwork;
    } else {
        EndpointVerdict v6 = EndpointVerdict::Allowed;
        bool isV6Literal = false;
        if (ClassifyIpv6Literal(host, v6, isV6Literal) && isV6Literal && v6 != EndpointVerdict::Allowed)
            return v6;
    }

    // Loopback is auto-exempt ONLY for unpinned providers (Ollama / DeepSeek):
    // a pinned cloud provider repointed at http://127.0.0.1 is still a
    // config-write redirect of a key-bearing request to an arbitrary local
    // listener, so it must clear the same consent gates. Pinning makes the
    // CanonicalHost non-empty.
    const bool loopback = IsLoopbackHost(host);
    const bool allowLoopbackWithoutConsent = loopback && policy.CanonicalHost.empty();
    // Cleartext API key on the wire: plain http:// needs explicit consent unless
    // it is loopback on an unpinned/local provider.
    if (scheme == "http" && !allowLoopbackWithoutConsent && !policy.AllowInsecureHttp)
        return EndpointVerdict::RejectedInsecureHttp;

    // Per-provider host pin: a config-write attacker must not silently repoint a
    // cloud provider's key-bearing request at an arbitrary host — including a
    // local one. Empty CanonicalHost = unpinned provider (Ollama / DeepSeek).
    if (!policy.CanonicalHost.empty() && !policy.AllowCustomHost && host != policy.CanonicalHost)
        return EndpointVerdict::RejectedNonProviderHost;

    out_url = url;
    return EndpointVerdict::Allowed;
}

EndpointVerdict SanitizeAiEndpointUrl(const std::string& url, std::string& out_url) {
    return SanitizeAiEndpointUrl(url, EndpointPolicy(), out_url);
}

std::string ExtractUrlHost(const std::string& url) {
    std::size_t schemeEnd = 0;
    if (!ExtractScheme(url, schemeEnd))
        return std::string();
    const std::string hostPort = ExtractHostPort(url, schemeEnd);
    if (hostPort.empty())
        return std::string();
    return LowerAscii(StripPort(hostPort));
}

const char* EndpointVerdictDescription(EndpointVerdict v) {
    switch (v) {
    case EndpointVerdict::Allowed:
        return "allowed";
    case EndpointVerdict::RejectedBadScheme:
        return "rejected: scheme must be http:// or https://";
    case EndpointVerdict::RejectedControlChars:
        return "rejected: control characters (CR/LF/NUL) embedded in URL";
    case EndpointVerdict::RejectedCloudMetadata:
        return "rejected: cloud-metadata IP literal (169.254.169.254 / 100.100.100.200) blocked to prevent SSRF";
    case EndpointVerdict::RejectedLinkLocal:
        return "rejected: link-local address (169.254.0.0/16 or fe80::/10) blocked to prevent SSRF";
    case EndpointVerdict::RejectedPrivateNetwork:
        return "rejected: private-network address (RFC1918 10/8 · 172.16/12 · 192.168/16, or IPv6 ULA "
               "fc00::/7) blocked to prevent SSRF";
    case EndpointVerdict::RejectedMalformed:
        return "rejected: malformed URL (missing host)";
    case EndpointVerdict::RejectedNonProviderHost:
        return "rejected: non-provider host — enable 'allow custom endpoint' in Preferences to use a proxy/gateway";
    case EndpointVerdict::RejectedInsecureHttp:
        return "rejected: plain http:// to a non-loopback host would send the API key in cleartext";
    }
    return "rejected: unknown";
}

} // namespace pure
} // namespace ai
} // namespace smatchet
