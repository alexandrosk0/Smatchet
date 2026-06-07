// VsyncControl pure-logic tests — the live vsync hub (Set/Get) and the boot
// precedence resolver (env SMATCHET_FPS_VSYNC > CLI flag > persisted config).
//
// `config.set vsync` command-path coverage is intentionally out of bounds for
// this pure-logic rig — it requires the full CommandRegistry harness (same
// boundary as SingleClickEditConfig.test.cpp); the table row + live-apply hook
// are covered by the CLI smoke in the plan's verification instead.
//
// Plan: docs/plans/active/vsync-toggle.md

#include "VsyncControl.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

namespace {

// Cross-platform setenv/unsetenv for the env-precedence cases.
void SetEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void UnsetEnvVar(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// RAII: leave the hub enabled and the env var unset for adjacent cases.
struct VsyncStateGuard {
    ~VsyncStateGuard() {
        UnsetEnvVar("SMATCHET_FPS_VSYNC");
        smatchet::vsync::SetEnabled(true);
    }
};

} // namespace

TEST_CASE("vsync hub Set/Get round-trips and defaults to enabled") {
    VsyncStateGuard guard;

    // Process default is enabled (matches glfwSwapInterval(1) legacy behaviour).
    smatchet::vsync::SetEnabled(true);
    CHECK(smatchet::vsync::Enabled());

    smatchet::vsync::SetEnabled(false);
    CHECK_FALSE(smatchet::vsync::Enabled());

    smatchet::vsync::SetEnabled(true);
    CHECK(smatchet::vsync::Enabled());
}

TEST_CASE("SeedFromBootSources: persisted config is the fallback source") {
    VsyncStateGuard guard;
    UnsetEnvVar("SMATCHET_FPS_VSYNC");

    const char* src = smatchet::vsync::SeedFromBootSources(false, false, true);
    CHECK(std::string(src) == "persisted config");
    CHECK_FALSE(smatchet::vsync::Enabled());

    src = smatchet::vsync::SeedFromBootSources(true, false, false);
    CHECK(std::string(src) == "persisted config");
    CHECK(smatchet::vsync::Enabled());
}

TEST_CASE("SeedFromBootSources: CLI flag beats persisted config") {
    VsyncStateGuard guard;
    UnsetEnvVar("SMATCHET_FPS_VSYNC");

    // --no-vsync over config-on.
    const char* src = smatchet::vsync::SeedFromBootSources(true, true, false);
    CHECK(std::string(src) == "CLI flag");
    CHECK_FALSE(smatchet::vsync::Enabled());

    // --vsync over config-off.
    src = smatchet::vsync::SeedFromBootSources(false, true, true);
    CHECK(std::string(src) == "CLI flag");
    CHECK(smatchet::vsync::Enabled());
}

TEST_CASE("SeedFromBootSources: SMATCHET_FPS_VSYNC env beats flag and config") {
    VsyncStateGuard guard;

    // '0' = off (the original FPS-measure contract), over flag-on + config-on.
    SetEnvVar("SMATCHET_FPS_VSYNC", "0");
    const char* src = smatchet::vsync::SeedFromBootSources(true, true, true);
    CHECK(std::string(src) == "env SMATCHET_FPS_VSYNC");
    CHECK_FALSE(smatchet::vsync::Enabled());

    // Any non-'0' first char = on, over flag-off + config-off.
    SetEnvVar("SMATCHET_FPS_VSYNC", "1");
    src = smatchet::vsync::SeedFromBootSources(false, true, false);
    CHECK(std::string(src) == "env SMATCHET_FPS_VSYNC");
    CHECK(smatchet::vsync::Enabled());
}
