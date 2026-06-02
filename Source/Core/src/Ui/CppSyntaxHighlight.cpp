#include "CppSyntaxHighlight.h"

#include "SmatchetTheme.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

bool IsIdentCont(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }

bool IsKeyword(const std::string& w) {
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

ImVec4 Rgba(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

} // namespace

void DrawColoredCppLine(const char* utf8Line) {
    if (!utf8Line) {
        return;
    }
    const size_t n = std::strlen(utf8Line);
    if (n == 0) {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        return;
    }

    const SmatchetThemeSyntaxColors& colors = SmatchetTheme::GetSyntaxColors();

    size_t i = 0;
    bool firstTok = true;
    auto emit = [&](const char* s, size_t len, const ImVec4& col) {
        if (len == 0) {
            return;
        }
        if (!firstTok) {
            ImGui::SameLine(0.f, 0.f);
        }
        firstTok = false;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(s, s + len);
        ImGui::PopStyleColor();
    };

    ImGui::PushID(utf8Line);
    while (i < n) {
        const char c = utf8Line[i];
        if (c == '/' && i + 1 < n && utf8Line[i + 1] == '/') {
            emit(utf8Line + i, n - i, Rgba(colors.Comment));
            break;
        }
        if (c == '/' && i + 1 < n && utf8Line[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(utf8Line[j] == '*' && utf8Line[j + 1] == '/')) {
                ++j;
            }
            if (j + 1 < n) {
                j += 2;
            } else {
                j = n;
            }
            emit(utf8Line + i, j - i, Rgba(colors.Comment));
            i = j;
            continue;
        }
        if (c == '"') {
            size_t j = i + 1;
            while (j < n && utf8Line[j] != '"') {
                if (utf8Line[j] == '\\' && j + 1 < n) {
                    j += 2;
                } else {
                    ++j;
                }
            }
            if (j < n) {
                ++j;
            }
            emit(utf8Line + i, j - i, Rgba(colors.String));
            i = j;
            continue;
        }
        if (c == '\'') {
            size_t j = i + 1;
            while (j < n && utf8Line[j] != '\'') {
                if (utf8Line[j] == '\\' && j + 1 < n) {
                    j += 2;
                } else {
                    ++j;
                }
            }
            if (j < n) {
                ++j;
            }
            emit(utf8Line + i, j - i, Rgba(colors.String));
            i = j;
            continue;
        }
        if (c == '#') {
            size_t j = i;
            while (j < n && utf8Line[j] != '\0' && utf8Line[j] != '/' && utf8Line[j] != '"') {
                ++j;
            }
            emit(utf8Line + i, j - i, Rgba(colors.Preprocessor));
            i = j;
            continue;
        }
        if (c >= '0' && c <= '9') {
            size_t j = i + 1;
            while (j < n && ((utf8Line[j] >= '0' && utf8Line[j] <= '9') || utf8Line[j] == '.' || utf8Line[j] == 'x' ||
                             utf8Line[j] == 'X' || utf8Line[j] == 'a' || utf8Line[j] == 'b' || utf8Line[j] == 'c' ||
                             utf8Line[j] == 'd' || utf8Line[j] == 'e' || utf8Line[j] == 'f' || utf8Line[j] == 'A' ||
                             utf8Line[j] == 'B' || utf8Line[j] == 'C' || utf8Line[j] == 'D' || utf8Line[j] == 'E' ||
                             utf8Line[j] == 'F' || utf8Line[j] == 'u' || utf8Line[j] == 'l' || utf8Line[j] == 'L')) {
                ++j;
            }
            emit(utf8Line + i, j - i, Rgba(colors.Number));
            i = j;
            continue;
        }
        if (IsIdentStart(c)) {
            size_t j = i + 1;
            while (j < n && IsIdentCont(utf8Line[j])) {
                ++j;
            }
            const std::string word(utf8Line + i, utf8Line + j);
            // Slice 6 of code-syntax-coloring-and-tooltips —
            // identifiers now use the per-theme Identifier color (was:
            // ImGuiCol_Text, indistinguishable from plain text).
            const ImVec4 col = IsKeyword(word) ? Rgba(colors.Keyword) : Rgba(colors.Identifier);
            emit(utf8Line + i, j - i, col);
            i = j;
            continue;
        }
        emit(utf8Line + i, 1, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ++i;
    }
    ImGui::PopID();
}

void DrawColoredCppText(const char* utf8Multiline) {
    if (!utf8Multiline) {
        return;
    }
    const char* p = utf8Multiline;
    std::string line;
    while (true) {
        const char ch = *p;
        if (ch == '\0' || ch == '\n') {
            DrawColoredCppLine(line.c_str());
            line.clear();
            if (ch == '\0') {
                break;
            }
            ++p;
            continue;
        }
        if (ch != '\r') {
            line.push_back(ch);
        }
        ++p;
    }
}

namespace {

// Per-row token-emit helper for the callstack tokenizer. Same shape as
// DrawColoredCppLine's lambda — push colour, TextUnformatted, pop, SameLine
// for the next token on the same ImGui row.
struct CallstackLineEmitter {
    bool firstTok = true;
    void Emit(const char* s, std::size_t len, const ImVec4& col) {
        if (len == 0) {
            return;
        }
        if (!firstTok) {
            ImGui::SameLine(0.f, 0.f);
        }
        firstTok = false;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(s, s + len);
        ImGui::PopStyleColor();
    }
};

// True iff the line looks like a canonical callstack frame —
// has `!` separating module from symbol AND has `(` somewhere after.
bool LooksLikeCallstack(const char* line, std::size_t n) {
    const char* bang = static_cast<const char*>(std::memchr(line, '!', n));
    if (bang == nullptr) {
        return false;
    }
    const std::size_t afterBang = static_cast<std::size_t>((bang + 1) - line);
    if (afterBang >= n) {
        return false;
    }
    // Look for `(` after the bang OR an `[` bracket group — either is a
    // strong signal this is a frame, not a random `foo!bar` string.
    return std::memchr(bang + 1, '(', n - afterBang) != nullptr || std::memchr(bang + 1, '[', n - afterBang) != nullptr;
}

} // namespace

void DrawColoredCallstackLine(const char* utf8Line) {
    if (!utf8Line) {
        return;
    }
    const std::size_t n = std::strlen(utf8Line);
    if (n == 0) {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        return;
    }

    // Non-callstack-looking lines fall through to the generic cpp highlighter
    // so the row still renders readably. Cheap precheck means orchestrator
    // never has to pre-classify the buffer.
    if (!LooksLikeCallstack(utf8Line, n)) {
        DrawColoredCppLine(utf8Line);
        return;
    }

    const SmatchetThemeSyntaxColors& colors = SmatchetTheme::GetSyntaxColors();
    const ImVec4 cDefault = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 cClass = Rgba(colors.Identifier);     // slice 6 — per-theme identifier tint
    const ImVec4 cMethod = Rgba(colors.Keyword);       // method names get keyword colour for max signal
    const ImVec4 cPath = Rgba(colors.Comment);         // dim — paths are noisy
    const ImVec4 cFilename = Rgba(colors.Number);      // bright — filename is the load-bearing token
    const ImVec4 cLineNum = Rgba(colors.Preprocessor); // distinct from filename so `:18` reads as own token

    ImGui::PushID(utf8Line);
    CallstackLineEmitter e;

    // Walk pointers into the input — no heap allocations on the hot path.
    const char* p = utf8Line;
    const char* end = utf8Line + n;

    // 1. Module — everything up to the first `!`.
    const char* bang = static_cast<const char*>(std::memchr(p, '!', static_cast<std::size_t>(end - p)));
    if (bang != nullptr && bang > p) {
        e.Emit(p, static_cast<std::size_t>(bang - p), cDefault);
        e.Emit("!", 1, cDefault);
        p = bang + 1;
    }

    // 2. Symbol chain (Class::Class::method) up to either `(` or `[` or end.
    const char* paren = static_cast<const char*>(std::memchr(p, '(', static_cast<std::size_t>(end - p)));
    const char* bracket = static_cast<const char*>(std::memchr(p, '[', static_cast<std::size_t>(end - p)));
    const char* chainEnd = end;
    if (paren != nullptr) {
        chainEnd = paren;
    }
    if (bracket != nullptr && bracket < chainEnd) {
        chainEnd = bracket;
    }
    // Split on `::` — emit each segment with the right colour.
    const char* segStart = p;
    while (p < chainEnd) {
        if (p + 1 < chainEnd && p[0] == ':' && p[1] == ':') {
            // segStart..p is a class identifier; emit then the `::`.
            e.Emit(segStart, static_cast<std::size_t>(p - segStart), cClass);
            e.Emit("::", 2, cDefault);
            p += 2;
            segStart = p;
            continue;
        }
        ++p;
    }
    // Final segment in the chain — if there's a `(` immediately after, it's a
    // method; otherwise (rare — `kernel32!ntdll` shape) treat as class-like.
    if (segStart < chainEnd) {
        const bool followedByParen = (paren != nullptr && paren == chainEnd);
        const ImVec4 finalCol = followedByParen ? cMethod : cClass;
        e.Emit(segStart, static_cast<std::size_t>(chainEnd - segStart), finalCol);
    }
    p = chainEnd;

    // 3. Parenthesised args (if present) — emit as default text + skip past `)`.
    if (p < end && *p == '(') {
        const char* closeParen = static_cast<const char*>(std::memchr(p, ')', static_cast<std::size_t>(end - p)));
        if (closeParen != nullptr) {
            e.Emit(p, static_cast<std::size_t>(closeParen - p + 1), cDefault);
            p = closeParen + 1;
        } else {
            // unbalanced — emit rest as default and exit.
            e.Emit(p, static_cast<std::size_t>(end - p), cDefault);
            p = end;
        }
    }

    // 4. Space + bracket group: ` [path\file.ext:line]`.
    // Emit any whitespace up to the `[` as default.
    while (p < end && *p != '[' && (*p == ' ' || *p == '\t')) {
        e.Emit(p, 1, cDefault);
        ++p;
    }
    if (p < end && *p == '[') {
        const char* closeBracket = static_cast<const char*>(std::memchr(p, ']', static_cast<std::size_t>(end - p)));
        const char* bracketEnd = (closeBracket != nullptr) ? closeBracket : end;
        // Emit `[`
        e.Emit("[", 1, cDefault);
        const char* bp = p + 1;
        // Inside the brackets: find last path separator + last `:` to split
        // into path / filename / lineno.
        const char* lastSep = nullptr;
        for (const char* q = bp; q < bracketEnd; ++q) {
            if (*q == '\\' || *q == '/') {
                lastSep = q;
            }
        }
        // The line-number colon is the LAST `:` in the bracket range (after the
        // file path's drive `C:`).
        const char* lastColon = nullptr;
        for (const char* q = bp; q < bracketEnd; ++q) {
            if (*q == ':') {
                lastColon = q;
            }
        }
        const char* filenameStart = (lastSep != nullptr) ? (lastSep + 1) : bp;
        const char* filenameEnd = (lastColon != nullptr && lastColon > filenameStart) ? lastColon : bracketEnd;
        // Path portion (everything before filename) — dim.
        if (filenameStart > bp) {
            e.Emit(bp, static_cast<std::size_t>(filenameStart - bp), cPath);
        }
        // Filename — bright.
        if (filenameEnd > filenameStart) {
            e.Emit(filenameStart, static_cast<std::size_t>(filenameEnd - filenameStart), cFilename);
        }
        // `:lineNumber` — own colour, includes the `:`.
        if (filenameEnd < bracketEnd) {
            e.Emit(filenameEnd, static_cast<std::size_t>(bracketEnd - filenameEnd), cLineNum);
        }
        if (closeBracket != nullptr) {
            e.Emit("]", 1, cDefault);
            p = closeBracket + 1;
        } else {
            p = end;
        }
    }

    // 5. Anything trailing — emit as default text.
    if (p < end) {
        e.Emit(p, static_cast<std::size_t>(end - p), cDefault);
    }

    ImGui::PopID();
}

void DrawColoredCallstackText(const char* utf8Multiline) {
    if (!utf8Multiline) {
        return;
    }
    const char* p = utf8Multiline;
    std::string line;
    while (true) {
        const char ch = *p;
        if (ch == '\0' || ch == '\n') {
            DrawColoredCallstackLine(line.c_str());
            line.clear();
            if (ch == '\0') {
                break;
            }
            ++p;
            continue;
        }
        if (ch != '\r') {
            line.push_back(ch);
        }
        ++p;
    }
}
