// P4DescribeCacheE2E — end-to-end exercise of `P4ChangelistDescribeCache` driven
// by the FakeP4Runner runner-seam fake. Slice 3 of autonomous-debugging-no-creds.
//
// Verifies the cache's hit/miss/eviction/thread-safety contract against the real
// `p4 describe -s`-fed `GetOrFetch` path — without spawning the binary.

#include <doctest/doctest.h>

#include "FakeP4Runner.h"
#include "P4Annotate.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string FixturePath(const char* leaf) {
    return std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures/p4/" + leaf;
}

AnnotateAnalysisConfig MakeCfgFromFixture(smatchet_tests::FakeP4Runner& runner, const char* leaf) {
    runner.LoadFromFile(FixturePath(leaf));
    AnnotateAnalysisConfig cfg;
    cfg.P4RunOverride = runner.AsCallback();
    return cfg;
}

} // namespace

TEST_CASE("P4ChangelistDescribeCache: miss then hit re-uses canned describe output") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

    P4ChangelistDescribeCache cache(/*maxEntries=*/16);
    P4ChangelistDetails d1 = cache.GetOrFetch(cfg, "1");
    CHECK(d1.Loaded);
    CHECK(d1.Author == "alice");
    CHECK_FALSE(d1.Date.empty());
    CHECK(runner.CallCount() == 1);

    // Second call hits the cache — the override is not invoked again.
    P4ChangelistDetails d2 = cache.GetOrFetch(cfg, "1");
    CHECK(d2.Loaded);
    CHECK(d2.Author == "alice");
    CHECK(runner.CallCount() == 1);
}

TEST_CASE("P4ChangelistDescribeCache: eviction past maxEntries drops the oldest entry") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

    P4ChangelistDescribeCache cache(/*maxEntries=*/2);
    cache.GetOrFetch(cfg, "1");
    cache.GetOrFetch(cfg, "2");
    cache.GetOrFetch(cfg, "3"); // evicts "1"
    CHECK(runner.CallCount() == 3);

    // Re-fetching "1" should re-invoke the override (cache miss after eviction).
    cache.GetOrFetch(cfg, "1");
    CHECK(runner.CallCount() == 4);
    // "3" is still cached (touched most recently) — no extra call.
    cache.GetOrFetch(cfg, "3");
    CHECK(runner.CallCount() == 4);
}

TEST_CASE("P4ChangelistDescribeCache: a changelist p4 says is unknown is remembered for good") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

    P4ChangelistDescribeCache cache(/*maxEntries=*/16, /*failureRetryAfter=*/std::chrono::seconds(0));
    P4ChangelistDetails d = cache.GetOrFetch(cfg, "99999");
    CHECK(d.Loaded);
    CHECK_FALSE(d.Error.empty());
    CHECK(runner.CallCount() == 1);
    // Final even with no backoff at all: hovering a changelist that does not exist costs no more p4 calls.
    CHECK_FALSE(cache.GetOrFetch(cfg, "99999").Error.empty());
    CHECK(runner.CallCount() == 1);
}

TEST_CASE("P4ChangelistDescribeCache: an unreachable server is asked again once the backoff passes") {
    smatchet_tests::FakeP4Runner down;
    smatchet_tests::FakeP4Response unreachable;
    unreachable.ArgvPrefix = "describe -s 1";
    unreachable.ExitCode = 1;
    unreachable.Stderr = "Perforce client error:\n\tConnect to server failed; check $P4PORT.";
    down.AddResponse(unreachable);
    AnnotateAnalysisConfig cfgDown;
    cfgDown.P4RunOverride = down.AsCallback();
    smatchet_tests::FakeP4Runner up;
    AnnotateAnalysisConfig cfgUp = MakeCfgFromFixture(up, "describe_cache.json");

    // Inside the backoff the failure is answered from the cache: no p4 call per hover.
    P4ChangelistDescribeCache held(/*maxEntries=*/16, /*failureRetryAfter=*/std::chrono::seconds(600));
    CHECK_FALSE(held.GetOrFetch(cfgDown, "1").Error.empty());
    CHECK_FALSE(held.GetOrFetch(cfgUp, "1").Error.empty());
    CHECK(down.CallCount() == 1);
    CHECK(up.CallCount() == 0);

    // Once it has passed, the next lookup runs p4 again; the success is then final.
    P4ChangelistDescribeCache retried(/*maxEntries=*/16, /*failureRetryAfter=*/std::chrono::seconds(0));
    CHECK_FALSE(retried.GetOrFetch(cfgDown, "1").Error.empty());
    const P4ChangelistDetails d = retried.GetOrFetch(cfgUp, "1");
    CHECK(d.Error.empty());
    CHECK(d.Author == "alice");
    CHECK(up.CallCount() == 1);
    CHECK(retried.GetOrFetch(cfgUp, "1").Author == "alice");
    CHECK(up.CallCount() == 1);
}

TEST_CASE("P4ChangelistDescribeCache: a describe that could not run at all is retried too") {
    smatchet_tests::FakeP4Runner runner;
    smatchet_tests::FakeP4Response spawnFail;
    spawnFail.ArgvPrefix = "describe -s 2";
    spawnFail.SimulateSpawnFail = true;
    runner.AddResponse(spawnFail);
    AnnotateAnalysisConfig cfg;
    cfg.P4RunOverride = runner.AsCallback();

    P4ChangelistDescribeCache cache(/*maxEntries=*/16, /*failureRetryAfter=*/std::chrono::seconds(0));
    CHECK_FALSE(cache.GetOrFetch(cfg, "2").Error.empty());
    CHECK_FALSE(cache.GetOrFetch(cfg, "2").Error.empty());
    CHECK(runner.CallCount() == 2);
}

TEST_CASE("P4ChangelistDescribeCache: two threads asking for the same CL converge") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

    P4ChangelistDescribeCache cache(/*maxEntries=*/16);
    std::atomic<int> aliceSeen{0};
    auto worker = [&]() {
        for (int i = 0; i < 50; ++i) {
            P4ChangelistDetails d = cache.GetOrFetch(cfg, "2");
            if (d.Author == "bob") {
                aliceSeen.fetch_add(1);
            }
        }
    };

    std::thread t1(worker);
    std::thread t2(worker);
    t1.join();
    t2.join();

    // Both threads must observe the same canonical value.
    CHECK(aliceSeen.load() == 100);
    // The cache may have raced and called the runner more than once (no
    // single-flight in production today) but must not crash, and the canonical
    // value must remain stable.
    CHECK(runner.CallCount() >= 1);
}

TEST_CASE("P4ChangelistDescribeCache: a failure never replaces a success another lookup stored meanwhile") {
    P4ChangelistDescribeCache cache(/*maxEntries=*/16, /*failureRetryAfter=*/std::chrono::seconds(0));
    // This lookup's p4 run fails, but while it runs a concurrent lookup of the same CL stores a success.
    AnnotateAnalysisConfig cfg;
    cfg.P4RunOverride = [&cache](const std::vector<std::string>&, int& outExit, std::string& outStdout,
                                 std::string& outStderr) -> bool {
        P4ChangelistDetails success;
        success.Loaded = true;
        success.Author = "alice";
        cache.Store("7", success);
        outExit = 1;
        outStdout.clear();
        outStderr = "Perforce client error:\n\tConnect to server failed; check $P4PORT.";
        return true;
    };
    CHECK_FALSE(cache.GetOrFetch(cfg, "7").Error.empty()); // this caller still sees its own failure
    const P4ChangelistDetails kept = cache.Get("7");
    CHECK(kept.Error.empty());
    CHECK(kept.Author == "alice");
}

TEST_CASE("P4ChangelistDescribeCache: an unknown-changelist answer never replaces a success stored meanwhile") {
    P4ChangelistDescribeCache cache(/*maxEntries=*/16, /*failureRetryAfter=*/std::chrono::seconds(0));
    // The failing run's answer is final ("no such changelist"), but a concurrent lookup stored a success.
    AnnotateAnalysisConfig cfg;
    cfg.P4RunOverride = [&cache](const std::vector<std::string>&, int& outExit, std::string& outStdout,
                                 std::string& outStderr) -> bool {
        P4ChangelistDetails success;
        success.Loaded = true;
        success.Author = "alice";
        cache.Store("7", success);
        outExit = 1;
        outStdout.clear();
        outStderr = "Change 7 unknown.";
        return true;
    };
    CHECK_FALSE(cache.GetOrFetch(cfg, "7").Error.empty());
    const P4ChangelistDetails kept = cache.Get("7");
    CHECK(kept.Error.empty());
    CHECK(kept.Author == "alice");
}
