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

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_SIZING_H
