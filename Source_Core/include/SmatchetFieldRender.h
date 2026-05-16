#pragma once

#include <string>

/** Shared clipped text renderer for grid and detail panels. When `renderMarkdown`
 *  is true the tooltip body is rendered as Markdown via `MarkdownPreviewRender` in
 *  Tooltip mode; otherwise the tooltip is plain wrapped text.
 *
 *  Pass `fieldId` so the renderer can opt the value into C++ syntax highlighting
 *  when it matches the configured callstack tracker-field id (see
 *  `SetCallstackFieldIdHint`). */
void RenderClippedFieldText(const std::string& rawValue, float availWidth, bool tooltipsEnabled, bool disabled,
                            const std::string* rawForTooltip = nullptr, bool renderMarkdown = false,
                            const std::string* fieldId = nullptr);

/** Tell the renderer which tracker field id holds C/C++ callstack source so it
 *  can paint the grid cell + overflow tooltip with `DrawColoredCppText`. */
void SetCallstackFieldIdHint(const std::string& fieldId);
