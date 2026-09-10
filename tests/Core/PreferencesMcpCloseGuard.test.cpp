#include <doctest/doctest.h>

#include "SmatchetPreferencesUi_detail.h"

#include <string>

// Bucket-A coverage for the MCP half of the Preferences close gate (#2133). The gate used to key
// on Tracker dirtiness alone, so an MCP auth-token / port edit that still held keyboard focus
// while its section was collapsed (or filtered out) was silently dropped on close: the section
// commits only when the field is no longer being edited (#2110), and a section that does not
// draw cannot commit. McpPrefsFieldsDiffer is the pure predicate the gate now consults.

using SmatchetPreferencesUiDetail::McpPrefsFieldsDiffer;

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
