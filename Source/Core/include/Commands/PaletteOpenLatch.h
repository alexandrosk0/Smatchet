#ifndef SMATCHET_COMMANDS_PALETTE_OPEN_LATCH_H
#define SMATCHET_COMMANDS_PALETTE_OPEN_LATCH_H

// Pure decision helper for the one-shot "open the command palette (optionally
// pre-filtered)" request that ui.open_view / the fuzzy scenario raise on the
// shared UiDrawSession (requestCommandPaletteOpen + requestCommandPaletteFilter).
// Kept free of imgui / UI includes so it is unit-testable on the pure ctest rig.
//
// DR27: the palette filter used to STICK for the whole session. The consume site
// (SmatchetUI.cpp) read requestCommandPaletteFilter but never cleared it, so a
// later plain Ctrl+Shift+P reopen re-applied the stale "view.toggle." filter.
// Consuming the latch through this helper clears BOTH inputs, making the request
// strictly one-shot so every reopen starts from a clean slate.

#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

struct PaletteOpenDecision {
    bool Open = false;  // open the palette this frame
    std::string Filter; // filter text to apply on open (empty = no filter)
};

// Consume the open + filter latches. When an open was requested, returns the
// filter to apply and CLEARS both inputs. When no open was requested the inputs
// are left untouched (ui.open_view always sets the filter together with the open
// flag, so the filter is only ever read in the same frame the open is consumed).
inline PaletteOpenDecision ConsumePaletteOpenLatch(bool& openRequested, std::string& filterLatch) {
    PaletteOpenDecision decision;
    if (openRequested) {
        decision.Open = true;
        decision.Filter = std::move(filterLatch);
        openRequested = false;
        filterLatch.clear();
    }
    return decision;
}

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_PALETTE_OPEN_LATCH_H
