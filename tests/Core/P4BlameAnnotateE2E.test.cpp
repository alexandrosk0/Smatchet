// P4BlameAnnotateE2E — end-to-end exercise of `P4AnnotateFile` against the
// FakeP4Runner runner-seam fake. Slice 3 of autonomous-debugging-no-creds.
//
// Drives the real `P4Blame.cpp:P4RunCommand` → `P4AnnotateFile` parse loop via
// `BlameAnalysisConfig::P4RunOverride`. Zero subprocess spawn, zero p4 server,
// zero credentials.

#include <doctest/doctest.h>

#include "FakeP4Runner.h"
#include "P4Blame.h"

#include <string>
#include <vector>

namespace {

std::string FixturePath(const char* leaf) {
    // SMATCHET_TESTS_REPO_ROOT is wired from tests/CMakeLists.txt.
    return std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures/p4/" + leaf;
}

BlameAnalysisConfig MakeCfgFromFixture(smatchet_tests::FakeP4Runner& runner, const char* leaf) {
    runner.LoadFromFile(FixturePath(leaf));
    BlameAnalysisConfig cfg;
    cfg.P4RunOverride = runner.AsCallback();
    return cfg;
}

} // namespace

TEST_CASE("P4AnnotateFile: happy path returns one P4AnnotatedLine per source line") {
    smatchet_tests::FakeP4Runner runner;
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    std::string err;
    std::vector<P4AnnotatedLine> rows = P4AnnotateFile(cfg, "//depot/foo.cpp", "", err);
    CHECK(err.empty());
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
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    std::string err;
    std::vector<P4AnnotatedLine> rows = P4AnnotateFile(cfg, "//depot/empty.cpp", "", err);
    CHECK(err.empty());
    CHECK(rows.empty());
}

TEST_CASE("P4AnnotateFile: non-zero p4 exit surfaces error and empty rows") {
    smatchet_tests::FakeP4Runner runner;
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    std::string err;
    std::vector<P4AnnotatedLine> rows = P4AnnotateFile(cfg, "//depot/missing.cpp", "", err);
    CHECK_FALSE(err.empty());
    CHECK(rows.empty());
}

TEST_CASE("P4AnnotateFile: stdout-capped fixture still parses available lines") {
    // Fake encodes a capped stdout as a stderr note + truncated stdout. The
    // production code logs the cap but still parses whatever stdout it got.
    smatchet_tests::FakeP4Runner runner;
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    std::string err;
    std::vector<P4AnnotatedLine> rows = P4AnnotateFile(cfg, "//depot/capped.cpp", "", err);
    CHECK(err.empty());
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0].Changelist == "12348");
    CHECK(rows[0].User == "dave");
}

TEST_CASE("P4AnnotateFile: timeout (override returns false) surfaces failed-to-run error") {
    smatchet_tests::FakeP4Runner runner;
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "annotate_happy.json");

    std::string err;
    std::vector<P4AnnotatedLine> rows = P4AnnotateFile(cfg, "//depot/timeout.cpp", "", err);
    CHECK_FALSE(err.empty());
    CHECK(rows.empty());
}
