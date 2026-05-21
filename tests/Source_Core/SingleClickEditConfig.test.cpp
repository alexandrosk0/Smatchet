// SingleClickToEditGridCells config round-trip — covers Save/Load behaviour for the
// new TrackerConfig bool added by `docs/design/single-click-grid-edit-toggle.md`.
//
// Round-trip path: TrackerConfig field → ConfigManager::Save() writes
// `single_click_to_edit_grid_cells` key → ConfigManager::Load() reads it back with
// `j.value(..., default)` so missing keys default to the in-struct default (`true`).
//
// `config.set` / `config.get` command-path coverage is intentionally out of bounds for
// this pure-logic rig — it requires the full CommandRegistry harness, which lives behind
// the AppController surface (not exercised by ctest). Tracked as deferred bucket-E or
// process residue per the plan's verification §.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <string>

TEST_CASE("SingleClickToEditGridCells round-trips via Save/Load") {
    smatchet_tests::TestEnvGuard env;

    // Toggle to false, save, reload — value survives the round-trip.
    TrackerConfig cfg = ConfigManager::Load();
    CHECK(cfg.SingleClickToEditGridCells == true); // struct default + first-load default
    cfg.SingleClickToEditGridCells = false;
    ConfigManager::Save(cfg);
    ConfigManager::InvalidateCache();
    const TrackerConfig reloaded = ConfigManager::Load();
    CHECK(reloaded.SingleClickToEditGridCells == false);

    // Flip back to true, repeat.
    TrackerConfig cfg2 = ConfigManager::Load();
    cfg2.SingleClickToEditGridCells = true;
    ConfigManager::Save(cfg2);
    ConfigManager::InvalidateCache();
    const TrackerConfig reloaded2 = ConfigManager::Load();
    CHECK(reloaded2.SingleClickToEditGridCells == true);
}

TEST_CASE("SingleClickToEditGridCells defaults to true when key absent from JSON") {
    smatchet_tests::TestEnvGuard env;

    // Overwrite the env-guard's minimal config with one that omits the new key entirely.
    const std::string cfgPath = env.DirWithSep() + "smatchet_config.json";
    {
        std::ofstream f(cfgPath, std::ios::binary | std::ios::trunc);
        f << "{\"read_only_mode\":false}";
    }
    ConfigManager::InvalidateCache();
    const TrackerConfig cfg = ConfigManager::Load();
    CHECK(cfg.SingleClickToEditGridCells == true);
}
