// P4AnnotateE2E — end-to-end exercise of `P4AnnotateFile` against the
// FakeP4Runner runner-seam fake. Slice 3 of autonomous-debugging-no-creds.
//
// Drives the real `P4Annotate.cpp:P4RunCommand` → `P4AnnotateFile` parse loop via
// `AnnotateAnalysisConfig::P4RunOverride`. Zero subprocess spawn, zero p4 server,
// zero credentials.

#include <doctest/doctest.h>

#include "FakeP4Runner.h"
#include "P4Annotate.h"

#include <string>
#include <vector>

namespace {

std::string FixturePath(const char* leaf) {
    // SMATCHET_TESTS_REPO_ROOT is wired from tests/CMakeLists.txt.
    return std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures/p4/" + leaf;
}

AnnotateAnalysisConfig MakeCfgFromFixture(smatchet_tests::FakeP4Runner& runner, const char* leaf) {
    runner.LoadFromFile(FixturePath(leaf));
    AnnotateAnalysisConfig cfg;
    cfg.P4RunOverride = runner.AsCallback();
    return cfg;
}

} // namespace

TEST_CASE("P4AnnotateFile: happy path returns one P4AnnotatedLine per source line") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    Result<std::vector<P4AnnotatedLine>> result = P4AnnotateFile(cfg, "//depot/foo.cpp", "");
    REQUIRE(result.has_value());
    const std::vector<P4AnnotatedLine>& rows = result.value();
    REQUIRE(rows.size() == 3u);
    CHECK(rows[0].SourceLine == 1);
    CHECK(rows[0].Changelist == "12345");
    CHECK(rows[0].User == "alice");
    CHECK(rows[0].Code == "#include <cstdio>");
    CHECK(rows[1].Changelist == "12346");
    CHECK(rows[1].User == "bob");
    CHECK(rows[2].Changelist == "12347");
    CHECK(rows[2].User == "carol");
    CHECK(runner.CallCount() == 1);
}

TEST_CASE("P4AnnotateFile: empty file produces empty row vector with no error") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    Result<std::vector<P4AnnotatedLine>> result = P4AnnotateFile(cfg, "//depot/empty.cpp", "");
    REQUIRE(result.has_value());
    CHECK(result.value().empty());
}

TEST_CASE("P4AnnotateFile: non-zero p4 exit surfaces error and empty rows") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    Result<std::vector<P4AnnotatedLine>> result = P4AnnotateFile(cfg, "//depot/missing.cpp", "");
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

TEST_CASE("P4AnnotateFile: stdout-capped fixture still parses available lines") {
    // Fake encodes a capped stdout as a stderr note + truncated stdout. The
    // production code logs the cap but still parses whatever stdout it got.
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    Result<std::vector<P4AnnotatedLine>> result = P4AnnotateFile(cfg, "//depot/capped.cpp", "");
    REQUIRE(result.has_value());
    const std::vector<P4AnnotatedLine>& rows = result.value();
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].Changelist == "12348");
    CHECK(rows[0].User == "dave");
}

TEST_CASE("P4AnnotateFile: timeout (override returns false) surfaces failed-to-run error") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    Result<std::vector<P4AnnotatedLine>> result = P4AnnotateFile(cfg, "//depot/timeout.cpp", "");
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

// The fake distinguishes a TIMEOUT (process launched, watchdog killed it: exit 124)
// from a SPAWN FAILURE (process never launched: simulate_spawn_fail / exit -1). Both
// collapse to P4RunCommand's `return false` "failed to run" shape at the production
// layer, so assert the distinction directly on the runner-seam callback.
TEST_CASE("FakeP4Runner: timeout vs spawn-fail are distinguishable at the callback seam") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");
    const AnnotateAnalysisConfig::P4RunCommandFn& cb = cfg.P4RunOverride;

    // Timeout: returns false, but preserves the distinct 124 exit code.
    {
        int exit = 999;
        std::string out, errOut;
        const bool ran = cb({"annotate", "-u", "-c", "-q", "//depot/timeout.cpp"}, exit, out, errOut);
        CHECK_FALSE(ran);
        CHECK(exit == smatchet_tests::kFakeP4TimeoutExit);
        CHECK_FALSE(errOut.empty());
    }
    // Spawn failure: returns false with the spawn-fail sentinel (-1), NOT 124.
    {
        int exit = 999;
        std::string out, errOut;
        const bool ran = cb({"annotate", "-u", "-c", "-q", "//depot/spawnfail.cpp"}, exit, out, errOut);
        CHECK_FALSE(ran);
        CHECK(exit == smatchet_tests::kFakeP4SpawnFailExit);
        CHECK(exit != smatchet_tests::kFakeP4TimeoutExit);
        CHECK_FALSE(errOut.empty());
    }
    // Non-zero COMPLETED exit (missing.cpp → exit 1): returns true so the production
    // non-zero-exit error path runs — must NOT be mistaken for a spawn-fail/timeout.
    {
        int exit = 999;
        std::string out, errOut;
        const bool ran = cb({"annotate", "-u", "-c", "-q", "//depot/missing.cpp"}, exit, out, errOut);
        CHECK(ran);
        CHECK(exit == 1);
    }
}
