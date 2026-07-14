// P4ChangeQueriesResult — exercises the Result<T> migration of the two P4
// changelist queries (build-quality-velocity-hardening #21, non-tracker tier):
//   * P4ChangesForUser                        -> Result<std::vector<P4ChangeSummary>>
//   * P4FirstSubmittedChangelistOnCalendarDay -> Result<std::string>
//
// Drives P4Annotate.cpp's P4RunCommand path via the FakeP4Runner runner-seam
// (AnnotateAnalysisConfig::P4RunOverride) — no subprocess spawn, no p4 server,
// no credentials. Mirrors the fixture-free inline-AddResponse style so each case
// scripts exactly the argv it expects. These two functions were untested before
// the Result<T> migration; the cases below lock in the ok/err/empty semantics.

#include <doctest/doctest.h>

#include "FakeP4Runner.h"
#include "P4Annotate.h"

#include <string>
#include <vector>

namespace {

AnnotateAnalysisConfig MakeCfg(smatchet_tests::FakeP4Runner& runner) {
    AnnotateAnalysisConfig cfg;
    cfg.P4RunOverride = runner.AsCallback();
    return cfg;
}

smatchet_tests::FakeP4Response Response(const std::string& argvPrefix, int exitCode, const std::string& out,
                                        const std::string& err = std::string()) {
    smatchet_tests::FakeP4Response r;
    r.ArgvPrefix = argvPrefix;
    r.ExitCode = exitCode;
    r.Stdout = out;
    r.Stderr = err;
    return r;
}

} // namespace

TEST_CASE("P4ChangesForUser: parses submitted changes into Ok(vector), newest first, domain stripped") {
    smatchet_tests::FakeP4Runner runner;
    runner.AddResponse(Response("changes -u alice", 0,
                                "Change 12347 on 2026/07/12 by alice@ws 'Third change'\n"
                                "Change 12346 on 2026/07/11 by alice@ws 'Second change'\n"
                                "Change 12345 on 2026/07/10 by alice@ws 'First change'\n"));
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::vector<P4ChangeSummary>> result = P4ChangesForUser(cfg, "alice", 5);
    REQUIRE(result.has_value());
    const std::vector<P4ChangeSummary>& changes = result.value();
    REQUIRE(changes.size() == 3u);
    CHECK(changes[0].Changelist == "12347");
    CHECK(changes[0].Date == "2026/07/12");
    CHECK(changes[0].User == "alice"); // @ws domain stripped
    CHECK(changes[0].Description == "Third change");
    CHECK(changes[2].Changelist == "12345");
    CHECK(runner.CallCount() == 1);
}

TEST_CASE("P4ChangesForUser: empty user is Err without invoking p4") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::vector<P4ChangeSummary>> result = P4ChangesForUser(cfg, "", 5);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == "empty p4 user");
    CHECK(runner.CallCount() == 0);
}

TEST_CASE("P4ChangesForUser: no visible changes is Ok(empty vector), not an error") {
    smatchet_tests::FakeP4Runner runner;
    runner.AddResponse(Response("changes -u ghost", 0, ""));
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::vector<P4ChangeSummary>> result = P4ChangesForUser(cfg, "ghost", 5);
    REQUIRE(result.has_value());
    CHECK(result.value().empty());
    CHECK(runner.CallCount() == 1);
}

TEST_CASE("P4ChangesForUser: non-zero p4 exit surfaces an Err") {
    smatchet_tests::FakeP4Runner runner;
    runner.AddResponse(Response("changes -u alice", 2, "", "p4: connection refused"));
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::vector<P4ChangeSummary>> result = P4ChangesForUser(cfg, "alice", 5);
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

TEST_CASE("P4ChangesForUser: spawn failure surfaces the failed-to-run Err") {
    smatchet_tests::FakeP4Runner runner;
    smatchet_tests::FakeP4Response r = Response("changes -u alice", 0, "");
    r.SimulateSpawnFail = true;
    runner.AddResponse(r);
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::vector<P4ChangeSummary>> result = P4ChangesForUser(cfg, "alice", 5);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == "failed to run p4");
}

TEST_CASE("P4FirstSubmittedChangelistOnCalendarDay: parses the changelist into Ok(string)") {
    smatchet_tests::FakeP4Runner runner;
    runner.AddResponse(
        Response("changes -r -m 1 -s submitted", 0, "Change 45678 on 2026/07/14 by bob@ws 'Landed something'\n"));
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::string> result = P4FirstSubmittedChangelistOnCalendarDay(cfg, 2026, 7, 14);
    REQUIRE(result.has_value());
    CHECK(result.value() == "45678");
    CHECK(runner.CallCount() == 1);
}

TEST_CASE("P4FirstSubmittedChangelistOnCalendarDay: invalid date is Err before any p4 call") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::string> result = P4FirstSubmittedChangelistOnCalendarDay(cfg, 2026, 13, 40);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == "invalid calendar date");
    CHECK(runner.CallCount() == 0);
}

TEST_CASE("P4FirstSubmittedChangelistOnCalendarDay: whitespace-only p4 output is a no-changes Err") {
    smatchet_tests::FakeP4Runner runner;
    runner.AddResponse(Response("changes -r -m 1 -s submitted", 0, "   \n"));
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::string> result = P4FirstSubmittedChangelistOnCalendarDay(cfg, 2026, 7, 14);
    CHECK_FALSE(result.has_value());
    CHECK(result.error() == "no submitted changelists on that calendar day (or no visibility to //...@)");
}

TEST_CASE("P4FirstSubmittedChangelistOnCalendarDay: non-zero p4 exit surfaces an Err") {
    smatchet_tests::FakeP4Runner runner;
    runner.AddResponse(Response("changes -r -m 1 -s submitted", 2, "", "p4: protocol error"));
    AnnotateAnalysisConfig cfg = MakeCfg(runner);

    Result<std::string> result = P4FirstSubmittedChangelistOnCalendarDay(cfg, 2026, 7, 14);
    CHECK_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}
