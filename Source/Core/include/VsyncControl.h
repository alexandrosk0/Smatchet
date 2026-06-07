#pragma once

// VsyncControl — tiny GLFW-free live vsync hub (the runtime source of truth).
//
// The persisted TrackerConfig::VsyncEnabled is the durable source of truth that
// seeds this hub at boot; every surface (Preferences checkbox, `config.set
// vsync`, the --vsync/--no-vsync CLI flag, the SMATCHET_FPS_VSYNC env var)
// funnels through SetEnabled(). The standalone render loop reads Enabled() each
// frame and calls glfwSwapInterval only on change. No GLFW here — this header
// compiles into SmatchetCore_DX12 too (the Unreal host owns its swapchain and
// honours the intent where it can).
//
// Plan: docs/plans/active/vsync-toggle.md

#include <atomic>
#include <cstdlib>

namespace smatchet {
namespace vsync {

inline std::atomic<bool>& State() {
    static std::atomic<bool> enabled(true);
    return enabled;
}

inline void SetEnabled(bool on) { State().store(on, std::memory_order_relaxed); }

inline bool Enabled() { return State().load(std::memory_order_relaxed); }

// Boot-time seed with the documented precedence:
//   SMATCHET_FPS_VSYNC env  >  --vsync/--no-vsync CLI flag  >  persisted config.
// Returns the resolved source name so the caller can LOG_INFO it (the boot log
// states where the effective value came from). Env semantics match the original
// FPS-measure contract: first char '0' = off, anything else = on.
inline const char* SeedFromBootSources(bool cfgEnabled, bool hasCliFlag, bool cliEnabled) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* v = std::getenv("SMATCHET_FPS_VSYNC");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (v != nullptr && v[0] != '\0') {
        SetEnabled(v[0] != '0');
        return "env SMATCHET_FPS_VSYNC";
    }
    if (hasCliFlag) {
        SetEnabled(cliEnabled);
        return "CLI flag";
    }
    SetEnabled(cfgEnabled);
    return "persisted config";
}

} // namespace vsync
} // namespace smatchet
