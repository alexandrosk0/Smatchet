#ifndef SMATCHET_COMMANDS_SCENARIOS_SCENARIO_SCREENSHOT_PATH_H
#define SMATCHET_COMMANDS_SCENARIOS_SCENARIO_SCREENSHOT_PATH_H

#include <string>

#include "Commands/PathConfinement.h"
#include "Commands/Scenarios/ScenarioArgs.h"
#include "Commands/Scenarios/ScenarioCaptureSizing.h"
#include "ConfigManager.h"

namespace smatchet {
namespace cmd {

// Confine a caller-supplied scenario `screenshotPath` under <userData>/screenshots/.
// Every screenshot-capturing scenario (dock-gap-sentinel, command-palette-fuzzy,
// code-syntax-coloring, theme-switch-roundtrip) is registered as a normal command and is
// therefore MCP- AND Lua-reachable; an unconfined screenshotPath is an arbitrary-file-write
// primitive (the #1566 class — SECURITY_AUDIT.md "MCP-reachable arbitrary file read/write").
// On success `resolvedOut` is the absolute path inside the screenshots subdir and the
// function returns true; on any escape (absolute path, '..' traversal, symlinked dir) it
// returns false with a stable, input-free `errOut` and the caller rejects the scenario.
//
// The trusted, directly-invoked --spawn CLI parent (Source/Standalone/CliCommandRunner.cpp)
// still honours a user's absolute --screenshotPath: it swaps in a confine-safe basename so
// this check passes in the child, then copies the child's confined capture back to the user's
// path itself (see SpawnAndRun's parent-fulfill path). In-process MCP/Lua callers have no such
// trusted parent, so confinement applies directly — which is exactly the fix.
inline bool ConfineScenarioScreenshotPath(const std::string& candidate, std::string& resolvedOut, std::string& errOut) {
    return ConfinePathUnderSubdir(ConfigManager::GetUserDataDirectory(), "screenshots", candidate, resolvedOut, errOut);
}

// In-place convenience over ConfineScenarioScreenshotPath for the screenshot scenarios:
// on accept, overwrites `path` with the confined absolute path and returns true; on any
// escape, sets `outErr` to a stable, scenario-prefixed reject message and returns false
// (the caller then returns from OnStart before mutating any session/global state). One
// chokepoint so the four scenarios share a single confine+reject line — no per-scenario
// copy-paste confine block (DRY Engineering Pillar 5).
inline bool ConfineScenarioScreenshotPathInPlace(const char* scenarioName, std::string& path, std::string& outErr) {
    std::string confined;
    std::string confineErr;
    if (!ConfineScenarioScreenshotPath(path, confined, confineErr)) {
        outErr = std::string(scenarioName) + ": screenshotPath rejected: " + confineErr;
        return false;
    }
    path = confined;
    return true;
}

// Shared OnStart prologue for the screenshot scenarios (debt 2026-06-28): parse +
// clamp warmupFrames, parse captureSize, require + confine screenshotPath, then
// stage the g_ui window-resize request to the capture size. Returns false with a
// scenario-prefixed outErr when screenshotPath is missing or escapes the
// screenshots subdir — no session/global state mutates on a reject, exactly as
// with the confine chokepoint above.
inline bool ConfigureScreenshotScenario(const char* scenarioName, const nlohmann::json& args, int warmupDefault,
                                        int warmupMin, int& warmupFrames, ScenarioCaptureSize& captureSize,
                                        std::string& screenshotPath, std::string& outErr) {
    warmupFrames = IntArg(args, "warmupFrames", warmupDefault);
    if (warmupFrames < warmupMin) {
        warmupFrames = warmupMin;
    }
    captureSize = ParseScenarioCaptureSize(args);
    screenshotPath = StringArg(args, "screenshotPath", std::string());
    if (screenshotPath.empty()) {
        outErr = std::string(scenarioName) + ": screenshotPath is required";
        return false;
    }
    if (!ConfineScenarioScreenshotPathInPlace(scenarioName, screenshotPath, outErr)) {
        return false;
    }
    RequestScenarioCaptureWindowResize(captureSize);
    return true;
}

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_SCENARIO_SCREENSHOT_PATH_H
