#pragma once

// Pure (ImGui-free) predicates behind the ticket-grid column-order repair + writeback in
// SmatchetActiveProjectGridTable.cpp. An ImGui table keeps two views of the same permutation
// -- each column's DisplayOrder (index -> visual slot) and the table's DisplayOrderToIndex
// (visual slot -> index) -- which must stay mutual inverses. The grid can observe them out of
// sync, so the decisions live here where they are testable without a live ImGui context; the
// grid TU supplies the two accessors over ImGui's own storage.

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

/// Index of the first column whose two order views disagree, or -1 when every column
/// round-trips. @p displayOrderOf(n) returns column n's display order; @p indexAtOrder(o)
/// returns the logical column the map places at visual slot o. Consistency means
/// indexAtOrder(displayOrderOf(n)) == n for every n; a display order outside
/// [0, columnCount) is itself a mismatch. A zero-column table is trivially consistent.
template <typename OrderFn, typename IndexFn>
int GridFirstDisplayOrderMismatch(int columnCount, OrderFn displayOrderOf, IndexFn indexAtOrder) {
    for (int n = 0; n < columnCount; ++n) {
        const int order = displayOrderOf(n);
        if (order < 0 || order >= columnCount) {
            return n;
        }
        if (indexAtOrder(order) != n) {
            return n;
        }
    }
    return -1;
}

/// True when @p visualOrder holds the keys of a @p columnCount-column set exactly once each.
/// Callers build it by indexing the column vector through the display-order map, so every
/// entry is a real column key by construction -- right length plus no repeat is therefore
/// enough to prove a permutation. A short or repeating order means the map was inconsistent
/// when it was read, and persisting it would bake a duplicate key into the saved column order.
inline bool GridVisualColumnOrderIsPermutation(const std::vector<std::string>& visualOrder, std::size_t columnCount) {
    if (visualOrder.size() != columnCount) {
        return false;
    }
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < visualOrder.size(); ++i) {
        if (!seen.insert(visualOrder[i]).second) {
            return false;
        }
    }
    return true;
}
