#ifndef SMATCHET_COMMANDS_SCENARIOS_DOCK_GAP_SENTINEL_SCENARIO_H
#define SMATCHET_COMMANDS_SCENARIOS_DOCK_GAP_SENTINEL_SCENARIO_H

// DockGapSentinelScenario — Phase 7 bucket-C verification (test-suite-expansion-
// completion plan). Drives the standalone window to a known steady dock layout
// for a small warm-up window, then triggers `debug.window.screenshot` against
// the user-supplied output path. The bash driver (scripts/dev/test-screenshot-
// diff.sh) compares the captured PPM against tests/golden/dock_gap_sentinel.ppm
// with per-channel L∞ ≤ 4 tolerance — any new dock gap, palette shift, or
// shell-chrome regression breaks the diff. Goldens regenerate on first run if
// absent (bootstrap mode).
// The scenario lives in Source/Core/ so DX12 / Unreal still compiles the TU,
// but the screenshot capture itself is GL-only (Source/Standalone/main.cpp
// owns the glReadPixels path); DX12 leaves the request flag untriggered and
// the scenario reports it in `OnFinish` as `captured=false`.

#include <memory>

namespace smatchet {
namespace cmd {
class IScenario;
}
} // namespace smatchet

// Factory entry point declared at namespace scope so AppController.cpp can
// `extern`-declare it inside the existing scenarioRunner_->RegisterFactory
// block. Mirrors MakeLuaRecorderFuzzScenario / MakePriorityGridScrollScenario.
std::unique_ptr<smatchet::cmd::IScenario> MakeDockGapSentinelScenario();

#endif // SMATCHET_COMMANDS_SCENARIOS_DOCK_GAP_SENTINEL_SCENARIO_H
