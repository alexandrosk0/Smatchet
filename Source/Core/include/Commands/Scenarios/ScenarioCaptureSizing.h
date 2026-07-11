#ifndef SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_SIZING_H
#define SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_SIZING_H

#include <nlohmann/json_fwd.hpp>

namespace smatchet {
namespace cmd {

struct ScenarioCaptureSize {
    int Width = 1920;
    int Height = 1009;
};

ScenarioCaptureSize ParseScenarioCaptureSize(const nlohmann::json& args);

// Stage a g_ui window-resize request to the capture size (consumed by the
// standalone frame loop on the next frame). Out-of-line so this header stays
// free of the SmatchetUiSession include; shared by the screenshot scenarios
// via ConfigureScreenshotScenario.
void RequestScenarioCaptureWindowResize(const ScenarioCaptureSize& size);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_SIZING_H
