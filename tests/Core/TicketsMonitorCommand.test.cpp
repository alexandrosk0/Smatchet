// TicketsMonitorCommand — bucket-A coverage for the `tickets.monitor on|off|status` decision
// helper (Source/Core/include/Commands/TicketsMonitorCommandPure.h) plus the config-side
// clamp on the poll interval it surfaces. The pure decision is unit-tested without the
// CommandRegistry/ConfigManager filesystem surface (CommandRegistry tests are out of bounds for
// the pure ctest rig); the clamp is verified through a real Save/Load round-trip via TestEnvGuard.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"

#include <doctest/doctest.h>

#include "Commands/TicketsMonitorCommandPure.h"

using smatchet::cmd::DecideTicketsMonitor;
using smatchet::cmd::TicketsMonitorAction;
using smatchet::cmd::TicketsMonitorDecision;

TEST_CASE("status (and empty) is a read-only query, no write") {
    for (const char* arg : {"", "status"}) {
        const TicketsMonitorDecision d = DecideTicketsMonitor(arg);
        CHECK(d.Action == TicketsMonitorAction::Status);
        CHECK(d.WriteEnabled == false);
        CHECK(d.Error.empty());
    }
}

TEST_CASE("on/off request a persist of the matching flag") {
    const TicketsMonitorDecision on = DecideTicketsMonitor("on");
    CHECK(on.Action == TicketsMonitorAction::On);
    CHECK(on.WriteEnabled == true);
    CHECK(on.EnabledValue == true);
    CHECK(on.Error.empty());

    const TicketsMonitorDecision off = DecideTicketsMonitor("off");
    CHECK(off.Action == TicketsMonitorAction::Off);
    CHECK(off.WriteEnabled == true);
    CHECK(off.EnabledValue == false);
    CHECK(off.Error.empty());
}

TEST_CASE("unknown verb is invalid and carries a usage hint, never writes") {
    const TicketsMonitorDecision d = DecideTicketsMonitor("enable");
    CHECK(d.Action == TicketsMonitorAction::Invalid);
    CHECK(d.WriteEnabled == false);
    CHECK(d.Error.find("Usage: tickets.monitor") != std::string::npos);
    CHECK(d.Error.find("enable") != std::string::npos);
}

TEST_CASE("poll interval is clamped to [30, 3600] s on Load") {
    smatchet_tests::TestEnvGuard env;

    TrackerConfig tooLow;
    tooLow.TicketChangeMonitorIntervalSec = 5; // below the floor
    ConfigManager::Save(tooLow);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().TicketChangeMonitorIntervalSec == 30);

    TrackerConfig tooHigh;
    tooHigh.TicketChangeMonitorIntervalSec = 999999; // above the ceiling
    ConfigManager::Save(tooHigh);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().TicketChangeMonitorIntervalSec == 3600);

    TrackerConfig inBand;
    inBand.TicketChangeMonitorIntervalSec = 240; // untouched
    ConfigManager::Save(inBand);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().TicketChangeMonitorIntervalSec == 240);
}

TEST_CASE("delta sync config: defaults, round-trip, and the row-age bound is clamped to [300, 604800] s on Load") {
    smatchet_tests::TestEnvGuard env;
    const TrackerConfig def;
    CHECK(def.DeltaSyncEnabled == true);
    CHECK(def.DeltaSyncMaxRowAgeSec == 21600);

    TrackerConfig tooLow;
    tooLow.DeltaSyncMaxRowAgeSec = 5;
    ConfigManager::Save(tooLow);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().DeltaSyncMaxRowAgeSec == 300);

    TrackerConfig tooHigh;
    tooHigh.DeltaSyncMaxRowAgeSec = 99999999;
    ConfigManager::Save(tooHigh);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().DeltaSyncMaxRowAgeSec == 604800);

    TrackerConfig custom;
    custom.DeltaSyncEnabled = false;
    custom.DeltaSyncMaxRowAgeSec = 7200;
    ConfigManager::Save(custom);
    ConfigManager::InvalidateCache();
    const TrackerConfig loaded = ConfigManager::Load();
    CHECK(loaded.DeltaSyncEnabled == false);
    CHECK(loaded.DeltaSyncMaxRowAgeSec == 7200);
}

TEST_CASE("lookup lifetime config: defaults, round-trip, clamps on Load") {
    smatchet_tests::TestEnvGuard env;
    const TrackerConfig def;
    CHECK(def.FieldCatalogRefreshTtlSec == 43200);
    CHECK(def.LookupCacheTtlSec == 0);

    TrackerConfig tooLow;
    tooLow.FieldCatalogRefreshTtlSec = 1;
    tooLow.LookupCacheTtlSec = 1;
    ConfigManager::Save(tooLow);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().FieldCatalogRefreshTtlSec == 300);
    CHECK(ConfigManager::Load().LookupCacheTtlSec == 60);

    TrackerConfig tooHigh;
    tooHigh.FieldCatalogRefreshTtlSec = 999999999;
    tooHigh.LookupCacheTtlSec = 999999999;
    ConfigManager::Save(tooHigh);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().FieldCatalogRefreshTtlSec == 2592000);
    CHECK(ConfigManager::Load().LookupCacheTtlSec == 2592000);

    TrackerConfig custom;
    custom.FieldCatalogRefreshTtlSec = 3600;
    custom.LookupCacheTtlSec = 7200;
    ConfigManager::Save(custom);
    ConfigManager::InvalidateCache();
    const TrackerConfig loaded = ConfigManager::Load();
    CHECK(loaded.FieldCatalogRefreshTtlSec == 3600);
    CHECK(loaded.LookupCacheTtlSec == 7200);

    TrackerConfig off;
    off.LookupCacheTtlSec = 0;
    ConfigManager::Save(off);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().LookupCacheTtlSec == 0);
}
