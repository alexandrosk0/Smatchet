#ifndef SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_SIZING_H
#define SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_SIZING_H

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace smatchet {
namespace cmd {

struct ScenarioCaptureSize {
    int Width = 1920;
    int Height = 1009;
};

ScenarioCaptureSize ParseScenarioCaptureSize(const nlohmann::json& args);

/// Outcome of comparing the size a screenshot scenario ASKED for against the framebuffer
/// it actually captured. `Ok` false means the capture is unusable as a golden-diff input.
struct ScenarioCaptureSizeVerdict {
    bool Ok = true;
    /// Empty when Ok; otherwise a stable, input-free explanation naming both sizes.
    std::string Message;
};

/// Verify a capture landed at the requested size. `RequestScenarioCaptureWindowResize` is
/// fire-and-forget — it stages a resize the frame loop services with `glfwSetWindowSize`,
/// which the window system is free to CLAMP. On a Windows runner whose desktop is smaller
/// than the request, Win32 MaxTrackSize caps a top-level window near the desktop size, so a
/// 1920x1009 request came back as a 1028x749 framebuffer; the scenario still reported
/// `ok:true` and wrote a PNG, and the bucket-C golden diff could only say `dimension
/// mismatch` (L_inf -1) — which its sanctioned mask swallowed, leaving the lane green while
/// it compared nothing (#2092). A wrong-sized capture is never a valid golden-diff input, so
/// it fails at the source rather than downstream. `actual` <= 0 (no capture serviced yet) is
/// likewise not a pass.
ScenarioCaptureSizeVerdict VerifyScenarioCaptureSize(const ScenarioCaptureSize& requested, int actualWidth,
                                                     int actualHeight);

// Stage a g_ui window-resize request to the capture size (consumed by the
// standalone frame loop on the next frame). Out-of-line so this header stays
// free of the SmatchetUiSession include; shared by the screenshot scenarios
// via ConfigureScreenshotScenario.
void RequestScenarioCaptureWindowResize(const ScenarioCaptureSize& size);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_SIZING_H
