#pragma once

/// Pure index arithmetic behind `view.next` / `view.previous`: step through the saved views in the
/// order the view list shows them. No view types here, so the rule is unit-tested on its own.
namespace smatchet {
namespace views {

/// The index `step` places away from `activeIndex` in a list of `count` views, wrapping at both
/// ends. An active index outside the list (no active view, or one that is gone) lands on the first
/// view for a forward step and the last for a backward one. Returns -1 when there is no view to land
/// on (`count` <= 0) or `step` is 0.
inline int CycleViewIndex(int count, int activeIndex, int step) {
    if (count <= 0 || step == 0) {
        return -1;
    }
    if (activeIndex < 0 || activeIndex >= count) {
        return step > 0 ? 0 : count - 1;
    }
    // Reduce the step first so the sum cannot overflow, then wrap into [0, count).
    const int reduced = step % count;
    const int moved = (activeIndex + reduced) % count;
    return moved < 0 ? moved + count : moved;
}

} // namespace views
} // namespace smatchet
