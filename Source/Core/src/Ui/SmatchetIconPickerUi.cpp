// Font-Awesome icon picker + curated catalog for the customizable toolbar.

#include "SmatchetIconPickerUi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

#include "IconsFontAwesome6.h"
#include "SmatchetImGuiFonts.h"
#include "imgui.h"

namespace {

// Curated set of common, action-oriented FA6 solid glyphs. Names are the FA slug.
const std::vector<SmatchetToolbarIconEntry>& Catalog() {
    static const std::vector<SmatchetToolbarIconEntry> kCatalog = {
        {"magnifying-glass", ICON_FA_MAGNIFYING_GLASS},
        {"magnifying-glass-plus", ICON_FA_MAGNIFYING_GLASS_PLUS},
        {"plus", ICON_FA_PLUS},
        {"minus", ICON_FA_MINUS},
        {"xmark", ICON_FA_XMARK},
        {"check", ICON_FA_CHECK},
        {"list", ICON_FA_LIST},
        {"list-check", ICON_FA_LIST_CHECK},
        {"gear", ICON_FA_GEAR},
        {"sliders", ICON_FA_SLIDERS},
        {"lock", ICON_FA_LOCK},
        {"lock-open", ICON_FA_LOCK_OPEN},
        {"arrows-rotate", ICON_FA_ARROWS_ROTATE},
        {"rotate", ICON_FA_ROTATE},
        {"trash", ICON_FA_TRASH},
        {"pen", ICON_FA_PEN},
        {"pen-to-square", ICON_FA_PEN_TO_SQUARE},
        {"copy", ICON_FA_COPY},
        {"floppy-disk", ICON_FA_FLOPPY_DISK},
        {"folder", ICON_FA_FOLDER},
        {"folder-open", ICON_FA_FOLDER_OPEN},
        {"file", ICON_FA_FILE},
        {"download", ICON_FA_DOWNLOAD},
        {"upload", ICON_FA_UPLOAD},
        {"star", ICON_FA_STAR},
        {"user", ICON_FA_USER},
        {"users", ICON_FA_USERS},
        {"tag", ICON_FA_TAG},
        {"tags", ICON_FA_TAGS},
        {"filter", ICON_FA_FILTER},
        {"table", ICON_FA_TABLE},
        {"table-columns", ICON_FA_TABLE_COLUMNS},
        {"circle-info", ICON_FA_CIRCLE_INFO},
        {"circle-question", ICON_FA_CIRCLE_QUESTION},
        {"circle-check", ICON_FA_CIRCLE_CHECK},
        {"circle-xmark", ICON_FA_CIRCLE_XMARK},
        {"circle-dot", ICON_FA_CIRCLE_DOT},
        {"triangle-exclamation", ICON_FA_TRIANGLE_EXCLAMATION},
        {"bug", ICON_FA_BUG},
        {"code", ICON_FA_CODE},
        {"code-branch", ICON_FA_CODE_BRANCH},
        {"code-commit", ICON_FA_CODE_COMMIT},
        {"terminal", ICON_FA_TERMINAL},
        {"play", ICON_FA_PLAY},
        {"pause", ICON_FA_PAUSE},
        {"stop", ICON_FA_STOP},
        {"house", ICON_FA_HOUSE},
        {"gauge", ICON_FA_GAUGE},
        {"calendar", ICON_FA_CALENDAR},
        {"clock", ICON_FA_CLOCK},
        {"bell", ICON_FA_BELL},
        {"envelope", ICON_FA_ENVELOPE},
        {"link", ICON_FA_LINK},
        {"arrow-up-right-from-square", ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE},
        {"eye", ICON_FA_EYE},
        {"eye-slash", ICON_FA_EYE_SLASH},
        {"bookmark", ICON_FA_BOOKMARK},
        {"thumbtack", ICON_FA_THUMBTACK},
        {"cube", ICON_FA_CUBE},
        {"cubes", ICON_FA_CUBES},
        {"rocket", ICON_FA_ROCKET},
        {"wrench", ICON_FA_WRENCH},
        {"screwdriver-wrench", ICON_FA_SCREWDRIVER_WRENCH},
    };
    return kCatalog;
}

}  // namespace

const std::vector<SmatchetToolbarIconEntry>& SmatchetToolbarIconCatalog() { return Catalog(); }

std::string SmatchetToolbarIconGlyph(const std::string& name) {
    if (name.empty()) {
        return std::string();
    }
    static std::unordered_map<std::string, std::string> index;
    if (index.empty()) {
        for (const SmatchetToolbarIconEntry& e : Catalog()) {
            index[e.Name] = e.Glyph;
        }
    }
    const std::unordered_map<std::string, std::string>::const_iterator it = index.find(name);
    return it == index.end() ? std::string() : it->second;
}

void SmatchetIconPickerUi::Open() { requestOpen_ = true; }

bool SmatchetIconPickerUi::Draw(std::string& outName, std::string& outGlyph) {
    if (requestOpen_) {
        requestOpen_ = false;
        search_[0] = '\0';
        ImGui::OpenPopup("Pick Icon##SmatchetIconPicker");
    }

    bool picked = false;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 320.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Pick Icon##SmatchetIconPicker", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##iconsearch", "Search icons...", search_, sizeof(search_));

        std::string q(search_);
        std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const bool fa = SmatchetAreFaIconsLoaded();
        ImGui::BeginChild("##icongrid", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), true);
        const float cell = ImGui::GetFrameHeight() + 10.0f;
        const int perRow = (std::max)(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
        int col = 0;
        for (const SmatchetToolbarIconEntry& e : Catalog()) {
            if (!q.empty() && std::string(e.Name).find(q) == std::string::npos) {
                continue;
            }
            if (col != 0) {
                ImGui::SameLine();
            }
            ImGui::PushID(e.Name);
            const char* label = (fa && e.Glyph[0] != '\0') ? e.Glyph : "?";
            if (ImGui::Button(label, ImVec2(cell, cell))) {
                outName = e.Name;
                outGlyph = e.Glyph;
                picked = true;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", e.Name);
            }
            ImGui::PopID();
            col = (col + 1) % perRow;
        }
        ImGui::EndChild();

        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return picked;
}
