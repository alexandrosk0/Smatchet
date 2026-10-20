#include "TextRedaction.h"

#include "AsciiCharClass.h"

#include <cstddef>
#include <exception>

// Each pass below is one forward scan that reproduces one shape pattern exactly. The pattern is
// quoted above each pass in ECMAScript regex notation (the form these passes replaced, kept so a
// reviewer can check the scan against it; tests/Core/TextRedactionScan.test.cpp runs the two side
// by side). std::regex is not used: its backtracking matchers rescan an unbroken token from every
// start position, so the email and URL-userinfo shapes cost time quadratic in the token length; the
// MSVC STL from toolset 14.51 on throws std::regex_error once a search exceeds its complexity budget
// (#2304); and libstdc++'s recursive matcher can exhaust the stack on a long run. Redaction runs on
// every LOG_* call, so it must not throw and must stay linear in the line length.

namespace smatchet {
namespace privacy {

namespace {

const std::size_t kNpos = std::string::npos;

// Ten characters: inside every supported standard library's short-string buffer, so returning it
// from the failure path below does not allocate.
const char kRedacted[] = "<redacted>";
const char kRedactedEmail[] = "<redacted-email>";

using ascii::IsAlnum;
using ascii::IsAlpha;
using ascii::IsDigit;
using ascii::IsHexDigit;
using ascii::IsLowerHexDigit;
// `\s` in the patterns. After the control strip only ' ' is left, but no pass depends on that.
using ascii::IsSpace;

// End of `word` (lowercase ASCII) when s[pos..] starts with it ignoring ASCII case, else kNpos.
std::size_t MatchWordNoCase(const std::string& s, std::size_t pos, const char* word) {
    for (; *word != '\0'; ++word, ++pos) {
        if (pos >= s.size() || ascii::ToLower(s[pos]) != *word) {
            return kNpos;
        }
    }
    return pos;
}

std::size_t SkipSpaces(const std::string& s, std::size_t pos) {
    while (pos < s.size() && IsSpace(s[pos])) {
        ++pos;
    }
    return pos;
}

// Builds the output of a pass that rewrites only the spans it matches. Nothing is copied until the
// first replacement, so a line with nothing of that shape costs no allocation.
class SpanRewriter {
  public:
    explicit SpanRewriter(const std::string& in) : in_(in) {}

    // Keep in_[copied, keepEnd), append `replacement`, and continue copying from `resumeAt`.
    void Replace(std::size_t keepEnd, const char* replacement, std::size_t resumeAt) {
        if (!active_) {
            out_.reserve(in_.size());
            active_ = true;
        }
        out_.append(in_, copied_, keepEnd - copied_);
        out_.append(replacement);
        copied_ = resumeAt;
    }

    // Store the rewritten text in `target` (the string the rewriter reads) if anything was replaced.
    void Finish(std::string& target) {
        if (!active_) {
            return;
        }
        out_.append(in_, copied_, kNpos);
        target.swap(out_);
    }

  private:
    const std::string& in_;
    std::string out_;
    std::size_t copied_ = 0;
    bool active_ = false;
};

// CSI `\x1b\[[0-9;?]*[ -/]*[@-~]` at `esc` (ESC '[' already seen): one past it, or esc + 1 when the
// final byte is missing (the ESC alone is then a control byte). Parameter, intermediate and final
// bytes are disjoint ranges, so the greedy runs never need to give characters back.
std::size_t CsiSequenceEnd(const std::string& s, std::size_t esc) {
    const std::size_t n = s.size();
    std::size_t i = esc + 2;
    while (i < n && (IsDigit(s[i]) || s[i] == ';' || s[i] == '?')) {
        ++i;
    }
    while (i < n && s[i] >= 0x20 && s[i] <= 0x2F) {
        ++i;
    }
    return (i < n && s[i] >= 0x40 && s[i] <= 0x7E) ? i + 1 : esc + 1;
}

// OSC `\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)?` at `esc` (ESC ']' already seen): always matches; an ESC
// that does not start the ST terminator ends the sequence and is stripped on its own.
std::size_t OscSequenceEnd(const std::string& s, std::size_t esc) {
    const std::size_t n = s.size();
    std::size_t i = esc + 2;
    while (i < n && s[i] != '\x07' && s[i] != '\x1b') {
        ++i;
    }
    if (i < n && s[i] == '\x07') {
        return i + 1;
    }
    return (i + 1 < n && s[i] == '\x1b' && s[i + 1] == '\\') ? i + 2 : i;
}

// One past the escape sequence that starts with the ESC at `esc`, or esc + 1 for a lone ESC.
std::size_t EscapeSequenceEnd(const std::string& s, std::size_t esc) {
    const char next = (esc + 1 < s.size()) ? s[esc + 1] : '\0';
    if (next == '[') {
        return CsiSequenceEnd(s, esc);
    }
    if (next == ']') {
        return OscSequenceEnd(s, esc);
    }
    return esc + 1;
}

// CSI/OSC ANSI sequences, then any remaining C0 control byte (CR, LF and ESC included) or DEL, each
// become one space. Server-controlled strings reaching a log line can carry these to forge a fake
// log entry (CR/LF) or drive the terminal (ANSI); a space rather than nothing keeps adjacent tokens
// from fusing, which could defeat the shape passes that follow. Compacts in place.
void StripControlAndAnsi(std::string& s) {
    const std::size_t n = s.size();
    std::size_t write = 0;
    std::size_t read = 0;
    while (read < n) {
        const unsigned char c = static_cast<unsigned char>(s[read]);
        if (c > 0x1F && c != 0x7F) {
            s[write++] = s[read++];
            continue;
        }
        read = (c == 0x1B) ? EscapeSequenceEnd(s, read) : read + 1;
        s[write++] = ' ';
    }
    s.resize(write);
}

bool IsAuthTokenChar(char c) {
    return IsAlnum(c) || c == '.' || c == '_' || c == '~' || c == '+' || c == '/' || c == '=' || c == '-';
}

// (Authorization\s*:\s*)(Bearer|Basic)\s+[A-Za-z0-9._~+/=\-]+  ->  $1<redacted>   (ASCII case-insensitive)
void RedactAuthHeaders(std::string& s) {
    const std::size_t n = s.size();
    SpanRewriter rewriter(s);
    std::size_t i = 0;
    while (i < n) {
        const std::size_t nameEnd = MatchWordNoCase(s, i, "authorization");
        const std::size_t colon = (nameEnd == kNpos) ? kNpos : SkipSpaces(s, nameEnd);
        if (colon == kNpos || colon >= n || s[colon] != ':') {
            ++i;
            continue;
        }
        const std::size_t schemeBegin = SkipSpaces(s, colon + 1);
        std::size_t schemeEnd = MatchWordNoCase(s, schemeBegin, "bearer");
        if (schemeEnd == kNpos) {
            schemeEnd = MatchWordNoCase(s, schemeBegin, "basic");
        }
        const std::size_t tokenBegin = (schemeEnd == kNpos) ? kNpos : SkipSpaces(s, schemeEnd);
        if (tokenBegin == kNpos || tokenBegin == schemeEnd) {
            ++i;
            continue;
        }
        std::size_t tokenEnd = tokenBegin;
        while (tokenEnd < n && IsAuthTokenChar(s[tokenEnd])) {
            ++tokenEnd;
        }
        if (tokenEnd == tokenBegin) {
            ++i;
            continue;
        }
        rewriter.Replace(schemeBegin, kRedacted, tokenEnd);
        i = tokenEnd;
    }
    rewriter.Finish(s);
}

// End of a secret parameter name starting at `pos`, or kNpos. At most one name can match at a given
// position (no name is a prefix of another at the same start), so the first hit is the only one.
std::size_t MatchSecretParamName(const std::string& s, std::size_t pos) {
    struct CompoundName {
        const char* head;
        const char* tail;
    };
    // api[_-]?key, access[_-]?token, client[_-]?secret
    static const CompoundName kCompound[] = {{"api", "key"}, {"access", "token"}, {"client", "secret"}};
    for (const CompoundName& name : kCompound) {
        const std::size_t headEnd = MatchWordNoCase(s, pos, name.head);
        if (headEnd == kNpos) {
            continue;
        }
        const bool separator = headEnd < s.size() && (s[headEnd] == '_' || s[headEnd] == '-');
        return MatchWordNoCase(s, separator ? headEnd + 1 : headEnd, name.tail);
    }
    static const char* const kPlain[] = {"token", "password", "passwd", "pwd", "secret"};
    for (const char* name : kPlain) {
        const std::size_t end = MatchWordNoCase(s, pos, name);
        if (end != kNpos) {
            return end;
        }
    }
    return kNpos;
}

bool IsParamValueChar(char c) { return c != '&' && c != '"' && c != '\'' && !IsSpace(c); }

// ((?:api[_-]?key|access[_-]?token|client[_-]?secret|token|password|passwd|pwd|secret)\s*=\s*)([^&\s"']+)
//   ->  $1<redacted>   (ASCII case-insensitive)
void RedactSecretParams(std::string& s) {
    const std::size_t n = s.size();
    SpanRewriter rewriter(s);
    std::size_t i = 0;
    while (i < n) {
        const std::size_t nameEnd = MatchSecretParamName(s, i);
        const std::size_t equals = (nameEnd == kNpos) ? kNpos : SkipSpaces(s, nameEnd);
        if (equals != kNpos && equals < n && s[equals] == '=') {
            const std::size_t valueBegin = SkipSpaces(s, equals + 1);
            std::size_t valueEnd = valueBegin;
            while (valueEnd < n && IsParamValueChar(s[valueEnd])) {
                ++valueEnd;
            }
            if (valueEnd > valueBegin) {
                rewriter.Replace(valueBegin, kRedacted, valueEnd);
                i = valueEnd;
                continue;
            }
        }
        ++i;
    }
    rewriter.Finish(s);
}

bool IsSchemeChar(char c) { return IsAlnum(c) || c == '+' || c == '.' || c == '-'; }
bool IsUserInfoUserChar(char c) { return c != ':' && c != '/' && c != '?' && c != '#' && !IsSpace(c); }
bool IsUserInfoPasswordChar(char c) { return c != '@' && c != '/' && c != '?' && c != '#' && !IsSpace(c); }

// ([a-zA-Z][a-zA-Z0-9+.\-]*://[^:/?#\s]+:)([^@/?#\s]+)(@)  ->  $1<redacted>$3
// Every match holds exactly one "://", directly after its scheme, so the scan visits each "://" and
// checks the scheme run before it and the user:password@ after it. None of the runs can contain the
// character that ends it, so each is decided without backtracking.
void RedactUrlPasswords(std::string& s) {
    const std::size_t n = s.size();
    SpanRewriter rewriter(s);
    std::size_t minStart = 0; // a match cannot start before the end of the previous one
    std::size_t from = 0;
    for (;;) {
        const std::size_t sep = s.find("://", from);
        if (sep == kNpos) {
            break;
        }
        from = sep + 1;
        std::size_t schemeBegin = sep;
        while (schemeBegin > minStart && IsSchemeChar(s[schemeBegin - 1])) {
            --schemeBegin;
        }
        bool schemeHasLetter = false;
        for (std::size_t k = schemeBegin; k < sep && !schemeHasLetter; ++k) {
            schemeHasLetter = IsAlpha(s[k]);
        }
        if (!schemeHasLetter) {
            continue;
        }
        std::size_t userEnd = sep + 3;
        while (userEnd < n && IsUserInfoUserChar(s[userEnd])) {
            ++userEnd;
        }
        if (userEnd == sep + 3 || userEnd >= n || s[userEnd] != ':') {
            continue;
        }
        const std::size_t passwordBegin = userEnd + 1;
        std::size_t passwordEnd = passwordBegin;
        while (passwordEnd < n && IsUserInfoPasswordChar(s[passwordEnd])) {
            ++passwordEnd;
        }
        if (passwordEnd == passwordBegin || passwordEnd >= n || s[passwordEnd] != '@') {
            continue;
        }
        rewriter.Replace(passwordBegin, kRedacted, passwordEnd); // the '@' is copied on
        minStart = passwordEnd + 1;
        from = minStart;
    }
    rewriter.Finish(s);
}

bool IsEmailLocalChar(char c) { return IsAlnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'; }
bool IsEmailDomainChar(char c) { return IsAlnum(c) || c == '.' || c == '-'; }

// End of `[A-Za-z0-9.\-]+\.[A-Za-z]{2,}` matched at `begin`, or kNpos. The greedy first run gives
// characters back until a '.' with at least two letters after it is found, so the match ends after
// the letter run that follows the LAST such '.' in the domain run.
std::size_t MatchEmailDomain(const std::string& s, std::size_t begin) {
    const std::size_t n = s.size();
    std::size_t runEnd = begin;
    while (runEnd < n && IsEmailDomainChar(s[runEnd])) {
        ++runEnd;
    }
    for (std::size_t dot = runEnd; dot > begin + 1;) {
        --dot;
        if (s[dot] != '.') {
            continue;
        }
        std::size_t lettersEnd = dot + 1;
        while (lettersEnd < n && IsAlpha(s[lettersEnd])) {
            ++lettersEnd;
        }
        if (lettersEnd - (dot + 1) >= 2) {
            return lettersEnd;
        }
    }
    return kNpos;
}

// [A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}  ->  <redacted-email>
// The local part cannot contain '@', so a match is anchored on its '@': the local part is the run of
// local characters right before it, from its start (or the previous match's end).
void RedactEmails(std::string& s) {
    SpanRewriter rewriter(s);
    std::size_t minStart = 0;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = s.find('@', from);
        if (at == kNpos) {
            break;
        }
        from = at + 1;
        std::size_t localBegin = at;
        while (localBegin > minStart && IsEmailLocalChar(s[localBegin - 1])) {
            --localBegin;
        }
        if (localBegin == at) {
            continue;
        }
        const std::size_t end = MatchEmailDomain(s, at + 1);
        if (end == kNpos) {
            continue;
        }
        rewriter.Replace(localBegin, kRedactedEmail, end);
        minStart = end;
        from = end;
    }
    rewriter.Finish(s);
}

bool IsUuidAt(const std::string& s, std::size_t pos) {
    static const std::size_t kGroupLengths[] = {8, 4, 4, 4, 12};
    if (s.size() - pos < 36) {
        return false;
    }
    std::size_t i = pos;
    for (std::size_t g = 0; g < 5; ++g) {
        if (g > 0) {
            if (s[i] != '-') {
                return false;
            }
            ++i;
        }
        for (std::size_t k = 0; k < kGroupLengths[g]; ++k, ++i) {
            if (!IsHexDigit(s[i])) {
                return false;
            }
        }
    }
    return true;
}

// [0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}  ->  <redacted>
// Plane API tokens and resource ids have this shape and are shorter than the long-token minStart; the
// dash layout keeps it from over-redacting arbitrary 36-character text.
void RedactUuids(std::string& s) {
    SpanRewriter rewriter(s);
    std::size_t i = 0;
    while (i + 36 <= s.size()) {
        if (s[i + 8] == '-' && IsUuidAt(s, i)) {
            rewriter.Replace(i, kRedacted, i + 36);
            i += 36;
        } else {
            ++i;
        }
    }
    rewriter.Finish(s);
}

bool IsLongTokenChar(char c) { return IsAlnum(c) || c == '+' || c == '/' || c == '=' || c == '_' || c == '-'; }

// [A-Za-z0-9+/=_\-]{40,}  ->  <redacted>, except a run of exactly 40 lowercase hex digits (a git
// SHA-1), which is kept so commit hashes survive in a report. A leftmost greedy match of this
// pattern is always a whole maximal run of token characters.
void RedactLongTokens(std::string& s) {
    const std::size_t n = s.size();
    SpanRewriter rewriter(s);
    std::size_t i = 0;
    while (i < n) {
        if (!IsLongTokenChar(s[i])) {
            ++i;
            continue;
        }
        std::size_t end = i;
        bool lowerHex = true;
        while (end < n && IsLongTokenChar(s[end])) {
            lowerHex = lowerHex && IsLowerHexDigit(s[end]);
            ++end;
        }
        const std::size_t length = end - i;
        if (length >= 40 && !(length == 40 && lowerHex)) {
            rewriter.Replace(i, kRedacted, end);
        }
        i = end;
    }
    rewriter.Finish(s);
}

} // namespace

std::string RedactLogLine(const std::string& line) {
    try {
        std::string s(line);
        // Strip CR/LF/ANSI first: a forged-newline or terminal-escape payload is neutralized before
        // secret-shape matching, and no later pass re-introduces a control byte, so the strip cannot
        // be evaded by hiding a control char mid-token (it is gone before the token is examined).
        StripControlAndAnsi(s);
        RedactAuthHeaders(s);
        RedactSecretParams(s);
        RedactUrlPasswords(s);
        RedactEmails(s);
        RedactUuids(s);
        RedactLongTokens(s);
        return s;
    } catch (const std::exception&) {
        // Out of memory part-way (the passes only allocate; no other operation here throws). This is
        // the logger's own ingestion path, so it cannot log. Default: withhold the whole line, since
        // returning it would pass on text a later pass never examined. kRedacted fits the short-string
        // buffer, so building the result does not allocate.
        return std::string(kRedacted);
    }
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
