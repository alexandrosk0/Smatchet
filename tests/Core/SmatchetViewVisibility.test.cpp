// SmatchetViewVisibility — maps a ViewSlot toggle onto the correct cfg.Show* flag.
// Pure over a TrackerConfig value (ConfigManager.cpp is already linked into the rig).

#include <doctest/doctest.h>

#include "Ui/SmatchetViewVisibility.h"

#include "ConfigManager.h"

TEST_CASE("SetViewVisible drives the primary sidebar flag") {
    TrackerConfig cfg;
    SetViewVisible(cfg, ViewSlot::PrimarySideBar, false);
    CHECK_FALSE(cfg.ShowPrimarySideBar);
    SetViewVisible(cfg, ViewSlot::PrimarySideBar, true);
    CHECK(cfg.ShowPrimarySideBar);
}

TEST_CASE("SetViewVisible drives the secondary sidebar flag") {
    TrackerConfig cfg;
    SetViewVisible(cfg, ViewSlot::SecondarySideBar, false);
    CHECK_FALSE(cfg.ShowSecondarySideBar);
    SetViewVisible(cfg, ViewSlot::SecondarySideBar, true);
    CHECK(cfg.ShowSecondarySideBar);
}

TEST_CASE("SetViewVisible drives the bottom panel flag") {
    TrackerConfig cfg;
    SetViewVisible(cfg, ViewSlot::BottomPanel, false);
    CHECK_FALSE(cfg.ShowPanel);
    SetViewVisible(cfg, ViewSlot::BottomPanel, true);
    CHECK(cfg.ShowPanel);
}

TEST_CASE("Each slot maps to a distinct flag (no cross-talk)") {
    TrackerConfig cfg;
    cfg.ShowPrimarySideBar = true;
    cfg.ShowSecondarySideBar = true;
    cfg.ShowPanel = true;
    SetViewVisible(cfg, ViewSlot::BottomPanel, false);
    // Only the bottom panel flag moved.
    CHECK(cfg.ShowPrimarySideBar);
    CHECK(cfg.ShowSecondarySideBar);
    CHECK_FALSE(cfg.ShowPanel);
}
