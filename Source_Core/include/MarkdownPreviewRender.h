#pragma once
#include <string>

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
};

void Render(const std::string& md, const Options& opts = Options());

} // namespace MarkdownPreviewRender
