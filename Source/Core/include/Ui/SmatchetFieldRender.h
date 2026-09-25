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

/** The one Markdown hover-tooltip path, shared by the description, comments and History
 *  tooltips: `MarkdownPreviewRender` in Tooltip mode at a fixed wrap width, inside a child
 *  that auto-fits the content and scrolls once taller than half the display. Opens and
 *  closes the tooltip itself; empty `markdown` opens nothing. UI thread only.
 *  (History reaches it through `RenderClippedFieldText`, keyed on the field id.) */
void RenderMarkdownTooltip(const std::string& markdown);

/** Tell the renderer which tracker field id holds C/C++ callstack source so it
 *  can paint the grid cell + overflow tooltip with `DrawColoredCppText`. */
void SetCallstackFieldIdHint(const std::string& fieldId);

/** True when the supplied tracker field id matches the configured callstack
 *  field hint (see `SetCallstackFieldIdHint`). Public so other render surfaces
 *  (TicketFieldEditor's long-text editor + markdown preview path) can branch
 *  on it to apply slice 7's callstack-aware tokenizer instead of the default
 *  markdown / prose rendering. Empty `fieldId` always returns false. */
bool IsCallstackFieldId(const std::string& fieldId);
