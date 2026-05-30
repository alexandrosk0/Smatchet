#include "AiEndpointSanitize.h"

#include <algorithm>
#include <cctype>
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
    return url.substr(hostStart, hostEnd - hostStart);
}

// Strip optional ":port" suffix from a "host[:port]" string. IPv6 [..] is not
// considered here — IPv6 literal in an AI endpoint URL is exotic and the
// validator falls back to the conservative "RejectedMalformed" if it ever
// appears, since we never accept it as a host.
std::string StripPort(const std::string& hostPort) {
    const std::size_t colon = hostPort.find(':');
    if (colon == std::string::npos)
        return hostPort;
    return hostPort.substr(0, colon);
}

bool IsAllDigitsOrDots(const std::string& s) {
    if (s.empty())
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c == '.' || (c >= '0' && c <= '9'); });
}

// Parse "a.b.c.d" into 4 octets; returns false if the host is not a plain IPv4
// literal (we use this purely to detect link-local / metadata addresses — DNS
// resolution is intentionally NOT performed by the validator).
bool ParseIpv4Literal(const std::string& host, unsigned char octets[4]) {
    if (!IsAllDigitsOrDots(host))
        return false;
    std::size_t i = 0;
    for (int oct = 0; oct < 4; ++oct) {
        if (i >= host.size())
            return false;
        unsigned val = 0;
        std::size_t digits = 0;
        while (i < host.size() && host[i] != '.') {
            if (host[i] < '0' || host[i] > '9')
                return false;
            val = val * 10u + static_cast<unsigned>(host[i] - '0');
            ++digits;
            ++i;
            if (digits > 3 || val > 255u)
                return false;
        }
        if (digits == 0)
            return false;
        octets[oct] = static_cast<unsigned char>(val);
        if (oct < 3) {
            if (i >= host.size() || host[i] != '.')
                return false;
            ++i; // skip dot
        }
    }
    return i == host.size();
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

} // namespace

EndpointVerdict SanitizeAiEndpointUrl(const std::string& url, std::string& out_url) {
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

    unsigned char octets[4];
    if (ParseIpv4Literal(host, octets)) {
        if (IsCloudMetadataLiteral(octets))
            return EndpointVerdict::RejectedCloudMetadata;
        if (IsLinkLocalLiteral(octets))
            return EndpointVerdict::RejectedLinkLocal;
    }

    out_url = url;
    return EndpointVerdict::Allowed;
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
        return "rejected: link-local IPv4 (169.254.0.0/16) blocked to prevent SSRF";
    case EndpointVerdict::RejectedMalformed:
        return "rejected: malformed URL (missing host)";
    }
    return "rejected: unknown";
}

} // namespace pure
} // namespace ai
} // namespace smatchet
