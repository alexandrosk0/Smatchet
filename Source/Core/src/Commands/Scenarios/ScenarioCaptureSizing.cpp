#include "Commands/Scenarios/ScenarioCaptureSizing.h"

#include "Logger.h"
#include "SmatchetUiSession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

// Same unconditional-extern shim the scenario TUs use: g_ui is defined in
// SmatchetUI.cpp without a guard while the header-side extern is gated on
// SMATCHET_WITH_LUA_AUTOMATION.
extern UiDrawSession g_ui;

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

void RequestScenarioCaptureWindowResize(const ScenarioCaptureSize& size) {
    g_ui.requestWindowWidth = size.Width;
    g_ui.requestWindowHeight = size.Height;
    g_ui.requestWindowResize = true;
}

} // namespace cmd
} // namespace smatchet
