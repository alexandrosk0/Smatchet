#ifndef SMATCHET_TARGET_STANDALONE_APP_BOOTSTRAP_H
#define SMATCHET_TARGET_STANDALONE_APP_BOOTSTRAP_H

// Hidden-window GLFW + ImGui + AppController boot shared by light in-process CLI
// and (future) ephemeral GUI refactor. See docs/design/light-release-unreal-default.md.

#include "ConfigManager.h"

#include <functional>
#include <memory>
#include <string>

struct GLFWwindow;

class AppController;
class PluginHost;
class SmatchetUI;

namespace smatchet {
namespace standalone {

enum class HeadlessCliMode {
    MetaCommand, ///< tier-1: init → dispatch → exit (no render loop)
    ScenarioRun  ///< tier-2: init → render loop until scenario/async work completes
};

struct BootstrapContext {
    GLFWwindow* window = nullptr;
    std::unique_ptr<AppController> app;
    std::unique_ptr<PluginHost> pluginHost;
    std::unique_ptr<SmatchetUI> mainWindow;
    const char* glslVersion = nullptr;
    ~BootstrapContext();
};

/// Parse standalone argv (`--db-path`, `--mcp-port`, `--help`, …). Returns false when `--help` was printed.
bool ParseStandaloneCli(int argc, char** argv, ConfigManager::CliOverrides& cli);

/// Register plugins and initialize AppController. Caller must have created GLFW + ImGui already
/// (`ctx.window` non-null) unless this is invoked from `Initialize` (hidden boot).
bool InitAppAndPlugins(BootstrapContext& ctx, const TrackerConfig& cfg, bool forceMcp, std::string& err);

/// Boot a hidden standalone instance. `err` receives a human-readable failure reason.
/// When `forceMcp` is true (`--ephemeral`), MCP plugin registers even if config has MCP disabled.
bool Initialize(BootstrapContext& ctx, int argc, char** argv, HeadlessCliMode mode, std::string& err,
                bool forceMcp = false);

/// `--ephemeral` path: hidden window + forced MCP + render loop until the window closes.
bool BootEphemeral(int argc, char** argv, std::string& err);

/// Minimal render loop for tier-2 CLI commands (scenario.run, etc.). Calls `shouldStop`
/// after each frame; return true to exit the loop.
void RunRenderLoop(BootstrapContext& ctx, const std::function<bool()>& shouldStop);

/// Tear down app/plugin/window-owned UI only (ImGui/GLFW lifetime stays with caller).
void ShutdownApplication(BootstrapContext& ctx);

void Shutdown(BootstrapContext& ctx);

} // namespace standalone
} // namespace smatchet

#endif // SMATCHET_TARGET_STANDALONE_APP_BOOTSTRAP_H
