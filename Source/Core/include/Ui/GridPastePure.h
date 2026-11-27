#pragma once

// GridPastePure — what one pasted cell writes (docs/plans/shipped/input-ux-w3-grid-spreadsheet.md). PlanPaste
// (Ui/GridCursorPure.h) says which cell each pasted text lands on; ResolveCellText says what that text means for the
// cell's field, or that it means nothing there (the cell is then skipped and counted, never written as raw text).
// No ImGui, cpr or SQLite includes.

#include "Tracker/TrackerFieldSchema.h"
#include "Tracker/TrackerFieldValueParseExplain.h" // CivilDate

#include <string>
#include <vector>

namespace smatchet {
namespace grid {
namespace paste {

/// One pasted cell for one grid cell.
struct PasteValueInput {
    std::string Text;    ///< the pasted text
    std::string Exact;   ///< the stored date behind Text, when this app copied it (CopyRecord); empty otherwise
    std::string Current; ///< the cell's stored value: a date-only paste keeps a date-time's time of day
};

/// The last grid copy. A date column is copied in its display form ("-2w", "Jan 5 '26"), which does not say the stored
/// date, so a paste of this very text reads the dates from here instead.
struct CopyRecord {
    std::string Tsv;                             ///< the text put on the clipboard
    std::vector<std::vector<std::string>> Exact; ///< per copied row and cell: the stored date; empty for other columns
};

/// True when `clipboard` is still the text of `copy` (carriage returns ignored).
bool IsOwnCopy(const CopyRecord& copy, const std::string& clipboard);

/// The stored date of the copied cell at (row, col), counted from the copy's first cell; empty when there is none.
std::string ExactAt(const CopyRecord& copy, int row, int col);

/// A free value: text, number or duration with no option list, so the displayed text is what its editor writes.
bool IsFreeValueField(const TrackerField& field);

/// Reads `in.Text` for `field` into the values its editor would queue. False (and `out` empty) when the field takes no
/// paste (read-only, rich text, a lazily filled or cascading option list) or the text does not read as its kind: an
/// option that is not in the list, a date, duration or number that does not parse, an empty status. An empty text
/// clears.
bool ResolveCellText(const TrackerField& field, const PasteValueInput& in, const TrackerParseExplain::CivilDate& today,
                     std::vector<std::string>& out);

} // namespace paste
} // namespace grid
} // namespace smatchet
