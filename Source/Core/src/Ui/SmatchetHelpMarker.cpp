#include "SmatchetHelpMarker.h"

#include "IconsFontAwesome6.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#include "imgui.h"

namespace SmatchetHelpMarker {

namespace {

// Draw the tooltip body. Shared by the hover and keyboard-focus paths so the
// two stay byte-identical (the #1128 parity requirement: focus must surface
// exactly what hover surfaces).
void DrawTooltipBody(const char* fullText) {
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(fullText);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

void RenderText(const char* fullText) {
    if (!fullText || fullText[0] == '\0') {
        return;
    }
    // Re-check per call — the FA merge state is invalidated by font hot-reload
    // (language / font-size change rebuilds the atlas mid-session).
    const char* glyph = SmatchetAreFaIconsLoaded() ? ICON_FA_CIRCLE_QUESTION : "(?)";

    // #1128: the marker must be reachable by keyboard nav, not just mouse hover.
    // A plain TextUnformatted item never participates in nav, so a keyboard-only
    // user could never surface the long-form help. We render the glyph over an
    // InvisibleButton carrying ImGuiButtonFlags_EnableNav: InvisibleButton is
    // non-navigable by default, and that flag opts it INTO Tab/arrow nav without
    // perturbing the mouse tab-order — mouse users see no new stop, keyboard
    // users can Tab to it (the "nav-only focusable" decision). The id derives
    // from the body text so repeated markers on one panel stay distinct.
    const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::PushID(fullText);
    ImGui::InvisibleButton("##helpmarker", glyphSize, ImGuiButtonFlags_EnableNav);
    ImGui::PopID();

    // Paint the dimmed glyph on top of the (zero-visual) button footprint.
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImGui::GetWindowDrawList()->AddText(origin, col, glyph);

    // Parity: tooltip shows on hover OR keyboard/gamepad focus. AllowWhenDisabled
    // keeps help reachable inside BeginDisabled() blocks (preserved behavior).
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled);
    if (hovered || ImGui::IsItemFocused()) {
        DrawTooltipBody(fullText);
    }
}

void Render(const char* tooltipKey, const char* tooltipFallback) {
    RenderText(SmatchetLocalization::T(tooltipKey, tooltipFallback));
}

} // namespace SmatchetHelpMarker
