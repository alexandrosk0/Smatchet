#include "SmatchetThemedTextEditorPalette.h"

#include "SmatchetTheme.h"
#include "imgui.h"

namespace SmatchetTheme {

namespace {

// Pack an ImVec4 (rgba in [0,1]) into a TextEditor-style ImU32 (ABGR little-
// endian; matches the literal layout used in TextEditor::GetDarkPalette()
// where `0xff7f7f7f` = full alpha, mid grey). Clamp before pack so out-of-
// range floats from custom themes don't wrap.
ImU32 ImU32FromVec4(const ImVec4& v) {
    auto pack = [](float f) -> uint32_t {
        if (f <= 0.0f)
            return 0u;
        if (f >= 1.0f)
            return 255u;
        return static_cast<uint32_t>(f * 255.0f + 0.5f);
    };
    const uint32_t r = pack(v.x);
    const uint32_t g = pack(v.y);
    const uint32_t b = pack(v.z);
    const uint32_t a = pack(v.w);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// Pack a 4-float array (the layout `SmatchetThemeSyntaxColors` stores) the
// same way ImU32FromVec4 does. Both helpers exist because the two source
// types differ (ImGui's ImVec4 vs the plain-array struct in SmatchetTheme.h)
// and we want the conversion intent obvious at each call site.
ImU32 ImU32FromArray4(const float (&v)[4]) { return ImU32FromVec4(ImVec4(v[0], v[1], v[2], v[3])); }

} // namespace

TextEditor::Palette GetThemedLuaConsolePalette() {
    // Branch on the live ImGui style brightness: light theme → start from the
    // built-in light palette so the editor background reads correctly; every
    // other theme starts from the dark palette. After picking the base, we
    // overlay the active SmatchetThemeSyntaxColors palette onto the five
    // syntax slots so theme switches reach the editor.
    // The "is this a light theme?" check looks at WindowBg luminance. Cheap,
    // robust against future theme additions, and avoids hard-coding a list of
    // ThemeIds in a TU that should stay theme-agnostic.
    const ImVec4& wb = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const float luma = 0.2126f * wb.x + 0.7152f * wb.y + 0.0722f * wb.z;
    const bool isLightTheme = luma > 0.5f;

    TextEditor::Palette p = isLightTheme ? TextEditor::GetLightPalette() : TextEditor::GetDarkPalette();

    const SmatchetThemeSyntaxColors& syn = SmatchetTheme::GetSyntaxColors();
    using PI = TextEditor::PaletteIndex;
    // Syntax overrides — these are the five slots the C++ tokenizer most
    // visibly uses and the same ones SmatchetThemeSyntaxColors carries.
    p[(int)PI::Keyword] = ImU32FromArray4(syn.Keyword);
    p[(int)PI::String] = ImU32FromArray4(syn.String);
    p[(int)PI::Comment] = ImU32FromArray4(syn.Comment);
    p[(int)PI::MultiLineComment] = ImU32FromArray4(syn.Comment); // same hue as single-line
    p[(int)PI::Number] = ImU32FromArray4(syn.Number);
    p[(int)PI::Preprocessor] = ImU32FromArray4(syn.Preprocessor);
    // Slice 6 of code-syntax-coloring-and-tooltips — identifier
    // slot now uses the per-theme Identifier color (was: ImGuiCol_Text, which
    // blended identifiers into plain text). Affects all editor-embedding sites
    // (Lua console, AI chat editor, ticket-field long-text editor) — desired:
    // identifier coloring is visibly useful everywhere code is rendered.
    p[(int)PI::Identifier] = ImU32FromArray4(syn.Identifier);
    p[(int)PI::KnownIdentifier] = ImU32FromArray4(syn.Identifier);
    p[(int)PI::PreprocIdentifier] = ImU32FromArray4(syn.Identifier);

    // Editor background, line numbers, and cursor follow the active ImGui
    // theme so the editor blends with the surrounding panel. Without these
    // overrides the editor would still show the hard-coded dark/light bg
    // even on Norton-Commander-teal panels.
    p[(int)PI::Background] = ImU32FromVec4(ImGui::GetStyle().Colors[ImGuiCol_ChildBg]);
    p[(int)PI::Default] = ImU32FromVec4(ImGui::GetStyle().Colors[ImGuiCol_Text]);
    p[(int)PI::Punctuation] = ImU32FromVec4(ImGui::GetStyle().Colors[ImGuiCol_Text]);
    p[(int)PI::Cursor] = ImU32FromVec4(ImGui::GetStyle().Colors[ImGuiCol_Text]);
    p[(int)PI::Selection] = ImU32FromVec4(ImGui::GetStyle().Colors[ImGuiCol_TextSelectedBg]);

    return p;
}

} // namespace SmatchetTheme
