#include "TextRedaction.h"

#include <regex>

namespace smatchet {
namespace privacy {

namespace {

const char* const kRedacted = "<redacted>";

// Compiled once (function-local statics — thread-safe init in C++11+). std::regex
// construction is expensive, so we never rebuild per call.

// Authorization: Bearer <tok> / Basic <tok>  (case-insensitive scheme + header).
const std::regex& AuthHeaderRe() {
    static const std::regex re(R"((Authorization\s*:\s*)(Bearer|Basic)\s+[A-Za-z0-9._~+/=\-]+)",
                               std::regex::icase | std::regex::optimize);
    return re;
}

// Secret-bearing URL/query params: token, api_key, apikey, access_token,
// password, passwd, pwd, secret, client_secret. Value runs until & or whitespace.
const std::regex& SecretParamRe() {
    static const std::regex re(
        R"(((?:api[_-]?key|access[_-]?token|client[_-]?secret|token|password|passwd|pwd|secret)\s*=\s*)([^&\s"']+))",
        std::regex::icase | std::regex::optimize);
    return re;
}

// URL userinfo: scheme://user:pass@host  -> keep user, redact pass.
const std::regex& UserInfoRe() {
    static const std::regex re(R"(([a-zA-Z][a-zA-Z0-9+.\-]*://[^:/?#\s]+:)([^@/?#\s]+)(@))", std::regex::optimize);
    return re;
}

// Emails.
const std::regex& EmailRe() {
    static const std::regex re(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})", std::regex::optimize);
    return re;
}

// Long opaque tokens — base64url/hex runs of >=40 chars. The SHA-1 guard below
// keeps 40-char lowercase-hex (git commit hashes) intact.
const std::regex& LongTokenRe() {
    static const std::regex re(R"([A-Za-z0-9+/=_\-]{40,})", std::regex::optimize);
    return re;
}

const std::regex& Sha1Re() {
    static const std::regex re(R"(^[0-9a-f]{40}$)", std::regex::optimize);
    return re;
}

// 36-char UUID/GUID shape (8-4-4-4-12 hex with dashes) — Plane API tokens and
// resource ids ride this shape and slip under the 40-char LongTokenRe floor.
// The dash structure keeps this from over-redacting arbitrary 36-char text.
const std::regex& UuidRe() {
    static const std::regex re(R"([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})",
                               std::regex::optimize);
    return re;
}

// CR, LF, lone ESC, and CSI/OSC ANSI escape sequences. Server-controlled strings
// reaching a log line can carry these to forge fake log entries (CR/LF) or drive
// terminal escapes (ANSI) — strip them so the sanitized line stays one physical
// line of inert text.
const std::regex& ControlAndAnsiRe() {
    // CSI: ESC [ params (0x30-0x3F) intermediates (0x20-0x2F) final (0x40-0x7E).
    // OSC: ESC ] ... terminated by BEL or ESC \. Plus any remaining ESC / C0
    // control (0x00-0x1F) / DEL (0x7F), CR and LF included.
    static const std::regex re(R"(\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)?|[\x00-\x1f\x7f])",
                               std::regex::optimize);
    return re;
}

// Replace stripped control/ANSI bytes with a single space so adjacent tokens do
// not silently fuse (which could defeat downstream secret-shape matching).
std::string StripControlAndAnsi(const std::string& in) { return std::regex_replace(in, ControlAndAnsiRe(), " "); }

// Redact long-token matches unless the match is exactly a 40-char git SHA-1.
std::string RedactLongTokens(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    auto begin = std::sregex_iterator(in.begin(), in.end(), LongTokenRe());
    const auto end = std::sregex_iterator();
    std::size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const std::smatch& m = *it;
        const std::size_t pos = static_cast<std::size_t>(m.position(0));
        const std::string tok = m.str(0);
        out.append(in, last, pos - last);
        if (tok.size() == 40 && std::regex_match(tok, Sha1Re())) {
            out.append(tok); // preserve git SHA-1
        } else {
            out.append(kRedacted);
        }
        last = pos + tok.size();
    }
    out.append(in, last, in.size() - last);
    return out;
}

} // namespace

std::string RedactLogLine(const std::string& line) {
    // Strip CR/LF/ANSI first: a forged-newline or terminal-escape payload is
    // neutralized before secret-shape matching, and nothing the secret passes
    // re-introduces a control byte, so the strip cannot be evaded by hiding a
    // control char mid-token (it is gone before the token is even examined).
    std::string s = StripControlAndAnsi(line);
    s = std::regex_replace(s, AuthHeaderRe(), std::string("$1") + kRedacted);
    s = std::regex_replace(s, SecretParamRe(), std::string("$1") + kRedacted);
    s = std::regex_replace(s, UserInfoRe(), std::string("$1") + kRedacted + "$3");
    s = std::regex_replace(s, EmailRe(), "<redacted-email>");
    s = std::regex_replace(s, UuidRe(), kRedacted);
    s = RedactLongTokens(s);
    return s;
}

std::string RedactLogText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        const std::size_t end = (nl == std::string::npos) ? text.size() : nl;
        out += RedactLogLine(text.substr(start, end - start));
        if (nl == std::string::npos) {
            break;
        }
        out += '\n';
        start = nl + 1;
    }
    return out;
}

} // namespace privacy
} // namespace smatchet
