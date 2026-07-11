// DR27 — the command palette's one-shot "open pre-filtered" latch used to stick:
// ui.open_view set requestCommandPaletteFilter = "view.toggle." and the consume
// site never cleared it, so a later plain Ctrl+Shift+P reopen re-applied the stale
// filter. These pin the pure consume helper that clears the latch on consume so
// each reopen starts unfiltered. The helper is imgui-free (pure ctest rig).
//
// The live-panel counterpart (CommandPaletteUi::Open clearing filtered_) and the
// production wiring of this helper at the consume site (Source/Core/src/Ui/
// SmatchetUI.cpp) are covered by the DR27 code changes; only the pure decision is
// unit-tested here.

#include <doctest/doctest.h>

#include "Commands/PaletteOpenLatch.h"

#include <string>

using smatchet::cmd::ConsumePaletteOpenLatch;
using smatchet::cmd::PaletteOpenDecision;

TEST_CASE("consume applies the filter and clears both latches") {
    bool openReq = true;
    std::string filter = "view.toggle.";

    const PaletteOpenDecision d = ConsumePaletteOpenLatch(openReq, filter);

    CHECK(d.Open == true);
    CHECK(d.Filter == "view.toggle.");
    // Latch drained: strictly one-shot.
    CHECK(openReq == false);
    CHECK(filter.empty());
}

TEST_CASE("a plain reopen after a filtered open is NOT re-filtered (the DR27 bug)") {
    bool openReq = true;
    std::string filter = "view.toggle.";

    // First open: ui.open_view raised open + filter.
    const PaletteOpenDecision first = ConsumePaletteOpenLatch(openReq, filter);
    CHECK(first.Filter == "view.toggle.");

    // User reopens via Ctrl+Shift+P, which raises only the open flag (no filter).
    openReq = true;
    const PaletteOpenDecision second = ConsumePaletteOpenLatch(openReq, filter);

    CHECK(second.Open == true);
    CHECK(second.Filter.empty()); // pre-fix this was the stale "view.toggle."
    CHECK(filter.empty());
    CHECK(openReq == false);
}

TEST_CASE("no open request leaves the decision closed") {
    bool openReq = false;
    std::string filter;

    const PaletteOpenDecision d = ConsumePaletteOpenLatch(openReq, filter);

    CHECK(d.Open == false);
    CHECK(d.Filter.empty());
    CHECK(openReq == false);
}
