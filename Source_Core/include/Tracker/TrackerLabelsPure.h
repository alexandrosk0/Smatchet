#ifndef SMATCHET_TRACKER_LABELS_PURE_H
#define SMATCHET_TRACKER_LABELS_PURE_H

#include <string>
#include <vector>

/**
 * Pure-logic helpers lifted from TrackerLabelsEditor.cpp's anonymous namespace so they can
 * be unit-tested without ImGui / AppController / CachedTicket. Implementations are
 * byte-identical to the originals (control flow + comparator preserved); the only change
 * is namespace + linkage.
 *
 * No ImGui, no cpr, no SQLite, no AppController. Stdlib + StringUtil only.
 *
 * Owner: TrackerLabelsEditor (UI) — pure helpers live here.
 * Test surface: tests/Source_Core/TrackerLabelsPure.test.cpp.
 */
namespace TrackerLabelsPure {

/**
 * Split a comma-separated string into trimmed, non-empty parts. Whitespace around each
 * part is stripped via TrimCopy (ASCII space / tab / CR / LF). Empty parts (between
 * adjacent commas, trailing comma, leading comma) are dropped. Whitespace-only parts
 * are dropped.
 *
 * Empty / pure-whitespace input -> empty vector. No UB on any string.
 */
std::vector<std::string> ParseCsv(const std::string& csv);

/**
 * Case-insensitive label equality (ASCII fold). Used by the dup-detect path. Folds both
 * sides via ToLowerAsciiCopy then compares. Non-ASCII bytes pass through unchanged.
 */
bool LabelsEqualCaseInsensitive(const std::string& a, const std::string& b);

/**
 * Returns true if `needle` matches any element of `values` under case-insensitive ASCII
 * equality. Empty `values` -> false. Empty `needle` matches any empty element.
 */
bool ContainsLabelCaseInsensitive(const std::vector<std::string>& values, const std::string& needle);

/**
 * Normalise a label list: trim each entry, drop empties, sort case-insensitively (with
 * raw lexicographic tiebreak on case-collision so the kept duplicate is deterministic),
 * and dedupe under case-insensitive equality.
 *
 * Empty input -> empty output. Whitespace-only entries are stripped. Stable contract:
 * dedupe keeps the first equivalent entry after sort; later equivalents drop.
 */
std::vector<std::string> SortAndUniqueLabels(std::vector<std::string> values);

/**
 * Substring filter predicate. `filterLower` is expected to already be lower-cased + trimmed
 * by the caller (the production combo input runs ToLowerAsciiCopy(TrimCopy(searchBuf)) once
 * per frame; this helper is hot-path-cheap). Empty `filterLower` -> always true.
 */
bool LabelMatchesFilter(const std::string& label, const std::string& filterLower);

/**
 * Filter a sorted-unique suggestion list for the combo dropdown: keep entries that either
 * are already selected (so the checkbox stays visible for un-checking) or whose lower-cased
 * label substring-matches `filterLower`.
 *
 * `filterLower` must be pre-lowered + trimmed. Empty `filterLower` -> all entries pass the
 * filter side; selected-side stays true regardless.
 */
std::vector<std::string> FilterSuggestionsForDisplay(const std::vector<std::string>& suggestions,
                                                     const std::vector<std::string>& selectedLabels,
                                                     const std::string& filterLower);

} // namespace TrackerLabelsPure

#endif
