#include <doctest/doctest.h>

#include "SmatchetPreferencesUi_detail.h"

#include <string>

// Bucket-A coverage for the MCP half of the Preferences close gate (#2133). The gate used to key
// on Tracker dirtiness alone, so an MCP auth-token / port edit that still held keyboard focus
// while its section was collapsed (or filtered out) was silently dropped on close: the section
// commits only when the field is no longer being edited (#2110), and a section that does not
// draw cannot commit. McpPrefsFieldsDiffer is the pure predicate the gate now consults.

// The gate later gained a third domain, the Assistant's staged working copy. PickPrefsCloseGuardKind
// is the pure mapping from the three dirty flags to the sentence the guard modal shows.

using SmatchetPreferencesUiDetail::McpPrefsFieldsDiffer;
using SmatchetPreferencesUiDetail::PickPrefsCloseGuardKind;
using SmatchetPreferencesUiDetail::PrefsCloseGuardKind;

namespace {
TrackerConfig SavedConfig() {
    TrackerConfig cfg;
    cfg.McpEnabled = true;
    cfg.McpPort = 4242;
    cfg.McpAllowRemote = false;
    cfg.McpAllowLuaExecution = false;
    cfg.McpAuthToken = "secret";
    return cfg;
}
} // namespace

TEST_CASE("McpPrefsFieldsDiffer — buffers mirroring the saved config read clean") {
    const TrackerConfig cfg = SavedConfig();
    CHECK_FALSE(McpPrefsFieldsDiffer(true, 4242, false, false, "secret", cfg));
}

TEST_CASE("McpPrefsFieldsDiffer — a typed prefix that never committed is dirty (#2133 repro)") {
    const TrackerConfig cfg = SavedConfig();
    // The user typed into the token field, collapsed the section while it still had focus, then
    // closed the window: the buffer holds the prefix, the config still holds the old value.
    CHECK(McpPrefsFieldsDiffer(true, 4242, false, false, "secre", cfg));
    CHECK(McpPrefsFieldsDiffer(true, 4242, false, false, "secret2", cfg));
    // Same for the port.
    CHECK(McpPrefsFieldsDiffer(true, 4243, false, false, "secret", cfg));
}

TEST_CASE("McpPrefsFieldsDiffer — every staged field participates") {
    const TrackerConfig cfg = SavedConfig();
    CHECK(McpPrefsFieldsDiffer(false, 4242, false, false, "secret", cfg));
    CHECK(McpPrefsFieldsDiffer(true, 4242, true, false, "secret", cfg));
    CHECK(McpPrefsFieldsDiffer(true, 4242, false, true, "secret", cfg));
}

TEST_CASE("PickPrefsCloseGuardKind — Tracker and MCP combinations keep the wording they had") {
    // Arguments: trackerDirty, mcpDirty, assistantDirty.
    CHECK(PickPrefsCloseGuardKind(true, false, false) == PrefsCloseGuardKind::Tracker);
    CHECK(PickPrefsCloseGuardKind(false, true, false) == PrefsCloseGuardKind::Mcp);
    CHECK(PickPrefsCloseGuardKind(true, true, false) == PrefsCloseGuardKind::TrackerAndMcp);
}

TEST_CASE("PickPrefsCloseGuardKind — nothing dirty falls back to the Tracker sentence") {
    // The modal can outlive the edit that opened it; the Tracker sentence was the fallback
    // before the Assistant joined the gate and stays it.
    CHECK(PickPrefsCloseGuardKind(false, false, false) == PrefsCloseGuardKind::Tracker);
}

TEST_CASE("PickPrefsCloseGuardKind — an Assistant-only edit is named as such") {
    CHECK(PickPrefsCloseGuardKind(false, false, true) == PrefsCloseGuardKind::Assistant);
}

TEST_CASE("PickPrefsCloseGuardKind — the Assistant plus any other domain gets the generic sentence") {
    // No sentence names three domains; the generic one must not claim only the Assistant is dirty.
    CHECK(PickPrefsCloseGuardKind(true, false, true) == PrefsCloseGuardKind::Generic);
    CHECK(PickPrefsCloseGuardKind(false, true, true) == PrefsCloseGuardKind::Generic);
    CHECK(PickPrefsCloseGuardKind(true, true, true) == PrefsCloseGuardKind::Generic);
}
