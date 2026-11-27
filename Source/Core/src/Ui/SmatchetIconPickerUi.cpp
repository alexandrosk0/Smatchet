// Searchable Font-Awesome icon picker modal for the customizable toolbar. The catalog
// data + name->glyph lookup live in the ImGui-free SmatchetIconCatalog.cpp; this TU only
// renders the grid (virtualized with ImGuiListClipper) over that shared catalog.

#include "SmatchetIconPickerUi.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "SmatchetImGuiFonts.h"
#include "TextMatchPure.h"
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

// Every icon for an empty query, in catalog order. Otherwise the shared matcher over the slugs, best first and
// ties in catalog order: the words "arrow up" find both arrow-up and up-arrow slugs, and the letters mgnf find
// magnifying-glass. When any slug holds every word as written, letters-in-order matches are left out.
void SmatchetIconPickerUi::refreshMatches(const std::vector<SmatchetToolbarIconEntry>& catalog) {
    if (matchesValid_ && matchesQuery_ == search_ && matchesCatalogSize_ == catalog.size()) {
        return;
    }
    matchesValid_ = true;
    matchesQuery_ = search_;
    matchesCatalogSize_ = catalog.size();
    matches_.clear();
    matches_.reserve(catalog.size());
    const smatchet::textmatch::Query query = smatchet::textmatch::MakeQuery(search_);
    if (query.Empty()) {
        for (std::size_t i = 0; i < catalog.size(); ++i) {
            matches_.push_back(static_cast<int>(i));
        }
        return;
    }
    struct Scored {
        long long Rank;
        int Index;
    };
    std::vector<Scored> scored;
    smatchet::textmatch::Scratch scratch;
    bool anyExact = false;
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const char* name = catalog[i].Name != nullptr ? catalog[i].Name : "";
        const smatchet::textmatch::Result r =
            smatchet::textmatch::Match(query, name, std::strlen(name), smatchet::textmatch::Mode::Fuzzy, scratch);
        if (r.Matched()) {
            anyExact = anyExact || r.Substring;
            scored.push_back(Scored{smatchet::textmatch::RankKey(r), static_cast<int>(i)});
        }
    }
    std::sort(scored.begin(), scored.end(),
              [](const Scored& a, const Scored& b) { return a.Rank != b.Rank ? a.Rank > b.Rank : a.Index < b.Index; });
    for (const Scored& row : scored) {
        if (!anyExact || smatchet::textmatch::IsExactRank(row.Rank)) {
            matches_.push_back(row.Index);
        }
    }
}

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

        const bool fa = SmatchetAreFaIconsLoaded();
        ImGui::BeginChild("##icongrid", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), true);

        // The matching icons (refreshed only when the query changes), drawn with ImGuiListClipper so
        // only on-screen rows cost time. About 1400 glyphs total — UX Pillar 1, 6.94 ms budget.
        const std::vector<SmatchetToolbarIconEntry>& catalog = SmatchetToolbarIconCatalog();
        refreshMatches(catalog);
        const std::vector<int>& matches = matches_;

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
            (!picked && keys.Esc == smatchet::dialogkeys::EscapeAction::Cancel &&
             matches.size() > 100000u)) { // MUTANT C3
            ImGui::CloseCurrentPopup();
        }
        smatchet::ui::EndDialog(s_iconPickerDialog);
    }
    return picked;
}
