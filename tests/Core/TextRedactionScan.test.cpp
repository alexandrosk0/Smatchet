// RedactLogLine's hand-written scans (#2304) checked against the std::regex pipeline they replaced,
// plus long-token inputs that made that pipeline quadratic or throw.
//
// The oracle below is the previous implementation verbatim. On a standard library whose matcher has
// a complexity budget (MSVC STL 14.51+) the oracle itself can throw on a long sample; such a sample
// is skipped, and the generated lines are kept short so that stays rare.

#include "TextRedaction.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <regex>
#include <string>
#include <vector>

using smatchet::privacy::RedactLogLine;

namespace {

namespace oracle {

const char* const kRedacted = "<redacted>";

const std::regex& AuthHeaderRe() {
    static const std::regex re(R"((Authorization\s*:\s*)(Bearer|Basic)\s+[A-Za-z0-9._~+/=\-]+)",
                               std::regex::icase | std::regex::optimize);
    return re;
}
const std::regex& SecretParamRe() {
    static const std::regex re(
        R"(((?:api[_-]?key|access[_-]?token|client[_-]?secret|token|password|passwd|pwd|secret)\s*=\s*)([^&\s"']+))",
        std::regex::icase | std::regex::optimize);
    return re;
}
const std::regex& UserInfoRe() {
    static const std::regex re(R"(([a-zA-Z][a-zA-Z0-9+.\-]*://[^:/?#\s]+:)([^@/?#\s]+)(@))", std::regex::optimize);
    return re;
}
const std::regex& EmailRe() {
    static const std::regex re(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})", std::regex::optimize);
    return re;
}
const std::regex& LongTokenRe() {
    static const std::regex re(R"([A-Za-z0-9+/=_\-]{40,})", std::regex::optimize);
    return re;
}
const std::regex& Sha1Re() {
    static const std::regex re(R"(^[0-9a-f]{40}$)", std::regex::optimize);
    return re;
}
const std::regex& UuidRe() {
    static const std::regex re(R"([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})",
                               std::regex::optimize);
    return re;
}
const std::regex& ControlAndAnsiRe() {
    static const std::regex re(R"(\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)?|[\x00-\x1f\x7f])",
                               std::regex::optimize);
    return re;
}

std::string RedactLongTokens(const std::string& in) {
    std::string out;
    auto it = std::sregex_iterator(in.begin(), in.end(), LongTokenRe());
    const auto end = std::sregex_iterator();
    std::size_t last = 0;
    for (; it != end; ++it) {
        const std::smatch& m = *it;
        const std::size_t pos = static_cast<std::size_t>(m.position(0));
        const std::string tok = m.str(0);
        out.append(in, last, pos - last);
        out.append((tok.size() == 40 && std::regex_match(tok, Sha1Re())) ? tok : std::string(kRedacted));
        last = pos + tok.size();
    }
    out.append(in, last, in.size() - last);
    return out;
}

std::string RedactLogLine(const std::string& line) {
    std::string s = std::regex_replace(line, ControlAndAnsiRe(), " ");
    s = std::regex_replace(s, AuthHeaderRe(), std::string("$1") + kRedacted);
    s = std::regex_replace(s, SecretParamRe(), std::string("$1") + kRedacted);
    s = std::regex_replace(s, UserInfoRe(), std::string("$1") + kRedacted + "$3");
    s = std::regex_replace(s, EmailRe(), "<redacted-email>");
    s = std::regex_replace(s, UuidRe(), kRedacted);
    return RedactLongTokens(s);
}

} // namespace oracle

// Pieces that steer generated lines into every branch of every pass: the shape keywords in mixed
// case, their near misses, the separators each run stops at, and the escape-sequence bytes.
const char* const kFragments[] = {
    "Authorization",
    "AUTHORIZATION:",
    "authorization :",
    "Bearer ",
    "bearer",
    "BASIC ",
    "Basicx",
    ": ",
    "api_key",
    "API-KEY",
    "apikey",
    "access_token",
    "Access-Token",
    "client_secret",
    "clientsecret",
    "token",
    "TOKEN",
    "password",
    "Passwd",
    "pwd",
    "secret",
    "secretkey",
    "=",
    " = ",
    "&",
    "\"",
    "'",
    "https://",
    "x+y.z-1://",
    "1http://",
    "://",
    "user",
    ":",
    "pa:ss",
    "@",
    "@@",
    "host",
    "/",
    "?",
    "#",
    "bob",
    "a.b",
    "example",
    ".com",
    ".c",
    ".co1",
    ".io.",
    "-",
    "_",
    "%",
    "+",
    ".",
    "0123abcd",
    "-1234-",
    "5678",
    "ABCDEF",
    "9abc-def0-1234-5678-9abcdef01234",
    "deadbeef",
    "0123456789abcdef0123456789abcdef01234567",
    "0123456789ABCDEF0123456789ABCDEF01234567",
    "aaaaaaaaaaaaaaaaaaaa",
    "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=",
    "~",
    " ",
    "  ",
    "\t",
    "\x1b",
    "\x1b[",
    "\x1b[31m",
    "\x1b[1;2?;3 !@",
    "\x1b]0;title\x07",
    "\x1b]8;;",
    "\x1b\\",
    "\x07",
    "\r",
    "\n",
    "\x7f",
    "\x01",
    "\xc3\xa9",
    "\xff",
    "[",
    "]",
    ";",
    "?",
    "x",
    "Z",
    "9",
};

std::string GenerateLine(std::mt19937& rng) {
    const std::size_t fragmentCount = sizeof(kFragments) / sizeof(kFragments[0]);
    const std::uint32_t pieces = 1 + rng() % 18;
    std::string line;
    for (std::uint32_t i = 0; i < pieces; ++i) {
        line += kFragments[rng() % fragmentCount];
    }
    return line;
}

// Returns false (sample skipped) when the oracle's matcher refuses the input.
bool OracleRedact(const std::string& line, std::string& out) {
    try {
        out = oracle::RedactLogLine(line);
        return true;
    } catch (const std::regex_error&) {
        return false;
    }
}

} // namespace

TEST_CASE("RedactLogLine scans agree with the regex pipeline they replaced (differential)" *
          doctest::test_suite("[high-risk]")) {
    std::mt19937 rng(0x2304u);
    int compared = 0;
    int skipped = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::string line = GenerateLine(rng);
        std::string expected;
        if (!OracleRedact(line, expected)) {
            ++skipped;
            continue;
        }
        ++compared;
        const std::string actual = RedactLogLine(line);
        if (actual != expected) {
            // One readable failure is enough; the line is shown escaped by doctest.
            CAPTURE(line);
            CHECK(actual == expected);
            break;
        }
    }
    CHECK(compared > 19000);
    CHECK(skipped < 1000);
}

TEST_CASE("RedactLogLine pins the leftmost-match choices the scans reproduce") {
    // CSI and OSC sequences collapse to one space each; an unterminated CSI leaves its '['.
    CHECK(RedactLogLine("a\x1b[1;31mb") == "a b");
    CHECK(RedactLogLine("a\x1b]0;title\x07"
                        "b") == "a b");
    CHECK(RedactLogLine("a\x1b]0;t\x1b\\b") == "a b");
    CHECK(RedactLogLine("a\x1b[12") == "a [12");
    // The header name, scheme and spacing are case- and space-tolerant; the scheme is replaced.
    CHECK(RedactLogLine("authorization :  BEARER abc.def") == "authorization :  <redacted>");
    CHECK(RedactLogLine("Authorization: Bearerx abc") == "Authorization: Bearerx abc");
    // A parameter name may end a longer word; the value runs to '&', a quote or whitespace.
    CHECK(RedactLogLine("xtoken=abc&y=1") == "xtoken=<redacted>&y=1");
    CHECK(RedactLogLine("api-key = v1 next") == "api-key = <redacted> next");
    CHECK(RedactLogLine("secretkey=v") == "secretkey=v");
    // Userinfo: the scheme is the run before "://" from its first letter; the password may hold ':'.
    CHECK(RedactLogLine("1http://u:p:w@h") == "1http://u:<redacted>@h");
    CHECK(RedactLogLine("x://a b://c:d@e") == "x://a b://c:<redacted>@e");
    // Email: the domain match ends after the letters that follow its last usable dot.
    CHECK(RedactLogLine("mail a@b.co1 now") == "mail <redacted-email>1 now");
    CHECK(RedactLogLine("a@b.com.x1") == "<redacted-email>.x1");
    // UUID inside a longer hex run is found at the position where the dash layout fits.
    CHECK(RedactLogLine("ff01234567-89ab-cdef-0123-456789abcdef") == "ff<redacted>");
    // 40 lowercase hex = git SHA-1, kept; the same in uppercase or one longer is redacted.
    const std::string sha = "0123456789abcdef0123456789abcdef01234567";
    CHECK(RedactLogLine(sha) == sha);
    CHECK(RedactLogLine("0123456789ABCDEF0123456789ABCDEF01234567") == "<redacted>");
    CHECK(RedactLogLine(sha + "8") == "<redacted>");
}

TEST_CASE("RedactLogLine stays linear and does not throw on long unbroken tokens (#2304)" *
          doctest::test_suite("[high-risk]")) {
    // The quadratic shapes: a long token with no '@' (email), no "://" (userinfo). 256 KiB each —
    // far past the ~576 characters at which the MSVC 14.51 matcher threw, and long enough that a
    // quadratic scan would not finish in any test budget.
    const std::size_t kLen = 256 * 1024;
    SUBCASE("hex run is a long token") {
        std::string hex;
        hex.reserve(kLen);
        for (std::size_t i = 0; i < kLen; ++i) {
            hex += "0123456789abcdef"[i % 16];
        }
        CHECK(RedactLogLine("dump " + hex + " end") == "dump <redacted> end");
    }
    SUBCASE("base64-like run is a long token") {
        std::string b64;
        b64.reserve(kLen);
        for (std::size_t i = 0; i < kLen; ++i) {
            b64 += "ABCxyz019+/="[i % 12];
        }
        CHECK(RedactLogLine(b64) == "<redacted>");
    }
    SUBCASE("dotted name has only short runs and passes through") {
        std::string dotted;
        dotted.reserve(kLen);
        while (dotted.size() < kLen) {
            dotted += "module.";
        }
        CHECK(RedactLogLine(dotted) == dotted);
    }
    SUBCASE("percent-encoded run passes through") {
        std::string pct;
        pct.reserve(kLen);
        while (pct.size() < kLen) {
            pct += "%2F%3A";
        }
        CHECK(RedactLogLine(pct) == pct);
    }
    SUBCASE("a long local part still becomes one redacted email") {
        std::string local(kLen, 'a');
        CHECK(RedactLogLine(local + "@example.com") == "<redacted-email>");
    }
    SUBCASE("a long scheme-shaped run before a userinfo URL") {
        std::string scheme(kLen, 'h');
        CHECK(RedactLogLine(scheme + "://u:pw@h") == "<redacted>://u:<redacted>@h");
    }
}
