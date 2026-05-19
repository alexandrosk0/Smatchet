#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace SelectableText {
struct Context;
} // namespace SelectableText

/// Shared Markdown preview renderer. Walks a Markdown string with md4c and emits
/// ImGui draw calls. Used by the long-text editor modal (Full mode) and the
/// description tooltip path (Tooltip mode — drops heading scaling, code BeginChild,
/// and link click handling so nothing breaks when nested inside a tooltip window).
namespace MarkdownPreviewRender {

enum class Mode { Full, Tooltip };

struct Options {
    Mode mode = Mode::Full;
    /// <=0 falls back to ImGui::GetContentRegionAvail().x at render time.
    float wrapWidth = 0.0f;
    /// Off in Tooltip mode — tooltip dismisses on mouse-up so the click never lands.
    bool clickableLinks = true;
    /// When non-empty AND `existingSelCtx == nullptr`, wraps the render in
    /// SelectableText::Begin(selectableId) / End() so prose is drag-selectable
    /// + Ctrl+C-copyable. Code blocks + tables remain non-selectable in MVP.
    /// See docs/design/markdown-language-definition-for-textedit.md § Slice 4.
    const char* selectableId = nullptr;
    /// When non-null, prose runs register Segments into THIS Context instead
    /// of opening a fresh one. Used by the AI chat surface where one outer
    /// Context spans many sequential Render() calls (one per message) so
    /// drag-select crosses message boundaries. Overrides `selectableId`.
    SelectableText::Context* existingSelCtx = nullptr;
};

void Render(const std::string& md, const Options& opts = Options());

/// Return true when an md4c fenced-code language tag should route to the C++
/// syntax tokenizer. Case-insensitive; covers common aliases. Inlined so the
/// pure-logic test rig can link this without pulling md4c / ImGui / fonts.
inline bool IsCppLikeLangTag(const std::string& lang) {
    if (lang.empty()) {
        return false;
    }
    std::string lower(lang.size(), '\0');
    std::transform(lang.begin(), lang.end(), lower.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return lower == "cpp" || lower == "c++" || lower == "cxx" || lower == "cc" || lower == "c" || lower == "hpp" ||
           lower == "h";
}

} // namespace MarkdownPreviewRender
