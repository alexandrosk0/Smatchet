// ProbeScenario / ProbeScope coverage — the lambda-driven programmatic scenario
// and its stack-scoped register/unregister RAII. Drives only the AppController-free
// surface (Name / IsDone / factory lifecycle): OnStart/OnFrame/OnFinish take an
// AppController& and are exercised by the integration scenario rig, not here.

#include "Commands/Scenarios/ProbeScenario.h"

#include "Commands/Scenarios/IScenario.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using smatchet::cmd::IScenario;
using smatchet::cmd::MakeProbeScenario;
using smatchet::cmd::ProbeScope;
using smatchet::cmd::ScenarioRunner;

namespace {
bool Contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

TEST_CASE("MakeProbeScenario reports its name and frame-count IsDone boundary") {
    std::unique_ptr<IScenario> sc = MakeProbeScenario("probe-x", nullptr, nullptr, 3);
    REQUIRE(sc != nullptr);
    CHECK(sc->Name() == "probe-x");

    // IsDone is frameIndex >= frames (3). The runner increments frame_ before
    // calling IsDone, so frames 1/2/3 -> not done until the count is reached.
    CHECK_FALSE(sc->IsDone(0));
    CHECK_FALSE(sc->IsDone(2));
    CHECK(sc->IsDone(3));
    CHECK(sc->IsDone(4));
}

TEST_CASE("MakeProbeScenario clamps a non-positive frame count to at least 1") {
    std::unique_ptr<IScenario> sc = MakeProbeScenario("probe-zero", nullptr, nullptr, 0);
    REQUIRE(sc != nullptr);
    CHECK_FALSE(sc->IsDone(0));
    CHECK(sc->IsDone(1));
}

TEST_CASE("ProbeScope registers its factory for the scope and unregisters on exit") {
    ScenarioRunner runner;
    CHECK_FALSE(Contains(runner.ListNames(), "scoped-probe"));

    {
        ProbeScope scope(runner, "scoped-probe", [](int) {}, nullptr, 5);
        CHECK(scope.Name() == "scoped-probe");
        CHECK(Contains(runner.ListNames(), "scoped-probe"));

        // The factory builds a working ProbeScenario each time the runner would start it.
        // (We can't call the private factory directly, but ListNames proves it is installed.)
    }

    // Scope exit must have removed the factory — back to the pre-scope state.
    CHECK_FALSE(Contains(runner.ListNames(), "scoped-probe"));
}

TEST_CASE("ProbeScope nested scopes register independently") {
    ScenarioRunner runner;
    {
        ProbeScope a(runner, "probe-a", [](int) {});
        {
            ProbeScope b(runner, "probe-b", [](int) {});
            CHECK(Contains(runner.ListNames(), "probe-a"));
            CHECK(Contains(runner.ListNames(), "probe-b"));
        }
        // Inner scope gone; outer survives.
        CHECK(Contains(runner.ListNames(), "probe-a"));
        CHECK_FALSE(Contains(runner.ListNames(), "probe-b"));
    }
    CHECK_FALSE(Contains(runner.ListNames(), "probe-a"));
}

TEST_CASE("ProbeScope with a colliding name leaves the pre-existing factory intact on destruction") {
    ScenarioRunner runner;

    // A factory installed by someone else (not a ProbeScope) under the same name.
    runner.RegisterFactory("collide", []() -> std::unique_ptr<IScenario> {
        return MakeProbeScenario("pre-existing", nullptr, nullptr, 1);
    });
    REQUIRE(Contains(runner.ListNames(), "collide"));

    {
        // ProbeScope hits the name collision: it must NOT take ownership of the
        // entry, so its dtor must NOT erase the pre-existing factory.
        ProbeScope scope(runner, "collide", [](int) {}, nullptr, 5);
        CHECK(scope.Name() == "collide");
        CHECK(Contains(runner.ListNames(), "collide"));
    }

    // Regression guard for the RAII bug: an unconditional dtor UnregisterFactory
    // would have erased the factory this scope never created.
    CHECK(Contains(runner.ListNames(), "collide"));
    // And it is still erasable exactly once — proving a single live entry remains
    // (no double-register, no double-erase).
    CHECK(runner.UnregisterFactory("collide") == 1u);
    CHECK_FALSE(Contains(runner.ListNames(), "collide"));
}

TEST_CASE("ScenarioRunner::UnregisterFactory returns the erased count") {
    ScenarioRunner runner;
    runner.RegisterFactory(
        "manual", []() -> std::unique_ptr<IScenario> { return MakeProbeScenario("manual", nullptr, nullptr, 1); });
    CHECK(runner.UnregisterFactory("manual") == 1u);
    CHECK(runner.UnregisterFactory("manual") == 0u); // already gone
    CHECK(runner.UnregisterFactory("never-registered") == 0u);
}
