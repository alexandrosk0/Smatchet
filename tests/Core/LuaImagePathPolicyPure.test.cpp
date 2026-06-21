// Pure unit coverage of the Lua imgui.image allowlist policy (LuaImagePathPolicyPure.h).
//
// SECURITY: the Lua `imgui.image(path)` binding must refuse http(s) URLs so a console script cannot
// drive an arbitrary outbound HTTP GET (SSRF / exfil / tracking pixel). Only non-http(s) local asset
// paths are permitted. Header-only predicate; no production .cpp to link.

#include "LuaImagePathPolicyPure.h"

#include <doctest/doctest.h>

TEST_CASE("imgui.image refuses http(s) URLs") {
    CHECK_FALSE(LuaImagePathPolicyPure::IsAllowedLuaImagePath("http://example.com/x.png"));
    CHECK_FALSE(LuaImagePathPolicyPure::IsAllowedLuaImagePath("https://example.com/x.png"));
    CHECK(LuaImagePathPolicyPure::IsHttpUrl("http://example.com/x.png"));
    CHECK(LuaImagePathPolicyPure::IsHttpUrl("https://example.com/x.png"));
}

TEST_CASE("imgui.image http(s) refusal is case-insensitive and tolerates leading whitespace") {
    // Scheme-case bypass attempt.
    CHECK_FALSE(LuaImagePathPolicyPure::IsAllowedLuaImagePath("HTTP://evil/x.png"));
    CHECK_FALSE(LuaImagePathPolicyPure::IsAllowedLuaImagePath("HtTpS://evil/x.png"));
    // Leading-whitespace bypass attempt.
    CHECK_FALSE(LuaImagePathPolicyPure::IsAllowedLuaImagePath("   https://evil/x.png"));
    CHECK_FALSE(LuaImagePathPolicyPure::IsAllowedLuaImagePath("\t\nhttp://evil/x.png"));
}

TEST_CASE("imgui.image permits local asset paths") {
    CHECK(LuaImagePathPolicyPure::IsAllowedLuaImagePath("Scripts/art/icon.png"));
    CHECK(LuaImagePathPolicyPure::IsAllowedLuaImagePath("C:/Dev/assets/icon.png"));
    CHECK(LuaImagePathPolicyPure::IsAllowedLuaImagePath("/usr/share/icon.png"));
    CHECK(LuaImagePathPolicyPure::IsAllowedLuaImagePath("./relative/icon.png"));
    // A path that merely CONTAINS http later (not as a scheme) is still a local path.
    CHECK(LuaImagePathPolicyPure::IsAllowedLuaImagePath("Scripts/http_icon.png"));
}

TEST_CASE("imgui.image policy handles empty and null safely") {
    CHECK(LuaImagePathPolicyPure::IsAllowedLuaImagePath(""));
    CHECK(LuaImagePathPolicyPure::IsAllowedLuaImagePath(nullptr));
    CHECK_FALSE(LuaImagePathPolicyPure::IsHttpUrl(nullptr));
    CHECK_FALSE(LuaImagePathPolicyPure::IsHttpUrl(""));
}
