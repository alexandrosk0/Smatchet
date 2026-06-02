#include "CppSyntaxHighlight.h"

#include "SmatchetTheme.h"
#include "Ui/CppSyntaxLex.h"
#include "imgui.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

ImVec4 Rgba(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

// Map a pure-lexer C/C++ token kind onto a concrete ImGui colour for the
// active theme. Default falls back to the current ImGui text colour.
ImVec4 ColorForCpp(CppLexKind kind, const SmatchetThemeSyntaxColors& colors) {
    switch (kind) {
    case CppLexKind::Comment:
        return Rgba(colors.Comment);
    case CppLexKind::String:
        return Rgba(colors.String);
    case CppLexKind::Preprocessor:
        return Rgba(colors.Preprocessor);
    case CppLexKind::Number:
        return Rgba(colors.Number);
    case CppLexKind::Keyword:
        return Rgba(colors.Keyword);
    case CppLexKind::Identifier:
        // Slice 6 of code-syntax-coloring-and-tooltips — identifiers use the
        // per-theme Identifier color (was ImGuiCol_Text, indistinguishable
        // from plain text).
        return Rgba(colors.Identifier);
    case CppLexKind::Default:
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

// Map a pure-lexer callstack token kind onto a concrete ImGui colour.
ImVec4 ColorForCallstack(CallstackLexKind kind, const SmatchetThemeSyntaxColors& colors) {
    switch (kind) {
    case CallstackLexKind::Class:
        return Rgba(colors.Identifier); // slice 6 — per-theme identifier tint
    case CallstackLexKind::Method:
        return Rgba(colors.Keyword); // method names get keyword colour for max signal
    case CallstackLexKind::Path:
        return Rgba(colors.Comment); // dim — paths are noisy
    case CallstackLexKind::Filename:
        return Rgba(colors.Number); // bright — filename is the load-bearing token
    case CallstackLexKind::LineNum:
        return Rgba(colors.Preprocessor); // distinct so `:18` reads as own token
    case CallstackLexKind::Default:
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

// Per-row token-emit helper. Push colour, TextUnformatted, pop; SameLine for
// every token after the first so the row renders as one continuous line.
struct LineEmitter {
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

} // namespace

void DrawColoredCppLine(const char* utf8Line) {
    if (!utf8Line) {
        return;
    }
    const std::size_t n = std::strlen(utf8Line);
    if (n == 0) {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        return;
    }

    const SmatchetThemeSyntaxColors& colors = SmatchetTheme::GetSyntaxColors();
    const std::vector<CppLexToken> tokens = CppLexLine(utf8Line, n);

    ImGui::PushID(utf8Line);
    LineEmitter e;
    for (std::size_t k = 0; k < tokens.size(); ++k) {
        const CppLexToken& t = tokens[k];
        e.Emit(utf8Line + t.offset, t.length, ColorForCpp(t.kind, colors));
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
    if (!CppLexLooksLikeCallstack(utf8Line, n)) {
        DrawColoredCppLine(utf8Line);
        return;
    }

    const SmatchetThemeSyntaxColors& colors = SmatchetTheme::GetSyntaxColors();
    const std::vector<CallstackLexToken> tokens = CppLexCallstackLine(utf8Line, n);

    ImGui::PushID(utf8Line);
    LineEmitter e;
    for (std::size_t k = 0; k < tokens.size(); ++k) {
        const CallstackLexToken& t = tokens[k];
        e.Emit(utf8Line + t.offset, t.length, ColorForCallstack(t.kind, colors));
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
