#ifndef SMATCHET_COMMANDS_SCENARIOS_THEME_SWITCH_ROUNDTRIP_SCENARIO_H
#define SMATCHET_COMMANDS_SCENARIOS_THEME_SWITCH_ROUNDTRIP_SCENARIO_H

// ThemeSwitchRoundtripScenario — bucket-C screenshot-diff guard for the
// SmatchetDark <-> NortonCommander <-> SmatchetDark round-trip. The user-facing
// bug was: after switching themes A -> B -> A, residual colours from theme B
// leaked into the restored A view. Three rounds of fixes hardened the static
// ImGuiStyle path (Colors[] reseed, ImGuiStyle{} full reset, theme-aware
// TextEditor::Palette refresh in AiChat + LuaConsole editors); this scenario
// pins the result so a future regression in any of those layers breaks the
// pixel diff against tests/golden/theme_switch_roundtrip.png.
// The scenario drives:
//   * frame 0..warmupA: SmatchetDark warm-up so the initial dock layout settles.
//   * frame warmupA+1: cfg.Theme = NortonCommander (the iconic teal/yellow/blue
//     palette is the most visually distinct target — biggest possible chance
//     for residual leakage on the return path).
//   * frame warmupA+1..warmupA+warmupB: NortonCommander warm-up so every panel
//     re-derives its palette via SmatchetTheme::ApplyStyle.
//   * frame warmupA+warmupB+1: cfg.Theme = SmatchetDark (back).
//   * frame warmupA+warmupB+1..warmupTotal: SmatchetDark warm-up so the per-
//     Draw palette refresh in AiChat + LuaConsole settles.
//   * OnFinish: trigger debug.window.screenshot to write the captured PNG.
// The expected output is BYTE-FOR-BYTE identical to a clean "boot straight
// into SmatchetDark" capture — any colour drift, even a single ImU32 in one
// TextEditor pane, breaks the diff. Tolerance is the standard L∞ ≤ 4 used by
// the rest of bucket-C so AA / sub-pixel rounding can't trip the gate.
// The scenario lives in Source/Core/ so DX12 / Unreal still compiles the TU.
// The standalone screenshot path is renderer-agnostic (GL front-buffer readback
// or DX12 swapchain back-buffer capture), so both standalone renderers satisfy
// the request; CI pins the bucket-C launch to `--renderer=gl` to keep the
// Mesa-rendered goldens byte-stable.

#include <memory>

namespace smatchet {
namespace cmd {
class IScenario;
}
} // namespace smatchet

// Factory entry point declared at namespace scope so AppController.cpp can
// `extern`-declare it inside the existing scenarioRunner_->RegisterFactory
// block. Mirrors MakeDockGapSentinelScenario / MakeCommandPaletteFuzzyScenario.
std::unique_ptr<smatchet::cmd::IScenario> MakeThemeSwitchRoundtripScenario();

#endif // SMATCHET_COMMANDS_SCENARIOS_THEME_SWITCH_ROUNDTRIP_SCENARIO_H
