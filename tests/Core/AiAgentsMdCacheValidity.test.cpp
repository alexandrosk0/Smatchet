// Pure-helper tests for the agents.md cache-validity decision extracted from
// `AiAssistantController::BuildChatPayload`. The helper lives in the
// `smatchet::ai::pure` namespace and is header-defined (`inline`) so this rig
// links it without the controller TU's AppController / cpr / Ui-session
// dependencies.
//
// Regression coverage for the #1xxx CodeRabbit finding: the cache key omitted
// cfg.AgentsMdAutoDiscoverProject, so toggling auto-discovery while both paths
// were unchanged served a stale cached blob. The cache must now invalidate on
// any AgentsMdAutoDiscoverProject toggle and stay valid only when nothing
// changed (and it was already populated).
//
// The whole TU self-gates on SMATCHET_WITH_AI to match the controller header,
// which collapses to nothing when AI is disabled.

#if defined(SMATCHET_WITH_AI)

#include "AiAssistantController.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::ai::pure::AgentsMdCacheStillValid;

TEST_CASE("AgentsMdCacheStillValid: identical inputs with valid cache stay valid") {
    CHECK(AgentsMdCacheStillValid(true, "/g", "/p", true, "/g", "/p", true));
    CHECK(AgentsMdCacheStillValid(true, "/g", "/p", false, "/g", "/p", false));
}

TEST_CASE("AgentsMdCacheStillValid: an uninitialised cache is never valid") {
    CHECK_FALSE(AgentsMdCacheStillValid(false, "/g", "/p", true, "/g", "/p", true));
    // Even with everything else matching, cacheValid=false short-circuits.
    CHECK_FALSE(AgentsMdCacheStillValid(false, "", "", false, "", "", false));
}

TEST_CASE("AgentsMdCacheStillValid: toggling auto-discover invalidates with paths unchanged") {
    // This is the #1xxx bug: same paths, only the flag flips.
    CHECK_FALSE(AgentsMdCacheStillValid(true, "/g", "/p", true, "/g", "/p", false));
    CHECK_FALSE(AgentsMdCacheStillValid(true, "/g", "/p", false, "/g", "/p", true));
}

TEST_CASE("AgentsMdCacheStillValid: changing either path invalidates") {
    CHECK_FALSE(AgentsMdCacheStillValid(true, "/g", "/p", true, "/g2", "/p", true));
    CHECK_FALSE(AgentsMdCacheStillValid(true, "/g", "/p", true, "/g", "/p2", true));
}

TEST_CASE("AgentsMdCacheStillValid: changing path and flag together invalidates") {
    CHECK_FALSE(AgentsMdCacheStillValid(true, "/g", "/p", true, "/g2", "/p2", false));
}

#endif // SMATCHET_WITH_AI
