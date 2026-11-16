#ifndef SMATCHET_UI_ICON_PICKER_UI_H
#define SMATCHET_UI_ICON_PICKER_UI_H

// Font-Awesome icon picker for the customizable toolbar. The full Font-Awesome 6
// catalog (generated from the FA header by scripts/dev/gen-fa-catalog.py) backs both
// the picker grid and the name-to-glyph lookup; the grid is virtualized with
// ImGuiListClipper so the ~1400-glyph set stays within the frame budget.

#include <string>
#include <vector>

struct SmatchetToolbarIconEntry {
    const char* Name;  // FA slug, e.g. "magnifying-glass"
    const char* Glyph; // UTF-8 glyph (ICON_FA_* macro value)
};

/** Curated FA icon catalog (stable order). */
const std::vector<SmatchetToolbarIconEntry>& SmatchetToolbarIconCatalog();

/** Resolve a catalog name to its glyph; returns "" when unknown. */
std::string SmatchetToolbarIconGlyph(const std::string& name);

/** Modal Font-Awesome icon picker. UI-thread only. The search box has the caret when the picker opens;
 *  Enter picks the first match (nothing on an empty list) and Esc in the box closes the picker without
 *  picking (docs/audits/INPUT_UX_AUDIT.md § 3, picker row). */
class SmatchetIconPickerUi {
  public:
    /** Request the picker to open on the next Draw(). */
    void Open();

    // Render the modal if open. When the user commits a pick, writes outName + outGlyph
    // and returns true exactly once; otherwise returns false. Must be called every frame
    // from inside the owning window/popup so the modal can render.
    bool Draw(std::string& outName, std::string& outGlyph);

  private:
    bool requestOpen_ = false;
    std::string search_; // std::string: a long query is never clipped
};

#endif // SMATCHET_UI_ICON_PICKER_UI_H
