#include "Ui/SmatchetIconButtons.h"

#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#include "SmatchetLocalizedImGui.h"
#include "imgui.h"

#include <string>

namespace {

// Localized hover tooltip on the just-drawn item. Mirrors
// SmatchetLocalizedImGui::SetTooltip (which translates its format string): the
// English `tooltip` is translated as a whole and shown via "%s" so any '%' it
// contains is treated literally. nullptr/"" skips the tooltip entirely (used by
// call sites that keep their own external SetItemTooltip).
void TooltipForLastItem(const char* tooltip) {
    if (tooltip && tooltip[0] != '\0' && ::ImGui::IsItemHovered()) {
        ::ImGui::SetTooltip("%s", SmatchetLocalization::TranslateSource(tooltip));
    }
}

} // namespace

bool SmatchetIconButton(const char* icon, const char* fallbackLabel, const char* tooltip, const ImVec2& size) {
    // Re-check per call — the FA merge state is invalidated by font hot-reload (a
    // language / font-size change rebuilds the atlas mid-session), mirroring
    // SmatchetHelpMarker::RenderText.
    const bool fa = SmatchetAreFaIconsLoaded();
    const std::string label = SmatchetIconButtonImGuiLabel(fa, icon, fallbackLabel);
    // Route through the localization wrapper so this helper localizes exactly like
    // a plain ImGui::Button call site did before:
    //   - icon path  ("<glyph>##<fallback>"): LabelFromSource splits at "##", sees a
    //     glyph-only visible part (no dictionary entry) and returns it unchanged —
    //     the glyph is shown, the English fallback stays a stable hidden id.
    //   - text path  ("<fallback>"): LabelFromSource translates the visible text and
    //     appends a stable English-derived id (e.g. "Actualiser##Refresh").
    const bool clicked = SmatchetLocalizedImGui::Button(label.c_str(), size);
    TooltipForLastItem(tooltip);
    return clicked;
}

bool SmatchetIconLeadingButton(const char* icon, const char* label, const char* tooltip, const ImVec2& size) {
    const bool fa = SmatchetAreFaIconsLoaded();
    // Translate the text FIRST: a composed "<glyph> <text>" string would never match
    // the dictionary (the glyph is part of the lookup key), so we cannot route it
    // through LabelFromSource. The id is then pinned to the English label (see below).
    // NB: this differs from SmatchetIconButton, which defers id derivation to
    // LabelFromSource; the two helpers build ids by different rules on purpose
    // (each is internally id-stable + unique) — do not "unify" them.
    const char* translated = SmatchetLocalization::TranslateSource(label ? label : "");
    std::string composed = SmatchetIconLeadingLabel(fa, icon, translated);
    // "###" (not "##"): only the triple form RESTARTS the ImGui id hash, so the id
    // is derived from the English label alone — independent of both the UI language
    // (translated visible text) and the leading glyph (which appears only once the
    // FA font finishes loading async). A "##" suffix would fold the glyph + visible
    // text into the id, flipping it across the font-load frame and dropping focus.
    composed += "###";
    composed += (label ? label : "");
    const bool clicked = ::ImGui::Button(composed.c_str(), size);
    TooltipForLastItem(tooltip);
    return clicked;
}
