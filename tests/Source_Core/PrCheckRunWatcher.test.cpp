// PrCheckRunWatcher.test.cpp — phase-6 of the `coderabbit-react-loop` plan.
// Exercises the Tick() loop with scripted CheckRun fetcher, annotation
// fetcher, log-tail fetcher, rerun dispatcher, and respawn dispatcher
// against a real in-memory AgentProposalStore.

#if defined(SMATCHET_WITH_AGENTIC)

#include "PrCheckRunWatcher.h"

#include "AgentProposalStore.h"
#include "CiFailureClassifier.h"
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
using ::smatchet::agentic::CiFailureClassifier;
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

GitHubClient::CheckRun MakeRun(std::int64_t id, const std::string& name, const std::string& conclusion,
                                const std::string& detailsUrl = "") {
    GitHubClient::CheckRun r;
    r.id = id;
    r.name = name;
    r.status = "completed";
    r.conclusion = conclusion;
    r.detailsUrl = detailsUrl.empty()
                       ? ("https://github.com/o/r/actions/runs/" + std::to_string(id) + "/job/1")
                       : detailsUrl;
    return r;
}

class ScriptedCheckRunFetcher {
  public:
    bool operator()(const std::string& owner, const std::string& repo, const std::string& headSha,
                    std::vector<GitHubClient::CheckRun>& outRuns, std::string& outError) {
        (void)outError;
        ++callCount;
        seen.push_back({owner, repo, headSha});
        auto it = bySha.find(headSha);
        if (it != bySha.end()) {
            outRuns = it->second;
        } else {
            outRuns.clear();
        }
        return true;
    }
    int callCount = 0;
    struct Call {
        std::string owner;
        std::string repo;
        std::string headSha;
    };
    std::vector<Call> seen;
    std::map<std::string, std::vector<GitHubClient::CheckRun>> bySha;
};

class ScriptedAnnotationFetcher {
  public:
    bool operator()(const std::string& owner, const std::string& repo, std::int64_t id,
                    std::vector<GitHubClient::CheckRunAnnotation>& outAnnot, std::string& outError) {
        (void)owner;
        (void)repo;
        (void)outError;
        ++callCount;
        auto it = byId.find(id);
        if (it != byId.end()) {
            outAnnot = it->second;
        } else {
            outAnnot.clear();
        }
        return true;
    }
    int callCount = 0;
    std::map<std::int64_t, std::vector<GitHubClient::CheckRunAnnotation>> byId;
};

class ScriptedRerunDispatcher {
  public:
    bool operator()(const std::string& owner, const std::string& repo, std::int64_t runId, std::string& outError) {
        (void)outError;
        fired.push_back({owner, repo, runId});
        return true;
    }
    struct Call {
        std::string owner;
        std::string repo;
        std::int64_t runId;
    };
    std::vector<Call> fired;
};

class ScriptedRespawnDispatcher {
  public:
    bool operator()(const std::string& prUrl, const std::string& dispatchSource, const std::string& target,
                    const GitHubClient::CheckRun& run, std::string& outError) {
        (void)outError;
        fired.push_back({prUrl, dispatchSource, target, run.id});
        return true;
    }
    struct Call {
        std::string prUrl;
        std::string dispatchSource;
        std::string target;
        std::int64_t runId;
    };
    std::vector<Call> fired;
};

GitHubClient::CheckRunAnnotation MakeAnnot(const std::string& msg) {
    GitHubClient::CheckRunAnnotation a;
    a.message = msg;
    a.annotationLevel = "failure";
    return a;
}

} // namespace

TEST_SUITE("PrCheckRunWatcher") {

    TEST_CASE("Tick: dispatcher invoked per row + cursor advanced") {
        AgentProposalStore store(":memory:");
        SeedRow(store, "https://github.com/team/repo/pull/1", 1, "aaa1");
        SeedRow(store, "https://github.com/team/repo/pull/2", 2, "bbb2");

        ScriptedCheckRunFetcher fetcher;
        fetcher.bySha["aaa1"] = {MakeRun(101, "Windows + MSYS2 UCRT64", "failure")};
        fetcher.bySha["bbb2"] = {MakeRun(202, "Windows + MSYS2 UCRT64", "failure")};

        ScriptedAnnotationFetcher annotFetcher;
        annotFetcher.byId[101] = {MakeAnnot("CMake Error at CMakeLists.txt:1")};
        annotFetcher.byId[202] = {MakeAnnot("CMake Error at CMakeLists.txt:2")};

        ScriptedRespawnDispatcher respawn;
        ScriptedRerunDispatcher rerun;

        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetAnnotationFetcher([&](const std::string& o, const std::string& r, std::int64_t id,
                                          std::vector<GitHubClient::CheckRunAnnotation>& out,
                                          std::string& e) { return annotFetcher(o, r, id, out, e); });
        watcher.SetCheckRunRespawnDispatcher([&](const std::string& url, const std::string& src, const std::string& tgt,
                                                const GitHubClient::CheckRun& run, std::string& e) {
            return respawn(url, src, tgt, run, e);
        });
        watcher.SetWorkflowRerunDispatcher([&](const std::string& o, const std::string& r, std::int64_t id,
                                                std::string& e) { return rerun(o, r, id, e); });
        watcher.SetClassifier(std::unique_ptr<PrCheckRunClassifier>(new CiFailureClassifier()));

        const int n = watcher.Tick();
        CHECK(n == 2);
        REQUIRE(respawn.fired.size() == 2);
        CHECK(respawn.fired[0].target == "build-doctor");
        CHECK(respawn.fired[0].dispatchSource == "ci_build_failure");
        CHECK(respawn.fired[1].target == "build-doctor");
        CHECK(rerun.fired.empty());

        // Cursors advanced + iteration counts bumped.
        AgentProposalStore::OpenPrWatchRow r;
        std::string err;
        REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/1", r, err));
        CHECK(r.lastSeenCheckRunId == 101);
        CHECK(r.iterationCount == 1);
        REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/2", r, err));
        CHECK(r.lastSeenCheckRunId == 202);
        CHECK(r.iterationCount == 1);
    }

    TEST_CASE("Tick: only failed runs are classified; success runs are ignored") {
        AgentProposalStore store(":memory:");
        SeedRow(store, "https://github.com/team/repo/pull/3", 3, "ccc3");

        ScriptedCheckRunFetcher fetcher;
        fetcher.bySha["ccc3"] = {
            MakeRun(300, "Windows + MSYS2 UCRT64", "success"),
            MakeRun(301, "Test-delta gate", "success"),
            MakeRun(302, "Windows + MSYS2 UCRT64", "failure"),
        };

        ScriptedAnnotationFetcher annotFetcher;
        annotFetcher.byId[302] = {MakeAnnot("CMake Error at CMakeLists.txt:5")};

        ScriptedRespawnDispatcher respawn;
        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetAnnotationFetcher([&](const std::string& o, const std::string& r, std::int64_t id,
                                          std::vector<GitHubClient::CheckRunAnnotation>& out,
                                          std::string& e) { return annotFetcher(o, r, id, out, e); });
        watcher.SetCheckRunRespawnDispatcher([&](const std::string& url, const std::string& src, const std::string& tgt,
                                                const GitHubClient::CheckRun& run, std::string& e) {
            return respawn(url, src, tgt, run, e);
        });
        watcher.SetClassifier(std::unique_ptr<PrCheckRunClassifier>(new CiFailureClassifier()));

        const int n = watcher.Tick();
        CHECK(n == 1);
        REQUIRE(respawn.fired.size() == 1);
        CHECK(respawn.fired[0].runId == 302);
        AgentProposalStore::OpenPrWatchRow r;
        std::string err;
        REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/3", r, err));
        // Cursor only advanced past the failed run.
        CHECK(r.lastSeenCheckRunId == 302);
    }

    TEST_CASE("Tick: transient flake fires rerun + cap eventually trips") {
        AgentProposalStore store(":memory:");
        const std::string prUrl = "https://github.com/team/repo/pull/4";
        SeedRow(store, prUrl, 4, "ddd4");

        ScriptedCheckRunFetcher fetcher;
        ScriptedAnnotationFetcher annotFetcher;
        ScriptedRerunDispatcher rerun;

        PrCheckRunWatcher watcher(&store, /*budget*/ 99, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetAnnotationFetcher([&](const std::string& o, const std::string& r, std::int64_t id,
                                          std::vector<GitHubClient::CheckRunAnnotation>& out,
                                          std::string& e) { return annotFetcher(o, r, id, out, e); });
        watcher.SetWorkflowRerunDispatcher([&](const std::string& o, const std::string& r, std::int64_t id,
                                                std::string& e) { return rerun(o, r, id, e); });
        watcher.SetClassifier(std::unique_ptr<PrCheckRunClassifier>(new CiFailureClassifier()));

        // Three successive transient flakes on the same PR. With rerun-cap=2,
        // the third should hit the cap and skip the rerun.
        const std::vector<std::int64_t> ids = {500, 501, 502};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::int64_t id = ids[i];
            fetcher.bySha["ddd4"] = {MakeRun(id, "Windows + MSYS2 UCRT64", "failure")};
            annotFetcher.byId[id] = {MakeAnnot("curl: (28) Operation timed out")};
            const int n = watcher.Tick();
            if (i < 2) {
                CHECK(n == 1);
            } else {
                CHECK(n == 0); // cap-exceeded
            }
        }
        // Only the first two reruns fired.
        CHECK(rerun.fired.size() == 2);
        CHECK(rerun.fired[0].runId == 500);
        CHECK(rerun.fired[1].runId == 501);
        CHECK(watcher.GetRerunCount(prUrl) == 2);

        // Cursor advanced past every processed run regardless of cap status.
        AgentProposalStore::OpenPrWatchRow r;
        std::string err;
        REQUIRE(store.GetOpenPrWatch(prUrl, r, err));
        CHECK(r.lastSeenCheckRunId == 502);
    }

    TEST_CASE("Tick: coverage-advisory rows skip without dispatch") {
        AgentProposalStore store(":memory:");
        SeedRow(store, "https://github.com/team/repo/pull/5", 5, "eee5");

        ScriptedCheckRunFetcher fetcher;
        fetcher.bySha["eee5"] = {MakeRun(700, "Coverage (windows-2022 + OpenCppCoverage)", "failure")};

        ScriptedRespawnDispatcher respawn;
        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetCheckRunRespawnDispatcher([&](const std::string& url, const std::string& src, const std::string& tgt,
                                                const GitHubClient::CheckRun& run, std::string& e) {
            return respawn(url, src, tgt, run, e);
        });
        watcher.SetClassifier(std::unique_ptr<PrCheckRunClassifier>(new CiFailureClassifier()));

        const int n = watcher.Tick();
        CHECK(n == 0);
        CHECK(respawn.fired.empty());
        AgentProposalStore::OpenPrWatchRow r;
        std::string err;
        REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/5", r, err));
        // Cursor still advanced so the same run is not re-classified.
        CHECK(r.lastSeenCheckRunId == 700);
        CHECK(r.iterationCount == 0);
    }

    TEST_CASE("Tick: unknown check-run name skips without dispatch") {
        AgentProposalStore store(":memory:");
        SeedRow(store, "https://github.com/team/repo/pull/6", 6, "fff6");

        ScriptedCheckRunFetcher fetcher;
        fetcher.bySha["fff6"] = {MakeRun(801, "Future build matrix variant", "failure")};

        ScriptedRespawnDispatcher respawn;
        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetCheckRunRespawnDispatcher([&](const std::string& url, const std::string& src, const std::string& tgt,
                                                const GitHubClient::CheckRun& run, std::string& e) {
            return respawn(url, src, tgt, run, e);
        });
        watcher.SetClassifier(std::unique_ptr<PrCheckRunClassifier>(new CiFailureClassifier()));

        const int n = watcher.Tick();
        CHECK(n == 0);
        CHECK(respawn.fired.empty());
        AgentProposalStore::OpenPrWatchRow r;
        std::string err;
        REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/6", r, err));
        CHECK(r.lastSeenCheckRunId == 801);
    }

    TEST_CASE("Tick: no classifier registered → skip every run but cursor advances") {
        AgentProposalStore store(":memory:");
        SeedRow(store, "https://github.com/team/repo/pull/7", 7, "ggg7");

        ScriptedCheckRunFetcher fetcher;
        fetcher.bySha["ggg7"] = {
            MakeRun(900, "Windows + MSYS2 UCRT64", "failure"),
            MakeRun(901, "Windows + MSYS2 UCRT64", "failure"),
        };

        ScriptedRespawnDispatcher respawn;
        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetCheckRunRespawnDispatcher([&](const std::string& url, const std::string& src, const std::string& tgt,
                                                const GitHubClient::CheckRun& run, std::string& e) {
            return respawn(url, src, tgt, run, e);
        });
        // No classifier set.

        const int n = watcher.Tick();
        CHECK(n == 0);
        CHECK(respawn.fired.empty());
        AgentProposalStore::OpenPrWatchRow r;
        std::string err;
        REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/7", r, err));
        CHECK(r.lastSeenCheckRunId == 901);
    }

    TEST_CASE("Tick: no fetcher → no-op") {
        AgentProposalStore store(":memory:");
        SeedRow(store, "https://github.com/team/repo/pull/8", 8, "hhh8");
        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        CHECK(watcher.Tick() == 0);
    }

    TEST_CASE("Tick: empty head_sha row is skipped + logged") {
        AgentProposalStore store(":memory:");
        // Hand-build a row with an empty head_sha to verify the watcher
        // doesn't crash + skips cleanly.
        AgentProposalStore::OpenPrWatchRow row;
        row.prUrl = "https://github.com/team/repo/pull/9";
        row.prNumber = 9;
        row.headRefName = "feature/x";
        row.headSha = "";
        std::string err;
        REQUIRE(store.SetOpenPrWatch(row, err));

        ScriptedCheckRunFetcher fetcher;
        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetClassifier(std::unique_ptr<PrCheckRunClassifier>(new CiFailureClassifier()));
        CHECK(watcher.Tick() == 0);
        // The fetcher was never called because the head_sha gate fired
        // first.
        CHECK(fetcher.callCount == 0);
    }

    TEST_CASE("Tick: iteration budget pre-flight blocks dispatch") {
        AgentProposalStore store(":memory:");
        const std::string prUrl = "https://github.com/team/repo/pull/10";
        // Seed with iteration_count at the budget.
        AgentProposalStore::OpenPrWatchRow row;
        row.prUrl = prUrl;
        row.prNumber = 10;
        row.headRefName = "feature/x";
        row.headSha = "iii10";
        row.iterationCount = 5;
        std::string err;
        REQUIRE(store.SetOpenPrWatch(row, err));

        ScriptedCheckRunFetcher fetcher;
        fetcher.bySha["iii10"] = {MakeRun(1010, "Windows + MSYS2 UCRT64", "failure")};
        ScriptedAnnotationFetcher annotFetcher;
        annotFetcher.byId[1010] = {MakeAnnot("CMake Error at CMakeLists.txt:1")};
        ScriptedRespawnDispatcher respawn;

        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetAnnotationFetcher([&](const std::string& o, const std::string& r, std::int64_t id,
                                          std::vector<GitHubClient::CheckRunAnnotation>& out,
                                          std::string& e) { return annotFetcher(o, r, id, out, e); });
        watcher.SetCheckRunRespawnDispatcher([&](const std::string& url, const std::string& src, const std::string& tgt,
                                                const GitHubClient::CheckRun& run, std::string& e) {
            return respawn(url, src, tgt, run, e);
        });
        watcher.SetClassifier(std::unique_ptr<PrCheckRunClassifier>(new CiFailureClassifier()));

        const int n = watcher.Tick();
        CHECK(n == 0);
        CHECK(respawn.fired.empty());
        // Cursor NOT advanced when budget gate trips — the run is deferred,
        // not skipped.
        AgentProposalStore::OpenPrWatchRow after;
        REQUIRE(store.GetOpenPrWatch(prUrl, after, err));
        CHECK(after.lastSeenCheckRunId == 0);
    }

    TEST_CASE("Tick: second tick on identical fetch is a no-op (cursor idempotency)") {
        AgentProposalStore store(":memory:");
        SeedRow(store, "https://github.com/team/repo/pull/11", 11, "jjj11");

        ScriptedCheckRunFetcher fetcher;
        fetcher.bySha["jjj11"] = {MakeRun(1101, "Windows + MSYS2 UCRT64", "failure")};
        ScriptedAnnotationFetcher annotFetcher;
        annotFetcher.byId[1101] = {MakeAnnot("CMake Error at CMakeLists.txt:1")};
        ScriptedRespawnDispatcher respawn;

        PrCheckRunWatcher watcher(&store, /*budget*/ 5, /*rerunCap*/ 2);
        watcher.SetCheckRunFetcher([&](const std::string& o, const std::string& r, const std::string& s,
                                        std::vector<GitHubClient::CheckRun>& runs,
                                        std::string& e) { return fetcher(o, r, s, runs, e); });
        watcher.SetAnnotationFetcher([&](const std::string& o, const std::string& r, std::int64_t id,
                                          std::vector<GitHubClient::CheckRunAnnotation>& out,
                                          std::string& e) { return annotFetcher(o, r, id, out, e); });
        watcher.SetCheckRunRespawnDispatcher([&](const std::string& url, const std::string& src, const std::string& tgt,
                                                const GitHubClient::CheckRun& run, std::string& e) {
            return respawn(url, src, tgt, run, e);
        });
        watcher.SetClassifier(std::unique_ptr<PrCheckRunClassifier>(new CiFailureClassifier()));

        CHECK(watcher.Tick() == 1);
        CHECK(respawn.fired.size() == 1);
        // Second tick with the same fetcher state.
        CHECK(watcher.Tick() == 0);
        CHECK(respawn.fired.size() == 1);
    }

    // ─── ParseOwnerRepoFromPrUrl static helper ───────────────────────────

    TEST_CASE("ParseOwnerRepoFromPrUrl: typical PR URL") {
        std::string o, r, e;
        CHECK(PrCheckRunWatcher::ParseOwnerRepoFromPrUrl("https://github.com/foo/bar/pull/42", o, r, e));
        CHECK(o == "foo");
        CHECK(r == "bar");
        CHECK(e.empty());
    }

    TEST_CASE("ParseOwnerRepoFromPrUrl: with query string") {
        std::string o, r, e;
        CHECK(PrCheckRunWatcher::ParseOwnerRepoFromPrUrl("https://github.com/foo/bar/pull/42?diff=split", o, r, e));
        CHECK(o == "foo");
        CHECK(r == "bar");
    }

    TEST_CASE("ParseOwnerRepoFromPrUrl: missing /pull/ → false") {
        std::string o, r, e;
        CHECK_FALSE(PrCheckRunWatcher::ParseOwnerRepoFromPrUrl("https://github.com/foo/bar/issues/42", o, r, e));
        CHECK_FALSE(e.empty());
    }

    TEST_CASE("ParseOwnerRepoFromPrUrl: empty URL → false") {
        std::string o, r, e;
        CHECK_FALSE(PrCheckRunWatcher::ParseOwnerRepoFromPrUrl("", o, r, e));
        CHECK_FALSE(e.empty());
    }

    // ─── Clamp behaviour ─────────────────────────────────────────────────

    TEST_CASE("budget clamps to [1, 50]") {
        AgentProposalStore store(":memory:");
        PrCheckRunWatcher w1(&store, /*budget*/ 0, /*rerunCap*/ 1);
        CHECK(w1.GetIterationBudget() == 1);
        PrCheckRunWatcher w2(&store, /*budget*/ 1000, /*rerunCap*/ 1);
        CHECK(w2.GetIterationBudget() == 50);
        w2.SetIterationBudget(-5);
        CHECK(w2.GetIterationBudget() == 1);
    }

    TEST_CASE("rerun cap clamps to [0, 10]") {
        AgentProposalStore store(":memory:");
        PrCheckRunWatcher w1(&store, /*budget*/ 5, /*rerunCap*/ -1);
        CHECK(w1.GetTransientRerunCap() == 0);
        PrCheckRunWatcher w2(&store, /*budget*/ 5, /*rerunCap*/ 100);
        CHECK(w2.GetTransientRerunCap() == 10);
        w2.SetTransientRerunCap(3);
        CHECK(w2.GetTransientRerunCap() == 3);
    }
}

#endif // SMATCHET_WITH_AGENTIC
