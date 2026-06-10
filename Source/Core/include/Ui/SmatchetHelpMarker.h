#pragma once

// Inline (?) help marker: a dimmed circled-question-mark glyph whose hover
// tooltip carries the long-form explanation that used to live inline in the
// panel. Pairs with a shortened visible label — the full original text moves
// into the tooltip, keeping panels compact without losing information.
// Caller is responsible for placement (ImGui::SameLine() before the marker).
// The tooltip fires inside ImGui::BeginDisabled blocks too
// (ImGuiHoveredFlags_AllowWhenDisabled) so help stays reachable there.
namespace SmatchetHelpMarker {

// Localized variant: tooltip body resolved via SmatchetLocalization::T(key, fallback).
void Render(const char* tooltipKey, const char* tooltipFallback);

// Pre-translated / dynamic variant: renders `fullText` (UTF-8, may contain
// '\n') verbatim in the tooltip. Use for snprintf/std::string-built bodies.
void RenderText(const char* fullText);

} // namespace SmatchetHelpMarker
