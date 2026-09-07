// LuaScriptsRootPure.test.cpp — issue #2144 regression.
//
// The Lua scripts root has to be answerable BEFORE AppController::Initialize latches
// luaScriptsDirectory_, because plugin OnEarlyInit resolves "Automation.lua" /
// "SmatchetHooks.lua" first. Resolving off the member alone returned empty there, and the
// LuaConsole plugin cached that empty result for the session: the editor opened blank and the
// script picker showed none of the .lua files that were on disk the whole time.
//
// These cases pin the three answers the derivation owes its callers: the latched value wins once
// it exists, the runtime asset directory covers the pre-Initialize window, and an unconfigured
// install resolves to EMPTY — never a cwd-relative "Scripts/", which would surface an untrusted
// working directory's .lua files for execution.
//
// Header-only pure decision (Source/Core/include/LuaScriptsRootPure.h); no production .cpp to
// link.

#include <doctest/doctest.h>

#include "LuaScriptsRootPure.h"

#include <string>

using smatchet::lua_scripts::ResolveScriptsRoot;

TEST_CASE("ResolveScriptsRoot returns the latched directory once Initialize has assigned it") {
    CHECK(ResolveScriptsRoot("D:/app/Scripts/", "D:/app/") == "D:/app/Scripts/");
}

TEST_CASE("ResolveScriptsRoot prefers the latched directory over the asset directory") {
    // A latched root that disagrees with the current asset directory (portable install moved,
    // Initialize re-run against a different base) must not be silently re-derived underneath the
    // caller — the member is the authority once set.
    CHECK(ResolveScriptsRoot("E:/portable/Scripts/", "D:/app/") == "E:/portable/Scripts/");
}

TEST_CASE("ResolveScriptsRoot derives the root before Initialize latches it (#2144)") {
    // The OnEarlyInit window: member still empty, host has already published the asset directory.
    CHECK(ResolveScriptsRoot("", "D:/app/") == "D:/app/Scripts/");
    CHECK(ResolveScriptsRoot("", "/opt/smatchet/") == "/opt/smatchet/Scripts/");
}

TEST_CASE("ResolveScriptsRoot fails closed with no configured directory at all") {
    // Empty, NOT a cwd-relative "Scripts/": every caller (path resolution, script enumeration)
    // refuses on an empty root, which is what keeps an untrusted working directory's .lua files
    // out of the picker and out of the Lua loader.
    CHECK(ResolveScriptsRoot("", "").empty());
}
