#include "AiChatMarkdownTokens.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <regex>

namespace AiChatMarkdownTokens {

namespace {

// Split the input markdown blob into lines on '\n'. The trailing '\r' (CRLF
// inputs) is stripped so the inline scan does not interpret '\r' as content.
// Empty input yields a single empty line so callers always see at least one
// line index.
std::vector<std::string> SplitLines(const std::string& md) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(64);
    for (std::size_t i = 0; i < md.size(); ++i) {
        const char c = md[i];
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') {
                cur.pop_back();
            }
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && cur.back() == '\r') {
        cur.pop_back();
    }
    out.push_back(cur);
    return out;
}

// "^\s*```(\w*)\s*$" — fence boundary detector. The CommonMark fence rule
// allows arbitrary attribute text after the language tag; we accept the
// permissive form so streaming partial fences are caught.
bool IsFenceLine(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i + 3 > line.size()) {
        return false;
    }
    if (line[i] != '`' || line[i + 1] != '`' || line[i + 2] != '`') {
        return false;
    }
    // Nothing else to validate — language tag / trailing text is allowed.
    return true;
}

// "^#{1,6} " — ATX heading prefix detector. Returns the marker length
// (including the trailing space) on match, 0 otherwise.
int HeadingMarkerLen(const std::string& line) {
    int hashes = 0;
    while (hashes < (int)line.size() && line[hashes] == '#') {
        ++hashes;
    }
    if (hashes < 1 || hashes > 6) {
        return 0;
    }
    if (hashes >= (int)line.size() || line[hashes] != ' ') {
        return 0;
    }
    return hashes + 1;
}

// "^> " — blockquote prefix. Returns 2 on match (the "> "), 0 otherwise.
int QuoteMarkerLen(const std::string& line) {
    if (line.size() < 2) {
        return 0;
    }
    if (line[0] != '>') {
        return 0;
    }
    if (line[1] != ' ') {
        return 0;
    }
    return 2;
}

// "^[-*+] " | "^\d+\. " — unordered or ordered list marker. Returns the
// length of the marker (including trailing space) on match, 0 otherwise.
// Leading whitespace is NOT consumed — the markdown spec allows up to three
// spaces of indent, but tracking nesting bloats the scanner. Plain prefix
// match only.
int ListMarkerLen(const std::string& line) {
    if (line.empty()) {
        return 0;
    }
    const char c = line[0];
    if (c == '-' || c == '*' || c == '+') {
        if (line.size() >= 2 && line[1] == ' ') {
            return 2;
        }
        return 0;
    }
    // Ordered: \d+\.\ —
    if (c >= '0' && c <= '9') {
        std::size_t i = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
            ++i;
        }
        if (i + 1 < line.size() && line[i] == '.' && line[i + 1] == ' ') {
            return (int)i + 2;
        }
    }
    return 0;
}

// Span comparator + overlap predicate for the inline-pass conflict resolver.
bool SpanLess(const TokenSpan& a, const TokenSpan& b) { return a.colStart < b.colStart; }

bool Overlaps(const TokenSpan& a, const TokenSpan& b) { return !(a.colEnd <= b.colStart || b.colEnd <= a.colStart); }

// Append a TokenSpan to `out` translating its line-relative col coordinates
// by `colOffset`. Used so an inline pass that operates on the line tail
// (after a list / quote marker is stripped) produces spans in source coords.
void Emit(std::vector<TokenSpan>& out, int line, int colStart, int colEnd, TokenKind kind, int colOffset) {
    if (colStart >= colEnd) {
        return;
    }
    TokenSpan s;
    s.line = line;
    s.colStart = colStart + colOffset;
    s.colEnd = colEnd + colOffset;
    s.kind = kind;
    out.push_back(s);
}

// Inline pass. Runs each regex independently then resolves overlap by
// keeping the earlier-starting span (process backticks first, then bold,
// then italic, then strike, then link). Italic is hand-rolled because
// std::regex on libstdc++ has spotty lookbehind support across MinGW UCRT.
void RunInlinePass(const std::string& line, int absLine, int colOffset, std::vector<TokenSpan>& out) {
    if (line.empty()) {
        return;
    }

    std::vector<TokenSpan> pass; // pre-conflict-resolution spans

    // 1) Backtick pairs `[^`\n]+`.
    {
        std::regex re("`([^`]+)`");
        for (std::sregex_iterator it(line.begin(), line.end(), re), end; it != end; ++it) {
            const std::smatch& m = *it;
            const int s = (int)m.position(0);
            const int e = s + (int)m.length(0);
            TokenSpan span;
            span.line = absLine;
            span.colStart = s + colOffset;
            span.colEnd = e + colOffset;
            span.kind = TokenKind::InlineCode;
            pass.push_back(span);
        }
    }

    // 2) Bold **...**.
    {
        std::regex re("\\*\\*([^*]+)\\*\\*");
        for (std::sregex_iterator it(line.begin(), line.end(), re), end; it != end; ++it) {
            const std::smatch& m = *it;
            const int s = (int)m.position(0);
            const int e = s + (int)m.length(0);
            // BoldMarker (2 chars) + BoldText (inside) + BoldMarker (2 chars)
            TokenSpan mk1;
            mk1.line = absLine;
            mk1.colStart = s + colOffset;
            mk1.colEnd = s + 2 + colOffset;
            mk1.kind = TokenKind::BoldMarker;
            pass.push_back(mk1);
            TokenSpan tx;
            tx.line = absLine;
            tx.colStart = s + 2 + colOffset;
            tx.colEnd = e - 2 + colOffset;
            tx.kind = TokenKind::BoldText;
            pass.push_back(tx);
            TokenSpan mk2;
            mk2.line = absLine;
            mk2.colStart = e - 2 + colOffset;
            mk2.colEnd = e + colOffset;
            mk2.kind = TokenKind::BoldMarker;
            pass.push_back(mk2);
        }
    }

    // 3) Italic *...* hand-rolled with neighbouring-char guard so we don't
    // collide with bold. Scan left-to-right; the open '*' must have a non-'*'
    // neighbour on each side (or a line boundary), same for the close.
    {
        const int n = (int)line.size();
        int i = 0;
        while (i < n) {
            if (line[i] != '*') {
                ++i;
                continue;
            }
            const bool prevOk = (i == 0) || (line[i - 1] != '*');
            const bool nextOk = (i + 1 < n) && (line[i + 1] != '*') && (line[i + 1] != ' ');
            if (!prevOk || !nextOk) {
                // Skip ahead past a run of '*' so bold openings don't restart
                // the scan inside the marker.
                ++i;
                while (i < n && line[i] == '*') {
                    ++i;
                }
                continue;
            }
            // Look for the matching close '*' with the same neighbour rule.
            int j = i + 1;
            int close = -1;
            while (j < n) {
                if (line[j] == '*') {
                    const bool prevOkC = (j - 1 < 0) || (line[j - 1] != '*' && line[j - 1] != ' ');
                    const bool nextOkC = (j + 1 >= n) || (line[j + 1] != '*');
                    if (prevOkC && nextOkC) {
                        close = j;
                        break;
                    }
                }
                ++j;
            }
            if (close < 0) {
                break;
            }
            TokenSpan mk1;
            mk1.line = absLine;
            mk1.colStart = i + colOffset;
            mk1.colEnd = i + 1 + colOffset;
            mk1.kind = TokenKind::ItalicMarker;
            pass.push_back(mk1);
            TokenSpan tx;
            tx.line = absLine;
            tx.colStart = i + 1 + colOffset;
            tx.colEnd = close + colOffset;
            tx.kind = TokenKind::ItalicText;
            pass.push_back(tx);
            TokenSpan mk2;
            mk2.line = absLine;
            mk2.colStart = close + colOffset;
            mk2.colEnd = close + 1 + colOffset;
            mk2.kind = TokenKind::ItalicMarker;
            pass.push_back(mk2);
            i = close + 1;
        }
    }

    // 4) Strike ~~...~~.
    {
        std::regex re("~~([^~]+)~~");
        for (std::sregex_iterator it(line.begin(), line.end(), re), end; it != end; ++it) {
            const std::smatch& m = *it;
            const int s = (int)m.position(0);
            const int e = s + (int)m.length(0);
            TokenSpan span;
            span.line = absLine;
            span.colStart = s + colOffset;
            span.colEnd = e + colOffset;
            span.kind = TokenKind::Strike;
            pass.push_back(span);
        }
    }

    // 5) Links [text](url).
    {
        std::regex re("\\[([^\\]]+)\\]\\(([^)]+)\\)");
        for (std::sregex_iterator it(line.begin(), line.end(), re), end; it != end; ++it) {
            const std::smatch& m = *it;
            const int s = (int)m.position(0);
            // [text] block: positions are relative to `line`. Match group 1 is
            // the link-text body; we span the whole `[...]` (including
            // brackets) under LinkText so the visual marker is colour-uniform.
            const int textBegin = s;
            const int textEnd = s + 1 + (int)m.length(1) + 1; // [ + text + ]
            const int urlBegin = textEnd;
            const int urlEnd = urlBegin + 1 + (int)m.length(2) + 1; // ( + url + )
            TokenSpan tx;
            tx.line = absLine;
            tx.colStart = textBegin + colOffset;
            tx.colEnd = textEnd + colOffset;
            tx.kind = TokenKind::LinkText;
            pass.push_back(tx);
            TokenSpan url;
            url.line = absLine;
            url.colStart = urlBegin + colOffset;
            url.colEnd = urlEnd + colOffset;
            url.kind = TokenKind::LinkUrl;
            pass.push_back(url);
        }
    }

    // Conflict resolution: sort by colStart; drop any span that overlaps an
    // earlier-accepted span. Backticks ran first in the pass-order above but
    // every span lands in the same `pass` vector; the stable sort + drop
    // policy keeps the leftmost / earliest-pass match per byte range.
    std::stable_sort(pass.begin(), pass.end(), SpanLess);
    int lastEnd = -1;
    for (std::size_t k = 0; k < pass.size(); ++k) {
        if (pass[k].colStart < lastEnd) {
            // Overlap with a previously-kept span — drop.
            continue;
        }
        // Bold pieces emit three spans in order; the middle (BoldText) starts
        // exactly at the previous span's colEnd which equals lastEnd. The `<`
        // guard above lets contiguous-but-not-overlapping pieces through.
        out.push_back(pass[k]);
        if (pass[k].colEnd > lastEnd) {
            lastEnd = pass[k].colEnd;
        }
    }
    (void)Overlaps; // referenced for future tightening
}

} // namespace

// Detect "> You:" / "> Assistant:" / "> Assistant (streaming...):" — the chat
// view's role-prefix lines. Returns 1 for user, 2 for assistant, 0 otherwise.
// `line` is the FULL quote line (including the leading "> "); the comparison
// runs on the bytes after "> ".
int DetectRoleMarker(const std::string& line) {
    if (line.size() < 3 || line[0] != '>' || line[1] != ' ') {
        return 0;
    }
    const char* p = line.c_str() + 2;
    const std::size_t n = line.size() - 2;
    // "You:" (4 chars). Tolerate any trailing characters because the chat view
    // emits exactly "> You:" today but we don't want to fail-closed on minor
    // variants.
    if (n >= 4 && std::strncmp(p, "You:", 4) == 0) {
        return 1;
    }
    if (n >= 10 && std::strncmp(p, "Assistant", 9) == 0) {
        // "Assistant:" or "Assistant (streaming...):" both start with the
        // same 9-byte prefix.
        return 2;
    }
    return 0;
}

std::vector<TokenSpan> Tokenize(const std::string& md) {
    std::vector<TokenSpan> out;
    if (md.empty()) {
        return out;
    }

    const std::vector<std::string> lines = SplitLines(md);
    bool inFence = false;
    bool inUserTurn = false;
    for (int li = 0; li < (int)lines.size(); ++li) {
        const std::string& line = lines[li];

        // Fence boundary — flips state. The marker line itself is FenceMarker.
        if (IsFenceLine(line)) {
            Emit(out, li, 0, (int)line.size(), TokenKind::FenceMarker, 0);
            inFence = !inFence;
            continue;
        }

        if (inFence) {
            // Entire line is code-fence body. No inline scan.
            Emit(out, li, 0, (int)line.size(), TokenKind::CodeFenceBody, 0);
            continue;
        }

        // Quote line. Detect the chat-specific role markers ("> You:" /
        // "> Assistant:") and flip the user-turn state so subsequent body
        // lines receive a UserMsgBody overlay span. Non-role quote lines fall
        // through to the regular QuoteMarker + inline pass.
        const int quoteLen = QuoteMarkerLen(line);
        if (quoteLen > 0) {
            const int role = DetectRoleMarker(line);
            if (role == 1) {
                inUserTurn = true;
                Emit(out, li, 0, (int)line.size(), TokenKind::UserRoleMarker, 0);
                continue;
            }
            if (role == 2) {
                inUserTurn = false;
                Emit(out, li, 0, (int)line.size(), TokenKind::AssistantRoleMarker, 0);
                continue;
            }
            Emit(out, li, 0, quoteLen, TokenKind::QuoteMarker, 0);
            RunInlinePass(line.substr((std::size_t)quoteLen), li, quoteLen, out);
            continue;
        }

        // Heading? Emit marker + inline pass on the remainder.
        const int headingLen = HeadingMarkerLen(line);
        if (headingLen > 0) {
            // User-turn overlay first, then heading marker + text so the
            // accent colours still win on top of the body tint.
            if (inUserTurn && !line.empty()) {
                Emit(out, li, 0, (int)line.size(), TokenKind::UserMsgBody, 0);
            }
            Emit(out, li, 0, headingLen, TokenKind::HeadingMarker, 0);
            const std::string rest = line.substr((std::size_t)headingLen);
            if (!rest.empty()) {
                TokenSpan tx;
                tx.line = li;
                tx.colStart = headingLen;
                tx.colEnd = (int)line.size();
                tx.kind = TokenKind::HeadingText;
                out.push_back(tx);
            }
            continue;
        }

        // List? Emit the marker + inline pass on the remainder.
        const int listLen = ListMarkerLen(line);
        if (listLen > 0) {
            if (inUserTurn && !line.empty()) {
                Emit(out, li, 0, (int)line.size(), TokenKind::UserMsgBody, 0);
            }
            Emit(out, li, 0, listLen, TokenKind::ListMarker, 0);
            RunInlinePass(line.substr((std::size_t)listLen), li, listLen, out);
            continue;
        }

        // Plain body line. UserMsgBody overlay (whole line) goes FIRST so the
        // inline pass spans (bold/italic/inline code/link) emitted after it
        // paint on top — the view's SetTokenColor calls run in vector order
        // and each call overwrites the prior color for its byte range.
        if (inUserTurn && !line.empty()) {
            Emit(out, li, 0, (int)line.size(), TokenKind::UserMsgBody, 0);
        }
        RunInlinePass(line, li, 0, out);
    }

    return out;
}

} // namespace AiChatMarkdownTokens
