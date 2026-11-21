#ifndef SMATCHET_UI_ICON_PICKER_UI_H
#define SMATCHET_UI_ICON_PICKER_UI_H

// Font-Awesome icon picker for the customizable toolbar. The full Font-Awesome 6
// catalog (generated from the FA header by scripts/dev/gen-fa-catalog.py) backs both
// the picker grid and the name-to-glyph lookup; the grid is virtualized with
// ImGuiListClipper so the ~1400-glyph set stays within the frame budget.

#include <cstddef>
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

/** Modal Font-Awesome icon picker. UI-thread only. Its keys come from the dialog contract
 *  (Ui/SmatchetDialogScope.h): the search box has the caret on open, Enter picks the icon when exactly
 *  one matches, Esc closes without picking. The query is a std::string field (Ui/SmatchetInput.h). */
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

    // Catalog indices that match search_ (the shared matcher, TextMatchPure.h), best first; rebuilt only
    // when the query or the catalog changes, not per frame.
    std::vector<int> matches_;
    std::string matchesQuery_;
    std::size_t matchesCatalogSize_ = 0;
    bool matchesValid_ = false;
    void refreshMatches(const std::vector<SmatchetToolbarIconEntry>& catalog);
};

#endif // SMATCHET_UI_ICON_PICKER_UI_H
