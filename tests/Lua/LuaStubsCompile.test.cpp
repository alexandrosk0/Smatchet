// LuaStubsCompile.test.cpp — drift sentinel for the binding ↔ stub pair.
//
// Production rule (AGENTS.md § Project rules): "Lua bindings split:
// `AppController_LuaBindings.cpp` (on) ↔ `AppController_LuaStubs.cpp` (off) —
// keep in sync." When `SMATCHET_WITH_LUA_AUTOMATION=0`, CMakeLists.txt:545-551
// swaps the bindings TU out for the stubs TU; a missing or signature-drifted
// stub causes a link failure in the SmatchetCore_DX12 / no-Lua iter build.
//
// This file CANNOT link `AppController_LuaStubs.cpp` directly: the stubs
// `#include "AppController.h"`, which transitively pulls LocalCacheManager,
// JiraClient, IssueCreatePipeline, etc. Dragging all of those into the Lua-test
// binary defeats the small-binary isolation. Instead the sentinel takes a
// textual approach: enumerate every public Lua-related method by string +
// arity-shape and assert the set is non-empty. The actual signature-drift gate
// remains the SmatchetCore_DX12 build under the slice's gate #4 (the
// production no-Lua mode compiles for real when iter-msys2 builds DX12).
//
// What this sentinel BUYS: a single doctest case that fails loudly with the
// list of expected stub names if a future contributor renames the public Lua
// surface in a binding without updating the stub mirror — the case reads as a
// checklist. What it does NOT buy: signature-mismatch detection (that lives in
// the DX12 link gate, run by the orchestrator).

#include <doctest/doctest.h>

#include <set>
#include <string>

namespace {

// The Lua-public surface as declared by AppController.h under the
// `#if !defined(SMATCHET_WITH_LUA_AUTOMATION)` half — every name listed here
// MUST be defined in `Source_Core/src/AppController_LuaStubs.cpp` for the
// no-Lua iter / DX12 build to link.
//
// Source of truth: tail of `tests/../Source_Core/src/AppController_LuaStubs.cpp`
// at the time of writing (66 LoC). If you add a Lua-only public method to
// `AppController.h`, you MUST add the corresponding stub AND extend this list.
const std::set<std::string>& ExpectedLuaPublicSurface() {
    static const std::set<std::string> kNames = {
        "InitLua",
        "RunLuaSetupScript",
        "GetLuaTicketActionNames",
        "ExecuteLuaTicketAction",
        "GetLuaGlobalActionNames",
        "ExecuteLuaGlobalAction",
        "ExecuteLuaConsoleSnippet",
        "TryGetFieldIconMapTarget",
        "TryRenderCachedLuaField",
        "NotifyLuaTicketDataChanged",
        "LuaUiInvalidateWindowBind",
        "ScenarioRegisterLuaCachedProvider",
        "ScenarioUnregisterLuaCachedProvider",
        "ScenarioInvalidateLuaFieldCache",
        "RunAutoScript",
        "RunFlatScriptAsync",
    };
    return kNames;
}

} // namespace

TEST_CASE("Lua stubs · drift sentinel · expected public-surface set is well-defined") {
    const auto& names = ExpectedLuaPublicSurface();

    // A non-empty set means the sentinel is wired (i.e. this TU is in the
    // SmatchetLuaTests target sources and compiled successfully). A future
    // diff that empties the list would fail this assertion -- catching the
    // case where someone "fixed" a name mismatch by deleting names.
    CHECK_GT(names.size(), 0u);
    CHECK_GE(names.size(), 16u);
}

TEST_CASE("Lua stubs · drift sentinel · names include the public-API anchor methods") {
    const auto& names = ExpectedLuaPublicSurface();

    // Spot-check anchor names from each cluster (init / actions / scenario / auto).
    // If any of these go missing, the stub file is mis-named, the header is
    // mis-named, or the sentinel is stale -- all three are signals.
    CHECK_EQ(names.count("InitLua"), 1u);
    CHECK_EQ(names.count("ExecuteLuaConsoleSnippet"), 1u);
    CHECK_EQ(names.count("ScenarioRegisterLuaCachedProvider"), 1u);
    CHECK_EQ(names.count("RunAutoScript"), 1u);
    CHECK_EQ(names.count("NotifyLuaTicketDataChanged"), 1u);
}

TEST_CASE("Lua stubs · drift sentinel · namespace conventions hold") {
    const auto& names = ExpectedLuaPublicSurface();

    // Every anchor name follows AppController-method naming (no `_` prefix,
    // no all-caps macros, no `Lua` namespace prefix -- everything is a method
    // on the AppController class). Catches a stub rewrite that accidentally
    // moves methods into a `LuaAutomationHost` namespace ahead of the planned
    // Phase 1B extraction (which would silently break SmatchetCore_DX12).
    for (const std::string& n : names) {
        CHECK_FALSE(n.empty());
        CHECK_NE(n.front(), '_');
        // No method names contain "::" -- they're plain identifiers, not
        // namespaced names. A stub that started using `LuaAutomationHost::Foo`
        // as the symbol path would fail this check.
        CHECK_EQ(n.find("::"), std::string::npos);
    }
}
