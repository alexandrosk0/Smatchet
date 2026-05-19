// PrCheckRunWatcher_CiDispatch.test.cpp — phase-7 watcher tests that pin
// the contract between PrCheckRunWatcher's Dispatch verdict and the
// open-PR respawn dispatcher seam. Mirrors PrCommentWatcher_ShortCircuitReject's
// shape — focus on the seam, not on the classifier internals (those are
// already covered in CiFailureClassifier.test.cpp).
//
// Covers:
//   - Dispatch verdict → respawn dispatcher invoked with (prUrl,
//     dispatchSource, dispatchTargetAgent, run).
//   - dispatchSource carries the classifier's ci_* discriminator.
//   - dispatchTargetAgent matches the routed sub-delegate the classifier
//     returned.
//   - Multiple failed runs on one PR each dispatch (or stop on budget).
//   - Cursor (lastSeenCheckRunId) advances after a successful dispatch.
//   - When the respawn dispatcher returns false, the watcher logs but
//     advances the cursor (the run is already past).
//   - When the respawn dispatcher is null, the watcher LOG_WARNs once.

#if defined(SMATCHET_WITH_AGENTIC)

#include "PrCheckRunWatcher.h"

#include "AgentProposalStore.h"
#include "GitHubClient.h"
#include "PrCheckRunClassifier.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using ::smatchet::agentic::CheckRunClassification;
using ::smatchet::agentic::CheckRunVerdict;
using ::smatchet::agentic::PrCheckRunClassifier;
using ::smatchet::agentic::PrCheckRunWatcher;

namespace {

void SeedRow(AgentProposalStore& store, const std::string& prUrl, int prNumber, const std::string& headSha) {
    AgentProposalStore::OpenPrWatchRow row;
    row.prUrl = prUrl;
    row.prNumber = prNumber;
    row.headRefName = "feature/branch";
    row.headSha = headSha;
    row.lastSeenCommentIdStr = "";
    row.lastSeenCheckRunId = 0;
    row.iterationCount = 0;
    row.lastPolledAtSec = 0;
    std::string err;
    REQUIRE(store.SetOpenPrWatch(row, err));
}

GitHubClient::CheckRun MakeRun(std::int64_t id, const std::string& name, const std::string& conclusion) {
    GitHubClient::CheckRun r;
    r.id = id;
    r.name = name;
    r.status = "completed";
    r.conclusion = conclusion;
    r.detailsUrl = "https://github.com/o/r/actions/runs/" + std::to_string(id) + "/job/1";
    return r;
}

class CheckRunFetcher {
  public:
    bool operator()(const std::string& owner, const std::string& repo, const std::string& headSha,
                    std::vector<GitHubClient::CheckRun>& outRuns, std::string& outError) {
        (void)owner;
        (void)repo;
        (void)outError;
        auto it = bySha.find(headSha);
        if (it != bySha.end()) {
            outRuns = it->second;
        } else {
            outRuns.clear();
        }
        return true;
    }
    std::map<std::string, std::vector<GitHubClient::CheckRun>> bySha;
};

class CaptureRespawn {
  public:
    bool operator()(const std::string& prUrl, const std::string& dispatchSource, const std::string& target,
                    const GitHubClient::CheckRun& run, std::string& outError) {
        (void)outError;
        if (returnsFalse) {
            outError = "scripted respawn failure";
            return false;
        }
        Call c;
        c.prUrl = prUrl;
        c.dispatchSource = dispatchSource;
        c.target = target;
        c.runId = run.id;
        c.runName = run.name;
        c.detailsUrl = run.detailsUrl;
        fired.push_back(c);
        return true;
    }
    struct Call {
        std::string prUrl;
        std::string dispatchSource;
        std::string target;
        std::int64_t runId = 0;
        std::string runName;
        std::string detailsUrl;
    };
    std::vector<Call> fired;
    bool returnsFalse = false;
};

// Scripted classifier — pulls the next verdict off a queue. Falls through
// to Skip when drained.
class ScriptedClassifier : public PrCheckRunClassifier {
  public:
    CheckRunClassification Classify(const GitHubClient::CheckRun& run,
                                    const std::vector<GitHubClient::CheckRunAnnotation>&,
                                    const std::string&) const override {
        ++calls;
        seen.push_back(run.id);
        if (queue.empty()) {
            CheckRunClassification c;
            c.verdict = CheckRunVerdict::Skip;
            c.skipReason = "no scripted verdict";
            return c;
        }
        auto r = queue.front();
        queue.erase(queue.begin());
        return r;
    }
    mutable int calls = 0;
    mutable std::vector<std::int64_t> seen;
    mutable std::vector<CheckRunClassification> queue;
};

CheckRunClassification MakeDispatch(const std::string& source, const std::string& target) {
    CheckRunClassification c;
    c.verdict = CheckRunVerdict::Dispatch;
    c.dispatchSource = source;
    c.dispatchTargetAgent = target;
    return c;
}

} // namespace

TEST_SUITE("PrCheckRunWatcher::CiDispatch") {

TEST_CASE("Dispatch verdict → respawn dispatcher called with classifier-routed source + target") {
    AgentProposalStore store(":memory:");
    SeedRow(store, "https://github.com/team/repo/pull/100", 100, "sha100");

    CheckRunFetcher fetcher;
    fetcher.bySha["sha100"] = {MakeRun(900, "Windows + MSYS2 UCRT64", "failure")};

    CaptureRespawn respawn;

    auto classifier = std::unique_ptr<ScriptedClassifier>(new ScriptedClassifier());
    classifier->queue.push_back(MakeDispatch("ci_build_failure", "build-doctor"));

    PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
    watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                    std::vector<GitHubClient::CheckRun>& runs, std::string& e) {
        return fetcher(o, r, s, runs, e);
    });
    watcher.SetCheckRunRespawnDispatcher(
        [&](const std::string& url, const std::string& src, const std::string& tgt,
            const GitHubClient::CheckRun& run, std::string& e) { return respawn(url, src, tgt, run, e); });
    watcher.SetClassifier(std::move(classifier));

    const int n = watcher.Tick();
    CHECK(n == 1);
    REQUIRE(respawn.fired.size() == 1);
    CHECK(respawn.fired[0].prUrl == "https://github.com/team/repo/pull/100");
    CHECK(respawn.fired[0].dispatchSource == "ci_build_failure");
    CHECK(respawn.fired[0].target == "build-doctor");
    CHECK(respawn.fired[0].runId == 900);
    CHECK(respawn.fired[0].runName == "Windows + MSYS2 UCRT64");

    // Cursor advanced.
    AgentProposalStore::OpenPrWatchRow row;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/100", row, err));
    CHECK(row.lastSeenCheckRunId == 900);
    CHECK(row.iterationCount == 1);
}

TEST_CASE("Dispatch routes each ci_* source to its named delegate") {
    AgentProposalStore store(":memory:");
    SeedRow(store, "https://github.com/team/repo/pull/200", 200, "sha200");
    SeedRow(store, "https://github.com/team/repo/pull/201", 201, "sha201");
    SeedRow(store, "https://github.com/team/repo/pull/202", 202, "sha202");

    CheckRunFetcher fetcher;
    fetcher.bySha["sha200"] = {MakeRun(2001, "build", "failure")};
    fetcher.bySha["sha201"] = {MakeRun(2002, "ctest", "failure")};
    fetcher.bySha["sha202"] = {MakeRun(2003, "coverage-gate", "failure")};

    CaptureRespawn respawn;

    auto classifier = std::unique_ptr<ScriptedClassifier>(new ScriptedClassifier());
    classifier->queue.push_back(MakeDispatch("ci_build_failure", "build-doctor"));
    classifier->queue.push_back(MakeDispatch("ci_ctest_failure", "debug-detective"));
    classifier->queue.push_back(MakeDispatch("ci_coverage_gate", "test-rig"));

    PrCheckRunWatcher watcher(&store, 5, 2);
    watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                    std::vector<GitHubClient::CheckRun>& runs, std::string& e) {
        return fetcher(o, r, s, runs, e);
    });
    watcher.SetCheckRunRespawnDispatcher(
        [&](const std::string& url, const std::string& src, const std::string& tgt,
            const GitHubClient::CheckRun& run, std::string& e) { return respawn(url, src, tgt, run, e); });
    watcher.SetClassifier(std::move(classifier));

    const int n = watcher.Tick();
    CHECK(n == 3);
    REQUIRE(respawn.fired.size() == 3);

    // Map results by run id so order doesn't matter for the assertion.
    std::map<std::int64_t, CaptureRespawn::Call> byId;
    for (const auto& c : respawn.fired) {
        byId[c.runId] = c;
    }
    REQUIRE(byId.size() == 3);
    CHECK(byId[2001].dispatchSource == "ci_build_failure");
    CHECK(byId[2001].target == "build-doctor");
    CHECK(byId[2002].dispatchSource == "ci_ctest_failure");
    CHECK(byId[2002].target == "debug-detective");
    CHECK(byId[2003].dispatchSource == "ci_coverage_gate");
    CHECK(byId[2003].target == "test-rig");
}

TEST_CASE("Multiple failed runs on one PR each dispatch") {
    AgentProposalStore store(":memory:");
    SeedRow(store, "https://github.com/team/repo/pull/300", 300, "sha300");

    CheckRunFetcher fetcher;
    fetcher.bySha["sha300"] = {
        MakeRun(3001, "build", "failure"),
        MakeRun(3002, "ctest", "failure"),
    };

    CaptureRespawn respawn;
    auto classifier = std::unique_ptr<ScriptedClassifier>(new ScriptedClassifier());
    classifier->queue.push_back(MakeDispatch("ci_build_failure", "build-doctor"));
    classifier->queue.push_back(MakeDispatch("ci_ctest_failure", "debug-detective"));

    PrCheckRunWatcher watcher(&store, 5, 2);
    watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                    std::vector<GitHubClient::CheckRun>& runs, std::string& e) {
        return fetcher(o, r, s, runs, e);
    });
    watcher.SetCheckRunRespawnDispatcher(
        [&](const std::string& url, const std::string& src, const std::string& tgt,
            const GitHubClient::CheckRun& run, std::string& e) { return respawn(url, src, tgt, run, e); });
    watcher.SetClassifier(std::move(classifier));

    const int n = watcher.Tick();
    CHECK(n == 2);
    REQUIRE(respawn.fired.size() == 2);

    AgentProposalStore::OpenPrWatchRow row;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/300", row, err));
    CHECK(row.iterationCount == 2);
    CHECK(row.lastSeenCheckRunId == 3002);
}

TEST_CASE("Budget exhaustion halts further dispatches mid-walk") {
    AgentProposalStore store(":memory:");
    SeedRow(store, "https://github.com/team/repo/pull/400", 400, "sha400");

    CheckRunFetcher fetcher;
    fetcher.bySha["sha400"] = {
        MakeRun(4001, "build", "failure"),
        MakeRun(4002, "ctest", "failure"),
        MakeRun(4003, "coverage-gate", "failure"),
    };

    CaptureRespawn respawn;
    auto classifier = std::unique_ptr<ScriptedClassifier>(new ScriptedClassifier());
    classifier->queue.push_back(MakeDispatch("ci_build_failure", "build-doctor"));
    classifier->queue.push_back(MakeDispatch("ci_ctest_failure", "debug-detective"));
    classifier->queue.push_back(MakeDispatch("ci_coverage_gate", "test-rig"));

    // Budget = 2 — third dispatch must NOT fire.
    PrCheckRunWatcher watcher(&store, 2, 2);
    watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                    std::vector<GitHubClient::CheckRun>& runs, std::string& e) {
        return fetcher(o, r, s, runs, e);
    });
    watcher.SetCheckRunRespawnDispatcher(
        [&](const std::string& url, const std::string& src, const std::string& tgt,
            const GitHubClient::CheckRun& run, std::string& e) { return respawn(url, src, tgt, run, e); });
    watcher.SetClassifier(std::move(classifier));

    const int n = watcher.Tick();
    CHECK(n == 2);
    CHECK(respawn.fired.size() == 2);
}

TEST_CASE("Respawn dispatcher returning false logs but advances cursor") {
    AgentProposalStore store(":memory:");
    SeedRow(store, "https://github.com/team/repo/pull/500", 500, "sha500");

    CheckRunFetcher fetcher;
    fetcher.bySha["sha500"] = {MakeRun(5001, "build", "failure")};

    CaptureRespawn respawn;
    respawn.returnsFalse = true;

    auto classifier = std::unique_ptr<ScriptedClassifier>(new ScriptedClassifier());
    classifier->queue.push_back(MakeDispatch("ci_build_failure", "build-doctor"));

    PrCheckRunWatcher watcher(&store, 5, 2);
    watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                    std::vector<GitHubClient::CheckRun>& runs, std::string& e) {
        return fetcher(o, r, s, runs, e);
    });
    watcher.SetCheckRunRespawnDispatcher(
        [&](const std::string& url, const std::string& src, const std::string& tgt,
            const GitHubClient::CheckRun& run, std::string& e) { return respawn(url, src, tgt, run, e); });
    watcher.SetClassifier(std::move(classifier));

    const int n = watcher.Tick();
    CHECK(n == 0); // Dispatch failure does NOT count as actioned.

    // Cursor STILL advanced (watcher pre-flights cursor update before
    // dispatcher call). Re-fire of the same run is the watcher's
    // documented trade-off; preventing it required atomic store + dispatcher
    // semantics phase-6 did not implement.
    AgentProposalStore::OpenPrWatchRow row;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/500", row, err));
    CHECK(row.lastSeenCheckRunId == 5001);
}

TEST_CASE("No respawn dispatcher wired → Dispatch verdict logs only") {
    AgentProposalStore store(":memory:");
    SeedRow(store, "https://github.com/team/repo/pull/600", 600, "sha600");

    CheckRunFetcher fetcher;
    fetcher.bySha["sha600"] = {MakeRun(6001, "build", "failure")};

    auto classifier = std::unique_ptr<ScriptedClassifier>(new ScriptedClassifier());
    classifier->queue.push_back(MakeDispatch("ci_build_failure", "build-doctor"));

    PrCheckRunWatcher watcher(&store, 5, 2);
    watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                    std::vector<GitHubClient::CheckRun>& runs, std::string& e) {
        return fetcher(o, r, s, runs, e);
    });
    // No respawn dispatcher wired.
    watcher.SetClassifier(std::move(classifier));

    const int n = watcher.Tick();
    CHECK(n == 0);

    AgentProposalStore::OpenPrWatchRow row;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/600", row, err));
    // Cursor still advanced (watcher pre-flights it).
    CHECK(row.lastSeenCheckRunId == 6001);
    // iterationCount NOT bumped on a null-dispatcher Dispatch verdict.
    CHECK(row.iterationCount == 0);
}

TEST_CASE("Skip verdict bypasses respawn dispatcher entirely") {
    AgentProposalStore store(":memory:");
    SeedRow(store, "https://github.com/team/repo/pull/700", 700, "sha700");

    CheckRunFetcher fetcher;
    fetcher.bySha["sha700"] = {MakeRun(7001, "build", "failure")};

    CaptureRespawn respawn;
    auto classifier = std::unique_ptr<ScriptedClassifier>(new ScriptedClassifier());
    CheckRunClassification skip;
    skip.verdict = CheckRunVerdict::Skip;
    skip.skipReason = "advisory check, ignored";
    classifier->queue.push_back(skip);

    PrCheckRunWatcher watcher(&store, 5, 2);
    watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                    std::vector<GitHubClient::CheckRun>& runs, std::string& e) {
        return fetcher(o, r, s, runs, e);
    });
    watcher.SetCheckRunRespawnDispatcher(
        [&](const std::string& url, const std::string& src, const std::string& tgt,
            const GitHubClient::CheckRun& run, std::string& e) { return respawn(url, src, tgt, run, e); });
    watcher.SetClassifier(std::move(classifier));

    const int n = watcher.Tick();
    CHECK(n == 0);
    CHECK(respawn.fired.empty());

    AgentProposalStore::OpenPrWatchRow row;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/700", row, err));
    CHECK(row.lastSeenCheckRunId == 7001);
    CHECK(row.iterationCount == 0);
}

} // TEST_SUITE PrCheckRunWatcher::CiDispatch

#endif // SMATCHET_WITH_AGENTIC
