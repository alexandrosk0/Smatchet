// DockGapSentinelScenario — see header for rationale. The scenario itself
// does very little — it lets the dock settle for a small warm-up window
// (default 8 frames), then sets the existing debug.window.screenshot request
// flags on UiDrawSession so Source/Standalone/main.cpp's post-swap handler
// writes the PPM at the user-supplied output path. The bash driver compares
// the PPM against tests/golden/dock_gap_sentinel.ppm.
// Why a scenario and not a one-shot CLI: capturing the screenshot requires
// the renderer to have already drawn the steady-state dock chrome. A bare
// `cmd debug.window.screenshot` against a freshly-spawned exe races the
// dock-builder which lays out asynchronously over the first 2-3 frames. A
// few warm-up frames inside the scenario eliminate that flake without
// putting the warm-up policy in the bash script.

// Scenario file-top scaffold — the shared include block + g_ui extern shim +
// anon-namespace IntArg/StringArg arg parsers that every screenshot scenario carries.
// dup_audit matches it against the sibling screenshot scenarios as a copy-paste run;
// it is idiomatic per-scenario boilerplate, not shared logic to extract.
// SMATCHET_DEVIATION(rule=duplication; reason=file-top scaffold clone; owner=command-system; revisit=2026-12-31)
#include "Commands/Scenarios/IScenario.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/Scenarios/ScenarioCaptureSizing.h"
#include "Commands/Scenarios/ScenarioScreenshotPath.h"
#include "Logger.h"
#include "SmatchetUiSession.h"

#include <string>

// g_ui — unconditional extern. The symbol is defined in SmatchetUI.cpp without
// any SMATCHET_WITH_LUA_AUTOMATION guard; the header-side `extern` in
// SmatchetUiSession.h is gated, so scenarios that need the singleton must
// re-declare it here (same shim BuiltinCommands_Debug.cpp uses).
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

// CLI args land as JSON strings (Source/Standalone/CliCommandRunner.cpp § ParseArgs
// stores --key=value as `string`). Scenarios must coerce defensively or risk
// nlohmann::json::type_error.302 when args.value<int>(...) hits a string.
int IntArg(const nlohmann::json& args, const char* key, int fallback) {
    if (!args.contains(key))
        return fallback;
    const auto& v = args[key];
    if (v.is_number())
        return v.get<int>();
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}
std::string StringArg(const nlohmann::json& args, const char* key, const std::string& fallback) {
    if (!args.contains(key))
        return fallback;
    const auto& v = args[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number())
        return std::to_string(v.get<long long>());
    return fallback;
}

// Scenario OnStart-prologue scaffold — the warmupFrames/captureSize/screenshotPath
// parse + required-empty check plus the identical anon-namespace StringArg tail,
// shared across screenshot scenarios. dup_audit matches it as a copy-paste run; the
// shared confine chokepoint (ConfineScenarioScreenshotPathInPlace) already de-dups the
// confine logic — a shared prologue helper for the parse tail is the remaining de-dup,
// tracked in the duplication backlog.
// SMATCHET_DEVIATION(rule=duplication; reason=prologue scaffold clone; owner=command-system; revisit=2026-12-31)
class DockGapSentinelScenario : public IScenario {
  public:
    std::string Name() const override { return "dock-gap-sentinel"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        warmupFrames_ = IntArg(args, "warmupFrames", 8);
        if (warmupFrames_ < 1) {
            warmupFrames_ = 1;
        }
        captureSize_ = ParseScenarioCaptureSize(args);
        screenshotPath_ = StringArg(args, "screenshotPath", std::string());
        if (screenshotPath_.empty()) {
            outErr = "dock-gap-sentinel: screenshotPath is required";
            return;
        }
        // Confine the caller-supplied path under <userData>/screenshots/ — this scenario is
        // MCP/Lua-reachable, so an unconfined path is an arbitrary-file-write primitive (the
        // #1566 class). Absolute/'..'/escaping paths are rejected; --spawn callers are fulfilled
        // by the trusted parent (CliCommandRunner) which swaps in a confine-safe basename.
        if (!ConfineScenarioScreenshotPathInPlace("dock-gap-sentinel", screenshotPath_, outErr))
            return;

        g_ui.requestWindowWidth = captureSize_.Width;
        g_ui.requestWindowHeight = captureSize_.Height;
        g_ui.requestWindowResize = true;

        // Arm the pink (magenta) clear-color override on the first frame too.
        ArmPinkClear();
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {
        // The dock is already in its steady-state layout by the time
        // SmatchetUI::Draw runs; warm-up frames give the docking builder time to
        // settle from a cold-spawn state. Re-arm the transient pink clear-color
        // EVERY warm-up frame: Source/Standalone/main.cpp consumes (clears) the
        // override each frame, so re-arming holds the diagnostic clear active right
        // through to the captured frame. Any viewport pixel the dock leaves
        // uncovered then renders magenta (255,0,255) instead of the theme bg.
        ArmPinkClear();
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= warmupFrames_; }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        // Trigger the screenshot AFTER the warm-up frames have rendered.
        // Source/Standalone/main.cpp's post-swap handler will consume the
        // request and write the PPM. On DX12 the request flag has no
        // consumer; we still report what we asked for so the bash driver
        // can detect the platform gap.
        // Keep the pink clear-color armed on the captured frame too: OnFinish runs
        // in the SAME runner tick as the final OnFrame, but arm here as well so the
        // captured frame is pink even if the warm-up window is a single frame. Any
        // magenta in the capture = a real dock gap (the bash driver asserts the
        // pink-pixel count is zero).
        ArmPinkClear();

        g_ui.requestScreenshotPath = screenshotPath_;
        g_ui.requestScreenshot = true;

        nlohmann::json out;
        out["scenario"] = Name();
        out["warmupFrames"] = warmupFrames_;
        out["windowWidth"] = captureSize_.Width;
        out["windowHeight"] = captureSize_.Height;
        out["screenshotPath"] = screenshotPath_;
        out["captureRequested"] = true;
        // Echo the sentinel color so the bash driver knows which RGB to count.
        out["pinkClear"] = true;
        out["sentinelR"] = 255;
        out["sentinelG"] = 0;
        out["sentinelB"] = 255;
        return out;
    }

  private:
    // Arm the transient magenta clear-color override for the current frame. The
    // standalone consumer (Source/Standalone/main.cpp) reads + clears it each frame,
    // so this is re-called every warm-up frame to hold the diagnostic clear active.
    static void ArmPinkClear() {
        g_ui.requestClearColorActive = true;
        g_ui.requestClearColorR = 1.0f; // 255
        g_ui.requestClearColorG = 0.0f; // 0
        g_ui.requestClearColorB = 1.0f; // 255
        g_ui.requestClearColorA = 1.0f;
    }

    int warmupFrames_ = 8;
    ScenarioCaptureSize captureSize_;
    std::string screenshotPath_;
};

} // namespace

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeDockGapSentinelScenario() {
    return std::make_unique<smatchet::cmd::DockGapSentinelScenario>();
}
