// ThemeSwitchRoundtripScenario — bucket-C screenshot-diff guard for the
// SmatchetDark <-> NortonCommander <-> SmatchetDark round-trip. The user-facing
// bug was: after switching themes A -> B -> A, residual colours from theme B
// leaked into the restored A view. Three rounds of fixes hardened the static
// ImGuiStyle path (Colors[] reseed, ImGuiStyle{} full reset, theme-aware
// TextEditor::Palette refresh in AiChat + LuaConsole editors); this scenario
// pins the result so a future regression in any of those layers breaks the
// pixel diff. The expected output is BYTE-FOR-BYTE identical to a clean "boot
// straight into SmatchetDark" capture — any colour drift, even a single ImU32
// in one TextEditor pane, breaks the diff. Tolerance is the standard L∞ ≤ 4
// used by the rest of bucket-C so AA / sub-pixel rounding can't trip the gate.
// The scenario lives in Source/Core/ so DX12 / Unreal still compiles the TU.
// The screenshot path is renderer-agnostic (GL front-buffer readback or DX12
// swapchain back-buffer capture); CI pins the bucket-C launch to
// `--renderer=gl` to keep the Mesa-rendered goldens byte-stable.
// The factory (`MakeThemeSwitchRoundtripScenario`) is `extern`-declared by
// SmatchetScenarioRegistry.cpp — there is deliberately no header.
// Drives a
// SmatchetDark -> NortonCommander -> SmatchetDark theme round-trip by mutating
// g_ui.cfg.Theme on the schedule documented below, then triggers
// `debug.window.screenshot` so the bash driver can pixel-diff the captured PNG
// against tests/golden/theme_switch_roundtrip.png. Any residual colour leakage
// from NortonCommander into the restored SmatchetDark view breaks the diff.

#include "Commands/Scenarios/IScenario.h"

#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/Scenarios/ScenarioScreenshotPath.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetThemeIds.h"
#include "SmatchetUiSession.h"

#include <string>

// Same singleton extern shim as DockGapSentinelScenario.cpp + BuiltinCommands_
// Debug.cpp — the SmatchetUiSession.h-side extern is gated on
// SMATCHET_WITH_LUA_AUTOMATION while g_ui itself is defined unconditionally
// in SmatchetUI.cpp.
// File-top scaffold clone across sibling scenarios — see the matching marker in
// DockGapSentinelScenario.cpp; the extractable parts now live in ScenarioArgs.h.
// SMATCHET_DEVIATION(rule=duplication; reason=file-top scaffold clone; owner=command-system; revisit=2026-12-31)
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

// IntArg/BoolArg/StringArg come from the shared Commands/Scenarios/ScenarioArgs.h
// (via ScenarioScreenshotPath.h) — the per-scenario anon-namespace copies are gone.

class ThemeSwitchRoundtripScenario : public IScenario {
  public:
    std::string Name() const override { return "theme-switch-roundtrip"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        // Warm-up budgets. Three knobs to keep the scenario tunable from CLI
        // without requiring a recompile when ImGui's docking layout cost
        // changes between minor versions:
        //   * warmupA — frames the initial SmatchetDark needs to settle the
        //     dock layout from a cold-spawn state. 6 frames is the dock-gap
        //     scenario's tuned floor; we keep parity.
        //   * warmupB — frames NortonCommander needs to fully repaint every
        //     panel after the cfg.Theme switch. Two frames are enough for
        //     ImGui's frame-coherent re-render, but we keep an extra cushion
        //     so the per-Draw TextEditor::SetPalette path has time to land.
        //   * warmupReturn — frames the restored SmatchetDark needs to settle.
        //     Symmetric with warmupA — the leak this scenario guards against
        //     would manifest here.
        warmupA_ = IntArg(args, "warmupA", 6);
        warmupB_ = IntArg(args, "warmupB", 6);
        warmupReturn_ = IntArg(args, "warmupReturn", 6);
        // skipSwitch=true gates BOTH cfg.Theme flips off so the scenario degrades
        // into a pure "warm up SmatchetDark for warmupA + warmupB + warmupReturn
        // frames then screenshot" path. The test driver in test-theme-roundtrip.sh
        // uses this to capture the fresh-launch baseline through the EXACT same
        // scenario shape as the round-trip — same total frame count, same warmup
        // timing, same OnFinish capture path — so any transient sync-banner /
        // toast / animation state is matched between the two captures. The diff
        // then isolates strictly the theme-switch state delta. Without this knob
        // the dual-capture compare ends up dominated by non-theme transient
        // state (sync.tracker_status banners, fire-and-forget toasts) that
        // appears in one capture and not the other purely because the
        // round-trip's extra warmup frames give async work more wall-clock to
        // complete.
        skipSwitch_ = BoolArg(args, "skipSwitch", false);
        if (warmupA_ < 1)
            warmupA_ = 1;
        if (warmupB_ < 1)
            warmupB_ = 1;
        if (warmupReturn_ < 1)
            warmupReturn_ = 1;

        screenshotPath_ = StringArg(args, "screenshotPath", std::string());
        if (screenshotPath_.empty()) {
            outErr = "theme-switch-roundtrip: screenshotPath is required";
            return;
        }
        // Confine the caller-supplied path under <userData>/screenshots/ — MCP/Lua-reachable
        // scenario, so an unconfined path is an arbitrary-file-write primitive (#1566 class).
        // Confine before mutating any session/theme state below so a reject returns cleanly.
        if (!ConfineScenarioScreenshotPathInPlace("theme-switch-roundtrip", screenshotPath_, outErr))
            return;

        // Capture the user's theme so OnFinish/OnCancel can restore it. This
        // scenario deliberately mutates cfg.Theme in-memory; the ephemeral
        // spawn flow never persists cfg, but on a long-lived dev session
        // (re-using one Smatchet instance for repeated runs) we must leave
        // the user's pick untouched.
        savedTheme_ = g_ui.cfg.Theme;

        // Force the scenario's known starting theme so the diff is byte-stable
        // even when the user-saved cfg.Theme differs. SmatchetUI::Draw's
        // `cfg.Theme != lastAppliedTheme_` check fires on the next frame and
        // runs SmatchetTheme::ApplyStyle(SmatchetDark).
        g_ui.cfg.Theme = ThemeId::SmatchetDark;
    }

    void OnFrame(IAppScenarioHost& /*app*/, int frameIndex) override {
        // skipSwitch_ short-circuits both theme flips. The scenario then walks
        // through the full warmup window holding SmatchetDark the entire time
        // — what the dual-capture test driver uses as its fresh-launch
        // baseline (matched warmup timing => matched transient sync state).
        if (skipSwitch_)
            return;
        // Stage 1: SmatchetDark warm-up. cfg.Theme was set in OnStart; nothing
        // to do here until the warm-up budget elapses.
        if (frameIndex == warmupA_) {
            // Stage transition A -> B: flip to NortonCommander. The flip is
            // visible to SmatchetUI::Draw next frame, which will then call
            // SmatchetTheme::ApplyStyle(NortonCommander).
            g_ui.cfg.Theme = ThemeId::NortonCommander;
        } else if (frameIndex == warmupA_ + warmupB_) {
            // Stage transition B -> A: flip back to SmatchetDark. Same dispatch
            // path — the next SmatchetUI::Draw frame applies SmatchetDark.
            g_ui.cfg.Theme = ThemeId::SmatchetDark;
        }
    }

    bool IsDone(int frameIndex) const override {
        // Total frame budget = sum of all three warm-up windows + a small
        // cushion so the final SmatchetTheme::ApplyStyle has fully drawn one
        // post-transition frame before we snap the screenshot.
        return frameIndex >= (warmupA_ + warmupB_ + warmupReturn_);
    }

    void OnCancel(IAppScenarioHost& /*app*/) override {
        // Restore the user's theme on cancellation so a Ctrl-C mid-run doesn't
        // leave the live UI on NortonCommander.
        g_ui.cfg.Theme = savedTheme_;
    }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        // Same capture-trigger pattern as DockGapSentinelScenario /
        // CommandPaletteFuzzyScenario — the post-swap handler in
        // Source/Standalone/main.cpp will write the PNG after the next
        // SwapBuffers.
        // DELIBERATELY DO NOT restore savedTheme_ here. The screenshot fires
        // in the post-swap handler one frame AFTER OnFinish returns. If we
        // flip cfg.Theme back to savedTheme_ now, SmatchetUI::Draw's
        // `cfg.Theme != lastAppliedTheme_` check fires on the NEXT frame and
        // SmatchetTheme::ApplyStyle(savedTheme_) runs BEFORE the screenshot
        // capture — so the captured PNG would be of the restored theme, not
        // the post-roundtrip SmatchetDark we just spent the scenario landing
        // on. The diff against the golden would then be non-deterministic
        // (depends on whatever cfg.Theme the spawn launched with).
        // On ephemeral spawn instances this is a non-issue — the process
        // exits immediately after the screenshot fires + cfg is never
        // persisted. On a long-running dev session re-using one Smatchet
        // process for repeated scenario.run calls, the user keeps the
        // SmatchetDark theme the scenario landed on and can switch back via
        // Preferences. The deterministic capture is far more valuable than
        // the auto-restore convenience.
        g_ui.requestScreenshotPath = screenshotPath_;
        g_ui.requestScreenshot = true;

        nlohmann::json out;
        out["scenario"] = Name();
        out["warmupA"] = warmupA_;
        out["warmupB"] = warmupB_;
        out["warmupReturn"] = warmupReturn_;
        out["skipSwitch"] = skipSwitch_;
        out["screenshotPath"] = screenshotPath_;
        out["captureRequested"] = true;
        return out;
    }

  private:
    int warmupA_ = 6;
    int warmupB_ = 6;
    int warmupReturn_ = 6;
    bool skipSwitch_ = false;
    std::string screenshotPath_;
    ThemeId savedTheme_ = ThemeId::SmatchetDark;
};

} // namespace

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeThemeSwitchRoundtripScenario() {
    return std::make_unique<smatchet::cmd::ThemeSwitchRoundtripScenario>();
}
