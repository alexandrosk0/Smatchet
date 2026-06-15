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
// The scenario lives in Source/Core/ so DX12 / Unreal still compiles the TU.
// The standalone screenshot path is renderer-agnostic: it captures from the GL
// front buffer (flipped) or the DX12 swapchain back buffer (already top-left),
// so both standalone renderers satisfy the request. CI pins the bucket-C launch
// to `--renderer=gl` to keep the Mesa-rendered goldens byte-stable.

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
