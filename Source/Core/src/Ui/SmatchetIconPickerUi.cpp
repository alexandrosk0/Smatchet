// Searchable Font-Awesome icon picker modal for the customizable toolbar. The catalog
// data + name->glyph lookup live in the ImGui-free SmatchetIconCatalog.cpp; this TU only
// renders the grid (virtualized with ImGuiListClipper) over that shared catalog.

#include "SmatchetIconPickerUi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "SmatchetImGuiFonts.h"
#include "Ui/SmatchetDialogScope.h"
#include "Ui/SmatchetInput.h"
#include "imgui.h"

#include "SmatchetLocalizedImGui.h"
// The alias routes the modal title, search hint, and Cancel button through the localization
// wrapper. Glyph buttons and icon-name tooltips are data and pass through untranslated.
#define ImGui SmatchetLocalizedImGui

namespace {
// Keys (smatchet::ui dialog contract): the search box takes the caret on open, Enter picks the icon when
// exactly one matches, Esc cancels. The search text is a filter, not work, so Esc closes in one press.
smatchet::ui::DialogScopeState s_iconPickerDialog;
} // namespace

void SmatchetIconPickerUi::Open() { requestOpen_ = true; }

bool SmatchetIconPickerUi::Draw(std::string& outName, std::string& outGlyph) {
    if (requestOpen_) {
        requestOpen_ = false;
        search_.clear();
        // "###" (not "##") so the popup ID survives title translation — OpenPopup is not
        // wrapped, so the raw and translated titles must hash to the same ID.
        ImGui::OpenPopup("Pick Icon###SmatchetIconPicker");
    }

    bool picked = false;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 320.0f), ImGuiCond_Appearing);
    if (smatchet::ui::BeginDialog(s_iconPickerDialog, "Pick Icon###SmatchetIconPicker", nullptr,
                                  ImGuiWindowFlags_NoSavedSettings)) {
        const smatchet::ui::DialogKeys keys = smatchet::ui::DialogReadKeys(s_iconPickerDialog, true);
        ImGui::SetNextItemWidth(-1.0f);
        smatchet::ui::DialogFocusHere(s_iconPickerDialog);
        // The query is a std::string (a long paste is kept whole) and a click selects it so a new one
        // replaces it. The dialog scope above owns the opening caret and Esc; this field only reports Enter.
        SmatchetInput::TextOptions searchOpts;
        searchOpts.hint = "Search icons...";
        searchOpts.selectAllOnFocus = true;
        const bool searchEnter = SmatchetInput::Text("##iconsearch", search_, searchOpts).enterCommitted;

        std::string q(search_);
        std::transform(q.begin(), q.end(), q.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const bool fa = SmatchetAreFaIconsLoaded();
        ImGui::BeginChild("##icongrid", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), true);

        // Build the visible list once per frame, then draw it with ImGuiListClipper so only
        // on-screen rows cost time. Slugs are stored lowercase, so a lowercased search box
        // matches by plain substring. About 1400 glyphs total — UX Pillar 1, 6.94 ms budget.
        const std::vector<SmatchetToolbarIconEntry>& catalog = SmatchetToolbarIconCatalog();
        std::vector<int> matches;
        matches.reserve(catalog.size());
        for (int i = 0; i < static_cast<int>(catalog.size()); ++i) {
            if (q.empty() || std::strstr(catalog[i].Name, q.c_str()) != nullptr) {
                matches.push_back(i);
            }
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        const float cell = ImGui::GetFrameHeight() + 10.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        const int perRow =
            (std::max)(1, static_cast<int>((avail + style.ItemSpacing.x) / (cell + style.ItemSpacing.x)));
        const int rowCount = (static_cast<int>(matches.size()) + perRow - 1) / perRow;

        ImGuiListClipper clipper;
        clipper.Begin(rowCount, cell + style.ItemSpacing.y);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int colN = 0; colN < perRow; ++colN) {
                    const int mi = row * perRow + colN;
                    if (mi >= static_cast<int>(matches.size())) {
                        break;
                    }
                    if (colN != 0) {
                        ImGui::SameLine();
                    }
                    const SmatchetToolbarIconEntry& e = catalog[matches[mi]];
                    ImGui::PushID(e.Name);
                    const char* label = (fa && e.Glyph[0] != '\0') ? e.Glyph : "?";
                    // Raw (::): glyph label is data; skips the wrapper's per-cell string pass
                    // (~80 visible cells per frame under the clipper).
                    if (::ImGui::Button(label, ImVec2(cell, cell))) {
                        outName = e.Name;
                        outGlyph = e.Glyph;
                        picked = true;
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered()) {
                        ::ImGui::SetTooltip("%s", e.Name);
                    }
                    ImGui::PopID();
                }
            }
        }
        clipper.End();
        ImGui::EndChild();

        if (!picked && searchEnter && matches.size() == 1) {
            const SmatchetToolbarIconEntry& e = catalog[matches[0]];
            outName = e.Name;
            outGlyph = e.Glyph;
            picked = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) ||
            (!picked && keys.Esc == smatchet::dialogkeys::EscapeAction::Cancel)) {
            ImGui::CloseCurrentPopup();
        }
        smatchet::ui::EndDialog(s_iconPickerDialog);
    }
    return picked;
}
