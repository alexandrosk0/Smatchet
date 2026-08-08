// Pure transition core for the per-window expand toggle. Deliberately ImGui-free
// so bucket-A (tests/Core/WindowExpand.test.cpp) links it directly.

#include "SmatchetWindowExpand.h"

#include <algorithm>

namespace SmatchetWindowExpand {
namespace detail {

namespace {

void ArmRestore(WindowExpandState& s, unsigned int id) {
    if (id == 0) {
        return;
    }
    if (std::find(s.RestorePending.begin(), s.RestorePending.end(), id) == s.RestorePending.end()) {
        s.RestorePending.push_back(id);
    }
}

} // namespace

void ApplyToggle(WindowExpandState& s, unsigned int id, const WindowExpandSaved& current) {
    if (id == 0) {
        return;
    }
    if (s.ExpandedId == id) {
        s.ExpandedId = 0;
        s.FocusOnExpand = false;
        ArmRestore(s, id);
        return;
    }
    // Expanding a second window minimizes the first — its saved slot stays keyed by
    // its own id, so both windows land back where they started.
    ArmRestore(s, s.ExpandedId);
    // Re-expanding before an armed restore was replayed (two toggles in one frame, e.g.
    // from the command surface): drop the stale latch, or BeginWindow would spend the
    // next frame restoring the window it was just told to expand.
    const std::vector<unsigned int>::iterator stale = std::find(s.RestorePending.begin(), s.RestorePending.end(), id);
    const bool hadStaleRestore = (stale != s.RestorePending.end());
    if (hadStaleRestore) {
        s.RestorePending.erase(stale);
    }
    // With a restore still armed, the window is STILL wearing its fullscreen
    // geometry — `current` is the override, not the home it came from. Keeping the
    // older entry is what stops a same-frame minimize+expand from recording the
    // work area as this window's home and turning the next minimize into a no-op.
    if (!hadStaleRestore || s.Saved.find(id) == s.Saved.end()) {
        s.Saved[id] = current;
    }
    s.ExpandedId = id;
    s.FocusOnExpand = true;
}

bool ConsumeRestore(WindowExpandState& s, unsigned int id, WindowExpandSaved& out) {
    const std::vector<unsigned int>::iterator pending = std::find(s.RestorePending.begin(), s.RestorePending.end(), id);
    if (pending == s.RestorePending.end()) {
        return false;
    }
    s.RestorePending.erase(pending);
    const std::unordered_map<unsigned int, WindowExpandSaved>::iterator saved = s.Saved.find(id);
    if (saved == s.Saved.end()) {
        // Latch armed without a stored slot (self-heal on a window that never
        // finished expanding) — nothing to replay, but the latch is still consumed.
        return false;
    }
    out = saved->second;
    s.Saved.erase(saved);
    return true;
}

void SelfHeal(WindowExpandState& s) {
    if (s.ExpandedId != 0 &&
        std::find(s.SubmittedIds.begin(), s.SubmittedIds.end(), s.ExpandedId) == s.SubmittedIds.end()) {
        ArmRestore(s, s.ExpandedId);
        s.ExpandedId = 0;
        s.FocusOnExpand = false;
    }
    s.SubmittedIds.clear();
}

} // namespace detail
} // namespace SmatchetWindowExpand
