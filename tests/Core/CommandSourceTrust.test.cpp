// CommandSourceTrust — unit gate for the source-aware destructive guard
// (security audit 2026-06-13 #2/#3). The command registry runs destructive
// commands behind a single chokepoint; #2 flagged that the gate ignored
// ctx.Source, and #3 flagged that a token-holding MCP/Lua caller reached the
// full registry. The chosen posture is "require-confirm destructive on non-UI
// sources": automation sources (CLI/MCP/Lua) must pass an EXPLICIT per-call
// confirm signal; interactive-UI sources (Palette/Unreal) gate intent in the
// front-end. The gate itself is uniform — no source may bypass the confirm
// requirement — and RequiresExplicitConfirm is the single trust decision.
//
// This test pins that pure decision (and the IsAutomationSource classifier the
// audit-log path keys on) without linking the registry / handlers.
//
// See Source/Core/include/Commands/Command.h.

#include <doctest/doctest.h>

#include "Commands/Command.h"

using smatchet::cmd::CommandSource;
using smatchet::cmd::IsAutomationSource;
using smatchet::cmd::RequiresExplicitConfirm;

TEST_CASE("RequiresExplicitConfirm: MCP destructive unconfirmed is blocked (#3)") {
    // Token possession must not equal full reach: a destructive MCP call with no
    // explicit __confirm is blocked with ConfirmRequired.
    CHECK(RequiresExplicitConfirm(CommandSource::Mcp, /*destructive=*/true,
                                  /*confirmed=*/false, /*dryRun=*/false) == true);
}

TEST_CASE("RequiresExplicitConfirm: MCP destructive confirmed is allowed") {
    // The explicit per-call __confirm:true signal releases the gate.
    CHECK(RequiresExplicitConfirm(CommandSource::Mcp, true, /*confirmed=*/true, false) == false);
}

TEST_CASE("RequiresExplicitConfirm: Lua destructive unconfirmed is blocked (#3)") {
    CHECK(RequiresExplicitConfirm(CommandSource::Lua, true, false, false) == true);
}

TEST_CASE("RequiresExplicitConfirm: CLI destructive unconfirmed is blocked (#2)") {
    // CLI without --yes (which sets confirmed) is blocked.
    CHECK(RequiresExplicitConfirm(CommandSource::Cli, true, false, false) == true);
}

TEST_CASE("RequiresExplicitConfirm: Palette destructive confirmed is allowed") {
    // Interactive-UI source: the palette sets confirmed deliberately after its
    // Shift+Enter gate. Confirmed -> allowed.
    CHECK(RequiresExplicitConfirm(CommandSource::Palette, true, /*confirmed=*/true, false) == false);
}

TEST_CASE("RequiresExplicitConfirm: non-destructive command is always allowed for every source") {
    const CommandSource all[] = {CommandSource::Cli,    CommandSource::Palette,  CommandSource::Mcp,
                                 CommandSource::Lua,    CommandSource::Unreal,   CommandSource::Internal};
    for (CommandSource s : all) {
        CHECK(RequiresExplicitConfirm(s, /*destructive=*/false, /*confirmed=*/false, /*dryRun=*/false) == false);
    }
}

TEST_CASE("RequiresExplicitConfirm: dry-run bypasses the gate even when destructive + unconfirmed") {
    // Dry-run mutates nothing, so it is safe without confirmation from any source.
    CHECK(RequiresExplicitConfirm(CommandSource::Mcp, true, /*confirmed=*/false, /*dryRun=*/true) == false);
    CHECK(RequiresExplicitConfirm(CommandSource::Lua, true, false, /*dryRun=*/true) == false);
    CHECK(RequiresExplicitConfirm(CommandSource::Cli, true, false, /*dryRun=*/true) == false);
}

TEST_CASE("IsAutomationSource: CLI/MCP/Lua are automation; Palette/Unreal/Internal are not") {
    CHECK(IsAutomationSource(CommandSource::Cli) == true);
    CHECK(IsAutomationSource(CommandSource::Mcp) == true);
    CHECK(IsAutomationSource(CommandSource::Lua) == true);
    CHECK(IsAutomationSource(CommandSource::Palette) == false);
    CHECK(IsAutomationSource(CommandSource::Unreal) == false);
    CHECK(IsAutomationSource(CommandSource::Internal) == false);
}
