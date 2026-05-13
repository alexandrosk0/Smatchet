#pragma once

#include <string>

/** Shared clipped text renderer for grid and detail panels. When `renderMarkdown`
 *  is true the tooltip body is rendered as Markdown via `MarkdownPreviewRender` in
 *  Tooltip mode; otherwise the tooltip is plain wrapped text. */
void RenderClippedFieldText(const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool disabled,
                            const std::string* rawForTooltip = nullptr, bool renderMarkdown = false);
