// P4DescribeCacheE2E — end-to-end exercise of `P4ChangelistDescribeCache` driven
// by the FakeP4Runner runner-seam fake. Slice 3 of autonomous-debugging-no-creds.
//
// Verifies the cache's hit/miss/eviction/thread-safety contract against the real
// `p4 describe -s`-fed `GetOrFetch` path — without spawning the binary.

#include <doctest/doctest.h>

#include "FakeP4Runner.h"
#include "P4Blame.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string FixturePath(const char* leaf) {
    return std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures/p4/" + leaf;
}

BlameAnalysisConfig MakeCfgFromFixture(smatchet_tests::FakeP4Runner& runner, const char* leaf) {
    runner.LoadFromFile(FixturePath(leaf));
    BlameAnalysisConfig cfg;
    cfg.P4RunOverride = runner.AsCallback();
    return cfg;
}

} // namespace

TEST_CASE("P4ChangelistDescribeCache: miss then hit re-uses canned describe output") {
    smatchet_tests::FakeP4Runner runner;
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

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
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

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
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

    P4ChangelistDescribeCache cache(/*maxEntries=*/16);
    P4ChangelistDetails d = cache.GetOrFetch(cfg, "99999");
    CHECK(d.Loaded);
    CHECK_FALSE(d.Error.empty());
}

TEST_CASE("P4ChangelistDescribeCache: two threads asking for the same CL converge") {
    smatchet_tests::FakeP4Runner runner;
    BlameAnalysisConfig cfg = MakeCfgFromFixture(runner, "describe_cache.json");

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
