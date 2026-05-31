#ifndef SMATCHET_UI_ICON_PICKER_UI_H
#define SMATCHET_UI_ICON_PICKER_UI_H

// Font-Awesome icon picker for the customizable toolbar. A curated catalog of common
// FA6 glyphs (name + glyph) backs both the picker grid and name->glyph resolution at
// render time. The full ~2500-glyph catalog + ImGuiListClipper grid is a deferred
// enhancement (docs/plans/active/customizable-icon-toolbar.md).

#include <string>
#include <vector>

struct SmatchetToolbarIconEntry {
    const char* Name;   // FA slug, e.g. "magnifying-glass"
    const char* Glyph;  // UTF-8 glyph (ICON_FA_* macro value)
};

/** Curated FA icon catalog (stable order). */
const std::vector<SmatchetToolbarIconEntry>& SmatchetToolbarIconCatalog();

/** Resolve a catalog name to its glyph; returns "" when unknown. */
std::string SmatchetToolbarIconGlyph(const std::string& name);

/** Modal Font-Awesome icon picker. UI-thread only. */
class SmatchetIconPickerUi {
  public:
    /** Request the picker to open on the next Draw(). */
    void Open();

    /**
     * Render the modal if open. When the user commits a pick, writes outName + outGlyph
     * and returns true exactly once; otherwise returns false. Must be called every frame
     * from inside the owning window/popup so the modal can render.
     */
    bool Draw(std::string& outName, std::string& outGlyph);

  private:
    bool requestOpen_ = false;
    char search_[64] = {0};
};

#endif  // SMATCHET_UI_ICON_PICKER_UI_H
