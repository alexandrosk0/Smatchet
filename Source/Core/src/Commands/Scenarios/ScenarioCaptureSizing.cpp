#include "Commands/Scenarios/ScenarioCaptureSizing.h"

#include "Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace smatchet {
namespace cmd {

namespace {

int IntArg(const nlohmann::json& args, const char* key, int fallback) {
    if (!args.contains(key)) {
        LOG_TRACE("ScenarioCaptureSizing::IntArg: key '%s' not found, using fallback %d", key, fallback);
        return fallback;
    }
    const nlohmann::json& v = args[key];
    if (v.is_number()) {
        return v.get<int>();
    }
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
            LOG_TRACE("ScenarioCaptureSizing::IntArg: key '%s' parse error, using fallback %d", key, fallback);
            return fallback;
        }
    }
    LOG_TRACE("ScenarioCaptureSizing::IntArg: key '%s' has unexpected type, using fallback %d", key, fallback);
    return fallback;
}

} // namespace

ScenarioCaptureSize ParseScenarioCaptureSize(const nlohmann::json& args) {
    ScenarioCaptureSize size;
    size.Width = (std::max)(320, IntArg(args, "windowWidth", size.Width));
    size.Height = (std::max)(240, IntArg(args, "windowHeight", size.Height));
    return size;
}

ScenarioCaptureSizeVerdict VerifyScenarioCaptureSize(const ScenarioCaptureSize& requested, int actualWidth,
                                                     int actualHeight) {
    ScenarioCaptureSizeVerdict verdict;
    if (actualWidth <= 0 || actualHeight <= 0) {
        verdict.Ok = false;
        verdict.Message = "no capture was recorded (requested " + std::to_string(requested.Width) + "x" +
                          std::to_string(requested.Height) + ")";
        return verdict;
    }
    if (actualWidth == requested.Width && actualHeight == requested.Height) {
        return verdict;
    }
    verdict.Ok = false;
    verdict.Message = "capture size " + std::to_string(actualWidth) + "x" + std::to_string(actualHeight) +
                      " does not match the requested " + std::to_string(requested.Width) + "x" +
                      std::to_string(requested.Height) +
                      " — the window-system clamped the resize (desktop smaller than the request?); "
                      "this capture cannot be golden-diffed";
    return verdict;
}

} // namespace cmd
} // namespace smatchet
