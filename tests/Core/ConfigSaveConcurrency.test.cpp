// ConfigSaveConcurrency tests — the config-io-safe-coalesced-writes work.
//
// Item 1 (RMW atomicity): GetConfigRmwMutexRef serializes the whole
// LoadMergedConfigJson -> modify -> WriteConfigJson transaction across the two
// smatchet_config.json writers (Save / SaveAnnotateAnalysis), so concurrent writers
// can't tear the file or deadlock (the RMW mutex is distinct from the IO mutex that
// WriteConfigJson holds internally).
//
// Item 2 (coalescing worker): smatchet::config_save Start/Enqueue*/Stop runs writes on a
// single background thread, coalescing per kind, flushing pending on Stop.
//
// Driven through TestEnvGuard which redirects ConfigManager at a private temp config dir.

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"
#include "ConfigSaveWorker.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("concurrent mixed config saves stay valid + terminate (no tear, no deadlock)" *
          doctest::test_suite("[high-risk]")) {
    smatchet_tests::TestEnvGuard env;

    // Seed both sections so the file carries tracker + annotate data from the start.
    {
        TrackerConfig seed;
        seed.TrackerType = "Jira";
        ConfigManager::Save(seed);
        AnnotateAnalysisConfig aseed;
        aseed.ChangelistCacheMaxEntries = 512;
        ConfigManager::SaveAnnotateAnalysis(aseed);
    }

    // Hammer both writers concurrently. Each is a full read-modify-write of the SAME file but a
    // DIFFERENT section — exactly the cross-section lost-update / tear scenario the RMW mutex guards.
    // Run on a worker thread with a generous deadline so a deadlock (e.g. RMW re-locking the IO mutex)
    // would surface as a timeout rather than hanging the suite.
    constexpr int kThreads = 12;
    auto hammer = [kThreads]() {
        std::vector<std::thread> ts;
        ts.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            ts.emplace_back([i]() {
                if (i % 2 == 0) {
                    TrackerConfig c;
                    c.TrackerType = "Jira";
                    c.Domain = "d" + std::to_string(100 + i);
                    ConfigManager::Save(c);
                } else {
                    AnnotateAnalysisConfig a;
                    a.ChangelistCacheMaxEntries = 100 + i; // in-range
                    ConfigManager::SaveAnnotateAnalysis(a);
                }
            });
        }
        for (auto& t : ts) {
            t.join();
        }
    };
    // Bounded deadlock guard: run on a detached thread that flips `done` when finished, then poll with
    // a deadline. If the RMW path deadlocked (e.g. re-locking the IO mutex) `done` never flips and the
    // test FAILS rather than hanging the whole suite. The shared_ptr keeps `done` alive past a
    // timeout-failure return, since the (leaked) detached thread may still hold it.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([hammer, done]() {
        hammer();
        done->store(true, std::memory_order_release);
    }).detach();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(done->load(std::memory_order_acquire)); // all writers completed → RMW path is deadlock-free

    ConfigManager::InvalidateCache();
    // No tear (both load paths parse) AND no lost section: each section reflects a WRITER-generated
    // value, not the pre-seed — proving the concurrent RMW preserved both sections.
    const TrackerConfig tc = ConfigManager::Load();
    CHECK(tc.TrackerType == "Jira");
    CHECK(tc.Domain.size() == 4); // "dNNN" from a tracker writer (even i: 100..110), not the empty seed
    CHECK(tc.Domain[0] == 'd');
    const AnnotateAnalysisConfig ac = ConfigManager::LoadAnnotateAnalysis();
    CHECK(ac.ChangelistCacheMaxEntries % 2 == 1); // odd writer value (101..111), not the even 512 seed
    CHECK(ac.ChangelistCacheMaxEntries >= 101);
    CHECK(ac.ChangelistCacheMaxEntries <= 111);
}

TEST_CASE("config_save worker persists both config kinds and flushes on Stop") {
    smatchet_tests::TestEnvGuard env;

    smatchet::config_save::Start();

    TrackerConfig tc;
    tc.TrackerType = "Plane";
    tc.Domain = "worker-domain";
    smatchet::config_save::EnqueueTrackerConfig(tc);

    AnnotateAnalysisConfig ac;
    ac.ChangelistCacheMaxEntries = 256;
    smatchet::config_save::EnqueueAnnotateConfig(ac);

    // Stop flushes pending writes within the bounded budget, then joins.
    smatchet::config_save::Stop();
    ConfigManager::InvalidateCache();

    CHECK(ConfigManager::Load().TrackerType == "Plane");
    CHECK(ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries == 256);

    // After Stop, Enqueue falls back to a synchronous save (never lost).
    AnnotateAnalysisConfig ac2;
    ac2.ChangelistCacheMaxEntries = 1024;
    smatchet::config_save::EnqueueAnnotateConfig(ac2);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries == 1024);
}
