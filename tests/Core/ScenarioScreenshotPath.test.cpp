// ScenarioScreenshotPath.test.cpp — unit coverage for the screenshot-scenario path
// confinement wrapper (Source/Core/include/Commands/Scenarios/ScenarioScreenshotPath.h).
// Every screenshot-capturing scenario (dock-gap-sentinel, command-palette-fuzzy,
// code-syntax-coloring, theme-switch-roundtrip) is a normal command and therefore
// MCP- AND Lua-reachable, so an unconfined `screenshotPath` is an arbitrary-file-write
// primitive (the #1566 class). These asserts pin the security contract that matters:
// an escaping candidate (absolute path, ".." traversal, empty) is rejected fail-closed
// with a stable, scenario-prefixed message — behaviour that holds independent of the
// runtime <userData> base, which is why it is unit-testable on the doctest rig.

#include "Commands/Scenarios/ScenarioScreenshotPath.h"

#include <doctest/doctest.h>

#include <string>

namespace {

using smatchet::cmd::ConfineScenarioScreenshotPath;
using smatchet::cmd::ConfineScenarioScreenshotPathInPlace;

} // namespace

TEST_CASE("ScenarioScreenshotPath · escapes are rejected fail-closed with a scenario-prefixed message") {
    std::string err;

    SUBCASE("absolute candidate is rejected") {
#ifdef _WIN32
        std::string p = "C:/Windows/System32/evil.png";
#else
        std::string p = "/etc/evil.png";
#endif
        CHECK_FALSE(ConfineScenarioScreenshotPathInPlace("dock-gap-sentinel", p, err));
        CHECK(err.find("dock-gap-sentinel") != std::string::npos);
        CHECK(err.find("screenshotPath rejected") != std::string::npos);
        // fail-closed: the caller's path is left untouched (not silently rewritten).
#ifdef _WIN32
        CHECK(p == "C:/Windows/System32/evil.png");
#else
        CHECK(p == "/etc/evil.png");
#endif
    }

    SUBCASE("'..' traversal is rejected") {
        std::string p = "../../escape.png";
        CHECK_FALSE(ConfineScenarioScreenshotPathInPlace("theme-switch-roundtrip", p, err));
        CHECK(err.find("theme-switch-roundtrip") != std::string::npos);
        CHECK(err.find("screenshotPath rejected") != std::string::npos);
    }

    SUBCASE("empty candidate is rejected") {
        std::string p;
        CHECK_FALSE(ConfineScenarioScreenshotPathInPlace("code-syntax-coloring", p, err));
        CHECK(err.find("code-syntax-coloring") != std::string::npos);
    }
}

TEST_CASE("ScenarioScreenshotPath · a plain relative basename resolves inside the screenshots subdir") {
    std::string resolved, err;
    const bool ok = ConfineScenarioScreenshotPath("shot.png", resolved, err);
    // GetUserDataDirectory() resolves to a real directory at runtime; on accept the
    // resolved path sits under .../screenshots/ and keeps the leaf name. If the userData
    // dir is unavailable in the bare test environment the wrapper must still fail CLOSED
    // (reject with a non-empty error) rather than accept — pin whichever branch applies so
    // the security invariant (never accept an escape / never accept without a base) holds.
    if (ok) {
        CHECK(err.empty());
        CHECK(resolved.find("screenshots") != std::string::npos);
        CHECK(resolved.find("shot.png") != std::string::npos);
    } else {
        CHECK_FALSE(err.empty());
    }
}
