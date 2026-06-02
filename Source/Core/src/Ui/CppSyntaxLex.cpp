#include "Ui/CppSyntaxLex.h"

#include <algorithm>
#include <cstring>

namespace {

bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

bool IsIdentCont(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Characters that continue a numeric literal after the leading digit: more
// digits, the radix dot, hex prefix/digits (x/X, a-f, A-F), and the integer
// suffixes (u/l/L). Matches the original inline `||` chain byte-for-byte.
bool IsNumberCont(char c) {
    if (IsDigit(c)) {
        return true;
    }
    if (c == '.' || c == 'x' || c == 'X') {
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        return true;
    }
    return c == 'u' || c == 'l' || c == 'L';
}

// Scan a "// line comment" or "/* block comment" starting at i. Returns the
// one-past-end index of the comment span. `lineComment` is set true for `//`.
std::size_t ScanComment(const char* line, std::size_t n, std::size_t i, bool& lineComment) {
    if (line[i + 1] == '/') {
        lineComment = true;
        return n;
    }
    lineComment = false;
    std::size_t j = i + 2;
    while (j + 1 < n && !(line[j] == '*' && line[j + 1] == '/')) {
        ++j;
    }
    if (j + 1 < n) {
        return j + 2;
    }
    return n;
}

// Scan a quoted run ("..." or '...') starting at the opening quote at i.
// Returns the one-past-end index including the closing quote when present.
std::size_t ScanQuoted(const char* line, std::size_t n, std::size_t i, char quote) {
    std::size_t j = i + 1;
    while (j < n && line[j] != quote) {
        if (line[j] == '\\' && j + 1 < n) {
            j += 2;
        } else {
            ++j;
        }
    }
    if (j < n) {
        ++j;
    }
    return j;
}

// Scan a preprocessor run starting at '#' at i. Stops at NUL, '/', or '"'.
std::size_t ScanPreprocessor(const char* line, std::size_t n, std::size_t i) {
    std::size_t j = i;
    while (j < n && line[j] != '\0' && line[j] != '/' && line[j] != '"') {
        ++j;
    }
    return j;
}

void Push(std::vector<CppLexToken>& out, std::size_t off, std::size_t len, CppLexKind kind) {
    if (len == 0) {
        return;
    }
    CppLexToken t;
    t.offset = off;
    t.length = len;
    t.kind = kind;
    out.push_back(t);
}

void PushCs(std::vector<CallstackLexToken>& out, std::size_t off, std::size_t len, CallstackLexKind kind) {
    if (len == 0) {
        return;
    }
    CallstackLexToken t;
    t.offset = off;
    t.length = len;
    t.kind = kind;
    out.push_back(t);
}

} // namespace

bool CppLexIsKeyword(const std::string& w) {
    static const char* kws[] = {
        "alignas",     "alignof",   "and",        "and_eq",    "asm",      "auto",         "bitand",
        "bitor",       "bool",      "break",      "case",      "catch",    "char",         "char8_t",
        "char16_t",    "char32_t",  "class",      "compl",     "concept",  "const",        "consteval",
        "constexpr",   "constinit", "const_cast", "continue",  "co_await", "co_return",    "co_yield",
        "decltype",    "default",   "delete",     "do",        "double",   "dynamic_cast", "else",
        "enum",        "explicit",  "export",     "extern",    "false",    "float",        "for",
        "friend",      "goto",      "if",         "inline",    "int",      "long",         "mutable",
        "namespace",   "new",       "noexcept",   "not",       "not_eq",   "nullptr",      "operator",
        "or",          "or_eq",     "private",    "protected", "public",   "register",     "reinterpret_cast",
        "requires",    "return",    "short",      "signed",    "sizeof",   "static",       "static_assert",
        "static_cast", "struct",    "switch",     "template",  "this",     "thread_local", "throw",
        "true",        "try",       "typedef",    "typeid",    "typename", "union",        "unsigned",
        "using",       "virtual",   "void",       "volatile",  "wchar_t",  "while",        "xor",
        "xor_eq",      "override",  "final",      "import",    "module",   "uint8_t",      "uint16_t",
        "uint32_t",    "uint64_t",  "int8_t",     "int16_t",   "int32_t",  "int64_t",      "size_t",
        "ssize_t",     "nullptr_t"};
    return std::any_of(std::begin(kws), std::end(kws), [&](const char* k) { return w == k; });
}

bool CppLexLooksLikeCallstack(const char* line, std::size_t n) {
    const char* bang = static_cast<const char*>(std::memchr(line, '!', n));
    if (bang == nullptr) {
        return false;
    }
    const std::size_t afterBang = static_cast<std::size_t>((bang + 1) - line);
    if (afterBang >= n) {
        return false;
    }
    return std::memchr(bang + 1, '(', n - afterBang) != nullptr || std::memchr(bang + 1, '[', n - afterBang) != nullptr;
}

std::vector<CppLexToken> CppLexLine(const char* line, std::size_t n) {
    std::vector<CppLexToken> out;
    if (line == nullptr || n == 0) {
        return out;
    }
    std::size_t i = 0;
    while (i < n) {
        const char c = line[i];
        if (c == '/' && i + 1 < n && (line[i + 1] == '/' || line[i + 1] == '*')) {
            bool lineComment = false;
            const std::size_t j = ScanComment(line, n, i, lineComment);
            Push(out, i, j - i, CppLexKind::Comment);
            if (lineComment) {
                break;
            }
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            const std::size_t j = ScanQuoted(line, n, i, c);
            Push(out, i, j - i, CppLexKind::String);
            i = j;
            continue;
        }
        if (c == '#') {
            const std::size_t j = ScanPreprocessor(line, n, i);
            Push(out, i, j - i, CppLexKind::Preprocessor);
            i = j;
            continue;
        }
        if (IsDigit(c)) {
            std::size_t j = i + 1;
            while (j < n && IsNumberCont(line[j])) {
                ++j;
            }
            Push(out, i, j - i, CppLexKind::Number);
            i = j;
            continue;
        }
        if (IsIdentStart(c)) {
            std::size_t j = i + 1;
            while (j < n && IsIdentCont(line[j])) {
                ++j;
            }
            const std::string word(line + i, line + j);
            Push(out, i, j - i, CppLexIsKeyword(word) ? CppLexKind::Keyword : CppLexKind::Identifier);
            i = j;
            continue;
        }
        Push(out, i, 1, CppLexKind::Default);
        ++i;
    }
    return out;
}

namespace {

// Emit the module prefix (everything up to and including the first `!`) when
// the line has one. Advances *p past the `!`. No-op when there's no leading
// module segment.
void LexCsModule(const char* p, const char* end, std::vector<CallstackLexToken>& out, const char* base,
                 const char** outP) {
    const char* bang = static_cast<const char*>(std::memchr(p, '!', static_cast<std::size_t>(end - p)));
    if (bang != nullptr && bang > p) {
        PushCs(out, static_cast<std::size_t>(p - base), static_cast<std::size_t>(bang - p), CallstackLexKind::Default);
        PushCs(out, static_cast<std::size_t>(bang - base), 1, CallstackLexKind::Default);
        *outP = bang + 1;
        return;
    }
    *outP = p;
}

// Emit the symbol chain Class::Class::method up to chainEnd. The final segment
// is Method when followed immediately by `(`, else Class.
void LexCsChain(const char* p, const char* chainEnd, bool followedByParen, std::vector<CallstackLexToken>& out,
                const char* base) {
    const char* segStart = p;
    while (p < chainEnd) {
        if (p + 1 < chainEnd && p[0] == ':' && p[1] == ':') {
            PushCs(out, static_cast<std::size_t>(segStart - base), static_cast<std::size_t>(p - segStart),
                   CallstackLexKind::Class);
            PushCs(out, static_cast<std::size_t>(p - base), 2, CallstackLexKind::Default);
            p += 2;
            segStart = p;
            continue;
        }
        ++p;
    }
    if (segStart < chainEnd) {
        const CallstackLexKind k = followedByParen ? CallstackLexKind::Method : CallstackLexKind::Class;
        PushCs(out, static_cast<std::size_t>(segStart - base), static_cast<std::size_t>(chainEnd - segStart), k);
    }
}

// Emit the bracketed `[path\file.ext:line]` group at p (which points at `[`).
// Returns the one-past-end pointer.
const char* LexCsBracket(const char* p, const char* end, std::vector<CallstackLexToken>& out, const char* base) {
    const char* closeBracket = static_cast<const char*>(std::memchr(p, ']', static_cast<std::size_t>(end - p)));
    const char* bracketEnd = (closeBracket != nullptr) ? closeBracket : end;
    PushCs(out, static_cast<std::size_t>(p - base), 1, CallstackLexKind::Default);
    const char* bp = p + 1;
    const char* lastSep = nullptr;
    const char* lastColon = nullptr;
    for (const char* q = bp; q < bracketEnd; ++q) {
        if (*q == '\\' || *q == '/') {
            lastSep = q;
        }
        if (*q == ':') {
            lastColon = q;
        }
    }
    const char* filenameStart = (lastSep != nullptr) ? (lastSep + 1) : bp;
    const char* filenameEnd = (lastColon != nullptr && lastColon > filenameStart) ? lastColon : bracketEnd;
    if (filenameStart > bp) {
        PushCs(out, static_cast<std::size_t>(bp - base), static_cast<std::size_t>(filenameStart - bp),
               CallstackLexKind::Path);
    }
    if (filenameEnd > filenameStart) {
        PushCs(out, static_cast<std::size_t>(filenameStart - base),
               static_cast<std::size_t>(filenameEnd - filenameStart), CallstackLexKind::Filename);
    }
    if (filenameEnd < bracketEnd) {
        PushCs(out, static_cast<std::size_t>(filenameEnd - base), static_cast<std::size_t>(bracketEnd - filenameEnd),
               CallstackLexKind::LineNum);
    }
    if (closeBracket != nullptr) {
        PushCs(out, static_cast<std::size_t>(closeBracket - base), 1, CallstackLexKind::Default);
        return closeBracket + 1;
    }
    return end;
}

} // namespace

std::vector<CallstackLexToken> CppLexCallstackLine(const char* line, std::size_t n) {
    std::vector<CallstackLexToken> out;
    if (line == nullptr || n == 0) {
        return out;
    }
    const char* base = line;
    const char* p = line;
    const char* end = line + n;

    // 1. Module — everything up to the first `!`.
    LexCsModule(p, end, out, base, &p);

    // 2. Symbol chain up to either `(` or `[` or end.
    const char* paren = static_cast<const char*>(std::memchr(p, '(', static_cast<std::size_t>(end - p)));
    const char* bracket = static_cast<const char*>(std::memchr(p, '[', static_cast<std::size_t>(end - p)));
    const char* chainEnd = end;
    if (paren != nullptr) {
        chainEnd = paren;
    }
    if (bracket != nullptr && bracket < chainEnd) {
        chainEnd = bracket;
    }
    const bool followedByParen = (paren != nullptr && paren == chainEnd);
    LexCsChain(p, chainEnd, followedByParen, out, base);
    p = chainEnd;

    // 3. Parenthesised args (if present).
    if (p < end && *p == '(') {
        const char* closeParen = static_cast<const char*>(std::memchr(p, ')', static_cast<std::size_t>(end - p)));
        if (closeParen != nullptr) {
            PushCs(out, static_cast<std::size_t>(p - base), static_cast<std::size_t>(closeParen - p + 1),
                   CallstackLexKind::Default);
            p = closeParen + 1;
        } else {
            PushCs(out, static_cast<std::size_t>(p - base), static_cast<std::size_t>(end - p),
                   CallstackLexKind::Default);
            p = end;
        }
    }

    // 4. Whitespace then the bracket group ` [path\file.ext:line]`.
    while (p < end && *p != '[' && (*p == ' ' || *p == '\t')) {
        PushCs(out, static_cast<std::size_t>(p - base), 1, CallstackLexKind::Default);
        ++p;
    }
    if (p < end && *p == '[') {
        p = LexCsBracket(p, end, out, base);
    }

    // 5. Anything trailing.
    if (p < end) {
        PushCs(out, static_cast<std::size_t>(p - base), static_cast<std::size_t>(end - p), CallstackLexKind::Default);
    }
    return out;
}
