#include "BlameSyntaxHighlight.h"

#include "imgui.h"

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

void BlameDrawColoredCppLine(const char* utf8Line, const BlameUiThemeColors& theme) {
    if (!utf8Line) {
        return;
    }
    const size_t n = std::strlen(utf8Line);
    if (n == 0) {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        return;
    }

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
            emit(utf8Line + i, n - i, Rgba(theme.SyntaxComment));
            i = n;
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
            emit(utf8Line + i, j - i, Rgba(theme.SyntaxComment));
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
            emit(utf8Line + i, j - i, Rgba(theme.SyntaxString));
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
            emit(utf8Line + i, j - i, Rgba(theme.SyntaxString));
            i = j;
            continue;
        }
        if (c == '#') {
            size_t j = i;
            while (j < n && utf8Line[j] != '\0' && utf8Line[j] != '/' && utf8Line[j] != '"') {
                ++j;
            }
            emit(utf8Line + i, j - i, Rgba(theme.SyntaxPreprocessor));
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
            emit(utf8Line + i, j - i, Rgba(theme.SyntaxNumber));
            i = j;
            continue;
        }
        if (IsIdentStart(c)) {
            size_t j = i + 1;
            while (j < n && IsIdentCont(utf8Line[j])) {
                ++j;
            }
            const std::string word(utf8Line + i, utf8Line + j);
            const ImVec4 col = IsKeyword(word) ? Rgba(theme.SyntaxKeyword) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            emit(utf8Line + i, j - i, col);
            i = j;
            continue;
        }
        emit(utf8Line + i, 1, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ++i;
    }
    ImGui::PopID();
}






