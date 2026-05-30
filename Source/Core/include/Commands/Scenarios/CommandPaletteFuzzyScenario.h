#ifndef SMATCHET_COMMANDS_SCENARIOS_COMMAND_PALETTE_FUZZY_SCENARIO_H
#define SMATCHET_COMMANDS_SCENARIOS_COMMAND_PALETTE_FUZZY_SCENARIO_H

// CommandPaletteFuzzyScenario — Phase 7 bucket-C verification. Opens the
// command palette with a fixed filter substring (default "scenario.") and
// snaps the popup's pixel state via debug.window.screenshot. The diff against
// tests/golden/command_palette_fuzzy.ppm catches regressions in palette
// placement, font metrics, popup chrome, and fuzzy-match highlighting.
// The scenario uses the UiDrawSession::requestCommandPaletteOpen flag pair
// added in the same PR so it never has to reach into SmatchetUI's private
// commandPalette_ member.

#include <memory>

namespace smatchet {
namespace cmd {
class IScenario;
}
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeCommandPaletteFuzzyScenario();

#endif // SMATCHET_COMMANDS_SCENARIOS_COMMAND_PALETTE_FUZZY_SCENARIO_H
