#pragma once

// Pure (ImGui-free) predicates behind the ticket-grid column-order repair + writeback in
// SmatchetActiveProjectGridTable.cpp. An ImGui table keeps two views of the same permutation
// -- each column's DisplayOrder (index -> visual slot) and the table's DisplayOrderToIndex
// (visual slot -> index) -- which must stay mutual inverses. The grid can observe them out of
// sync, so the decisions live here where they are testable without a live ImGui context; the
// grid TU supplies the two accessors over ImGui's own storage.

#include <algorithm>
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

/// Visual slot a dragged header belongs in for the mouse at @p mouseX: the number of OTHER
/// columns whose centre lies left of it. @p centersByDisplayOrder[o] is the horizontal centre of
/// the column at visual slot o (same space as @p mouseX); @p heldOrder is the dragged column's
/// current slot. Measuring against the other columns' centres rather than their edges gives the
/// move a hysteresis of the dragged column's own width: once it passes a column, that column's
/// centre shifts by exactly that width and stays on the same side of the mouse, so a narrow
/// column dragged over a wide one cannot flip back and forth. Returns @p heldOrder when the
/// input is out of range.
inline int GridHeaderDragTargetOrder(const std::vector<float>& centersByDisplayOrder, int heldOrder, float mouseX) {
    const int count = static_cast<int>(centersByDisplayOrder.size());
    if (heldOrder < 0 || heldOrder >= count) {
        return heldOrder;
    }
    int before = 0;
    for (int o = 0; o < count; ++o) {
        if (o != heldOrder && centersByDisplayOrder[static_cast<std::size_t>(o)] < mouseX) {
            ++before;
        }
    }
    return before;
}

/// Signed horizontal auto-scroll speed (px/second; negative scrolls left) while a header is
/// dragged near or past either edge of the table's scrollable strip [@p stripMinX, @p stripMaxX].
/// Zero away from the edges; inside an edge band @p edgeZonePx wide it ramps up linearly to
/// @p speedAtEdgePxPerSec at the edge itself, and keeps ramping past the edge up to four times
/// that, so pulling the mouse further out scrolls faster. The band shrinks to a third of the
/// strip when the strip is too narrow to hold two full bands. Scrolls only toward the side the
/// drag has moved to from @p dragStartX (where the header was grabbed): grabbing a column that
/// already sits in an edge band and nudging it the other way must not scroll the grid away.
inline float GridHeaderDragAutoScrollSpeed(float mouseX, float dragStartX, float stripMinX, float stripMaxX,
                                           float edgeZonePx, float speedAtEdgePxPerSec) {
    const float stripWidth = stripMaxX - stripMinX;
    if (stripWidth <= 0.0f || edgeZonePx <= 0.0f || speedAtEdgePxPerSec <= 0.0f) {
        return 0.0f;
    }
    const float zone = std::min(edgeZonePx, stripWidth / 3.0f);
    const float kMaxMultiple = 4.0f;
    const float leftDepth = (stripMinX + zone) - mouseX;
    if (leftDepth > 0.0f && mouseX < dragStartX) {
        return -speedAtEdgePxPerSec * std::min(leftDepth / zone, kMaxMultiple);
    }
    const float rightDepth = mouseX - (stripMaxX - zone);
    if (rightDepth > 0.0f && mouseX > dragStartX) {
        return speedAtEdgePxPerSec * std::min(rightDepth / zone, kMaxMultiple);
    }
    return 0.0f;
}
