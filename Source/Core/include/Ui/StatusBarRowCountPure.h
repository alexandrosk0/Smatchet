#pragma once

#include <cstddef>

// Pure, ImGui-free decision for the status bar's grid row count ("340 rows" /
// "12 of 340 rows"). The impure driver (SmatchetStatusBarUi.cpp) feeds it the focused
// pane's cached sort/filter projection sizes, so the count costs O(1) per frame.

namespace StatusBarRowCountPure {

struct Inputs {
    std::size_t paneCount = 0;     ///< Open grid panes.
    bool hasFocusedPane = false;   ///< A pane is focused (only matters with several panes).
    bool backendReachable = false; ///< False while the grid shows its welcome state.
    bool hasSnapshot = false;      ///< The focused pane has a ticket snapshot to count.
    std::size_t loaded = 0;        ///< Tickets in that snapshot.
    std::size_t sorted = 0;        ///< Size of the pane's sort projection (cachedSortedIndices).
    std::size_t filtered = 0;      ///< Rows the grid draws (filteredIndices).
};

struct Result {
    bool show = false;
    std::size_t visible = 0; ///< == total when nothing narrows the rows.
    std::size_t total = 0;

    bool Filtered() const { return show && visible < total; }
};

inline Result Compute(const Inputs& in) {
    Result r;
    // With several panes and none focused, "the" grid is ambiguous.
    if (in.paneCount == 0 || (in.paneCount > 1 && !in.hasFocusedPane)) {
        return r;
    }
    if (!in.backendReachable || !in.hasSnapshot) {
        return r;
    }
    r.show = true;
    r.total = in.loaded;
    r.visible = in.loaded;
    if (in.sorted == 0 || in.sorted > in.loaded) {
        // No projection yet, or one left over from a larger snapshot: the grid's loading /
        // error states return before the rebuild, so the stale sort list keeps its old size.
        return r;
    }
    // While a debounced rebuild lags a streaming sync, the grid still draws the old
    // projection — report that projection's own pair so the bar matches the grid.
    r.total = in.sorted;
    r.visible = in.filtered < in.sorted ? in.filtered : in.sorted;
    return r;
}

} // namespace StatusBarRowCountPure
