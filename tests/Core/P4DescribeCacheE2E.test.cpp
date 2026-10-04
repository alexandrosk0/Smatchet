// P4DescribeCacheE2E — end-to-end exercise of `P4ChangelistDescribeCache` driven
// by the FakeP4Runner runner-seam fake. Slice 3 of autonomous-debugging-no-creds.
//
// Verifies the cache's hit/miss/eviction/retry/thread-safety contract against the real
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

// A server that is unreachable until `serverUp` flips: p4 runs and exits 1 with a connect error
// (a transient failure), then answers `p4 describe -s 7` normally.
struct FlakyDescribeServer {
    std::atomic<int> calls{0};
    std::atomic<bool> serverUp{false};
};

AnnotateAnalysisConfig MakeFlakyCfg(FlakyDescribeServer& server) {
    AnnotateAnalysisConfig cfg;
    cfg.P4RunOverride = [&server](const std::vector<std::string>&, int& outExit, std::string& outStdout,
                                  std::string& outStderr) {
        server.calls.fetch_add(1);
        if (!server.serverUp.load()) {
            outExit = 1;
            outStdout.clear();
            outStderr = "Perforce client error:\n\tConnect to server failed; check $P4PORT.\n"
                        "\tTCP connect to perforce:1666 failed.";
            return true;
        }
        outExit = 0;
        outStdout = "Change 7 by alice@workstation on 2026/01/15 14:33:00\n\n\tFix\n";
        outStderr.clear();
        return true;
    };
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

TEST_CASE("P4ChangelistDescribeCache: p4 describe failure caches an error entry") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

    P4ChangelistDescribeCache cache(/*maxEntries=*/16);
    P4ChangelistDetails d = cache.GetOrFetch(cfg, "99999");
    CHECK(d.Loaded);
    CHECK_FALSE(d.Error.empty());
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

TEST_CASE("P4ChangelistDescribeCache: a transient failure is re-fetched once the retry window passes") {
    FlakyDescribeServer server;
    AnnotateAnalysisConfig cfg = MakeFlakyCfg(server);

    P4ChangelistDescribeCache cache(/*maxEntries=*/16, std::chrono::seconds(0), P4ChangelistDescribeCache::NowFn());
    P4ChangelistDetails failed = cache.GetOrFetch(cfg, "7");
    CHECK(failed.Loaded);
    CHECK_FALSE(failed.Error.empty());
    CHECK(server.calls.load() == 1);

    server.serverUp = true;
    P4ChangelistDetails recovered = cache.GetOrFetch(cfg, "7");
    CHECK(recovered.Error.empty());
    CHECK(recovered.Author == "alice");
    CHECK(server.calls.load() == 2);

    // The successful describe is final: no further p4 call.
    CHECK(cache.GetOrFetch(cfg, "7").Author == "alice");
    CHECK(server.calls.load() == 2);
}

TEST_CASE("P4ChangelistDescribeCache: a transient failure is served from the cache inside the retry window") {
    FlakyDescribeServer server;
    AnnotateAnalysisConfig cfg = MakeFlakyCfg(server);
    P4ChangelistDescribeCache::Clock::time_point now =
        P4ChangelistDescribeCache::Clock::time_point() + std::chrono::hours(1);

    P4ChangelistDescribeCache cache(/*maxEntries=*/16, std::chrono::seconds(30), [&now]() { return now; });
    CHECK_FALSE(cache.GetOrFetch(cfg, "7").Error.empty());
    CHECK(server.calls.load() == 1);

    server.serverUp = true;
    now += std::chrono::seconds(29);
    P4ChangelistDetails held = cache.GetOrFetch(cfg, "7");
    CHECK(held.Loaded);
    CHECK_FALSE(held.Error.empty());
    CHECK(server.calls.load() == 1);

    now += std::chrono::seconds(1);
    P4ChangelistDetails recovered = cache.GetOrFetch(cfg, "7");
    CHECK(recovered.Error.empty());
    CHECK(recovered.Author == "alice");
    CHECK(server.calls.load() == 2);
}

TEST_CASE("P4ChangelistDescribeCache: 'Change N unknown' stays final even with a zero retry window") {
    smatchet_tests::FakeP4Runner runner;
    AnnotateAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

    P4ChangelistDescribeCache cache(/*maxEntries=*/16, std::chrono::seconds(0), P4ChangelistDescribeCache::NowFn());
    P4ChangelistDetails first = cache.GetOrFetch(cfg, "99999");
    CHECK(first.Loaded);
    CHECK(first.Error.find("unknown") != std::string::npos);
    P4ChangelistDetails second = cache.GetOrFetch(cfg, "99999");
    CHECK(second.Error == first.Error);
    CHECK(runner.CallCount() == 1);
}

TEST_CASE("P4ChangelistDescribeCache: a spawn failure is transient") {
    smatchet_tests::FakeP4Runner runner;
    smatchet_tests::FakeP4Response spawnFail;
    spawnFail.ArgvPrefix = "describe -s 8";
    spawnFail.SimulateSpawnFail = true;
    runner.AddResponse(spawnFail);
    AnnotateAnalysisConfig cfg;
    cfg.P4RunOverride = runner.AsCallback();

    P4ChangelistDescribeCache cache(/*maxEntries=*/16, std::chrono::seconds(0), P4ChangelistDescribeCache::NowFn());
    CHECK_FALSE(cache.GetOrFetch(cfg, "8").Error.empty());
    CHECK_FALSE(cache.GetOrFetch(cfg, "8").Error.empty());
    CHECK(runner.CallCount() == 2);
}

TEST_CASE("P4ChangelistDescribeCache: a failure never replaces a final answer stored meanwhile") {
    P4ChangelistDescribeCache cache(/*maxEntries=*/16, std::chrono::seconds(0), P4ChangelistDescribeCache::NowFn());
    AnnotateAnalysisConfig cfg;
    // While this describe is failing, a concurrent fetch for the same CL lands a final answer.
    cfg.P4RunOverride = [&cache](const std::vector<std::string>&, int& outExit, std::string&, std::string& outStderr) {
        P4ChangelistDetails ok;
        ok.Loaded = true;
        ok.Author = "bob";
        cache.Store("9", ok);
        outExit = 1;
        outStderr = "TCP connect to perforce:1666 failed.";
        return true;
    };

    P4ChangelistDetails d = cache.GetOrFetch(cfg, "9");
    CHECK(d.Error.empty());
    CHECK(d.Author == "bob");
    CHECK(cache.Get("9").Author == "bob");
}
