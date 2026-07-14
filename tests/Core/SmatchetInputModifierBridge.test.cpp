// SmatchetInputModifierBridge — last ctrl/shift modifier flags pushed from the
// plugin input thread. Process-global atomics, so each case sets-then-reads.

#include <doctest/doctest.h>

#include "SmatchetInputModifierBridge.h"

TEST_CASE("Ctrl+Shift both set round-trips through the getters") {
    SmatchetInput_SetPluginModifiersPushed(true, true);
    CHECK(SmatchetInput_GetPluginModifiersPushedCtrl());
    CHECK(SmatchetInput_GetPluginModifiersPushedShift());
}

TEST_CASE("Both cleared round-trips") {
    SmatchetInput_SetPluginModifiersPushed(false, false);
    CHECK_FALSE(SmatchetInput_GetPluginModifiersPushedCtrl());
    CHECK_FALSE(SmatchetInput_GetPluginModifiersPushedShift());
}

TEST_CASE("Ctrl and Shift are independent") {
    SmatchetInput_SetPluginModifiersPushed(true, false);
    CHECK(SmatchetInput_GetPluginModifiersPushedCtrl());
    CHECK_FALSE(SmatchetInput_GetPluginModifiersPushedShift());

    SmatchetInput_SetPluginModifiersPushed(false, true);
    CHECK_FALSE(SmatchetInput_GetPluginModifiersPushedCtrl());
    CHECK(SmatchetInput_GetPluginModifiersPushedShift());
}
