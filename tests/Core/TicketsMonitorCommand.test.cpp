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
