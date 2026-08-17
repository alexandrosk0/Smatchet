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
#include "Config/TrackerConfigSaveRepair.h"

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

// #2026 — the persistent-views slot. Views::Save used to do this whole read-merge-write inline on
// the UI thread on every view activation (i.e. every pane show/hide); it now enqueues here. Two
// properties matter and both are asserted through the production symbol:
//   1. the snapshot reaches disk (worker path, flushed by Stop);
//   2. the DR13a out-of-band ToolbarAppend merge still happens — it moved onto the worker with
//      the write, so a toolbar button written straight to disk must survive the rewrite.
TEST_CASE("config_save worker persists the views file and keeps the out-of-band ToolbarAppend") {
    smatchet_tests::TestEnvGuard env;

    // Out-of-band writer (the toolbar editor's tracker-scope save): ToolbarAppend only.
    {
        PersistentViewsFile onDisk;
        onDisk.Version = 2;
        ToolbarButton btn;
        btn.CommandId = "view.refresh";
        onDisk.Backends["Jira"].ToolbarAppend.push_back(btn);
        ConfigManager::SavePersistentViewsToDisk(onDisk);
    }

    smatchet::config_save::Start();

    // The Views wrapper's snapshot: owns Views / ActiveViewId, and knows nothing about the
    // toolbar write above (its in-memory Disk predates it).
    PersistentViewsFile snapshot;
    snapshot.Version = 2;
    ViewDefinition v;
    v.Id = "worker-view";
    v.Name = "Worker View";
    v.Jql = "project = W";
    snapshot.Backends["Jira"].ActiveViewId = "worker-view";
    snapshot.Backends["Jira"].Views.push_back(v);
    smatchet::config_save::EnqueuePersistentViews(snapshot);

    smatchet::config_save::Stop(); // flush pending within budget, then join

    const PersistentViewsFile reloaded = ConfigManager::LoadPersistentViewsFromDisk();
    REQUIRE(reloaded.Backends.count("Jira") == 1);
    const ViewWorkspaceState& jira = reloaded.Backends.at("Jira");
    CHECK(jira.ActiveViewId == "worker-view");
    REQUIRE(jira.Views.size() == 1);
    CHECK(jira.Views[0].Id == "worker-view");
    // DR13a: the out-of-band toolbar button was folded back in by the worker's re-read.
    REQUIRE(jira.ToolbarAppend.size() == 1);
    CHECK(jira.ToolbarAppend[0].CommandId == "view.refresh");
}

TEST_CASE("EnqueuePersistentViews falls back to a synchronous write when the worker is stopped") {
    smatchet_tests::TestEnvGuard env;

    // Worker never started (tests / CLI / pre-init / post-shutdown): the write must still land,
    // otherwise a views change made during teardown would be silently dropped.
    PersistentViewsFile snapshot;
    snapshot.Version = 2;
    ViewDefinition v;
    v.Id = "sync-view";
    v.Name = "Sync View";
    snapshot.Backends["Jira"].ActiveViewId = "sync-view";
    snapshot.Backends["Jira"].Views.push_back(v);
    smatchet::config_save::EnqueuePersistentViews(snapshot);

    const PersistentViewsFile reloaded = ConfigManager::LoadPersistentViewsFromDisk();
    REQUIRE(reloaded.Backends.count("Jira") == 1);
    CHECK(reloaded.Backends.at("Jira").ActiveViewId == "sync-view");
    REQUIRE(reloaded.Backends.at("Jira").Views.size() == 1);
    CHECK(reloaded.Backends.at("Jira").Views[0].Id == "sync-view");
}

// --- Persisted-field repair hooks (Issue #2047) -------------------------------------------------
//
// A screenshot-capture scenario clears persisted TrackerConfig fields (GitHubPat above all) for the
// duration of a capture and unwinds them a frame LATER, so a config save landing inside that window
// — or a process exiting with the unwind still queued — used to write the cleared credential to the
// user's real config. The repair hook makes that write unissuable at ConfigManager::Save, the seam
// every TrackerConfig write in the process funnels through.

TEST_CASE("repair hook rewrites a DIRECT ConfigManager::Save — not just the worker's enqueue") {
    smatchet_tests::TestEnvGuard env;

    // This is the path that matters most and the one a worker-only repair misses entirely: the UI
    // has dozens of direct `ConfigManager::Save(g_ui.cfg)` call sites (the update modal's "Skip This
    // Version", the layout-schema migration, the preferences debounce, ...) and any of them firing
    // inside a capture window would otherwise persist the scenario's cleared PAT.
    const int token = smatchet::config_repair::RegisterTrackerConfigRepair(
        [](TrackerConfig& cfg) { cfg.GitHubPat = "user-real-pat"; });
    REQUIRE(token != 0);

    TrackerConfig cleared; // what a capture scenario leaves on the live config
    cleared.TrackerType = "Jira";
    cleared.Domain = "direct-save-domain";
    cleared.GitHubPat.clear();
    ConfigManager::Save(cleared);

    ConfigManager::InvalidateCache();
    const TrackerConfig onDisk = ConfigManager::Load();
    // The pinned credential survives...
    CHECK(onDisk.GitHubPat == "user-real-pat");
    // ...while every other edit in the same save is written normally.
    CHECK(onDisk.Domain == "direct-save-domain");

    smatchet::config_repair::UnregisterTrackerConfigRepair(token);

    // Once unregistered the caller's value is authoritative again — the repair never outlives the
    // window, so a user who really does clear their PAT can still do so.
    TrackerConfig later;
    later.TrackerType = "Jira";
    later.GitHubPat = "pat-the-user-typed";
    ConfigManager::Save(later);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().GitHubPat == "pat-the-user-typed");
}

TEST_CASE("repair hook also rewrites the coalescing worker's outgoing snapshot") {
    smatchet_tests::TestEnvGuard env;

    // Worker not running -> EnqueueTrackerConfig saves synchronously on this thread, which is the
    // path a process tearing down actually takes.
    const int token = smatchet::config_repair::RegisterTrackerConfigRepair(
        [](TrackerConfig& cfg) { cfg.GitHubPat = "user-real-pat"; });
    REQUIRE(token != 0);

    TrackerConfig cleared;
    cleared.TrackerType = "Jira";
    cleared.Domain = "repair-domain";
    cleared.GitHubPat.clear();
    smatchet::config_save::EnqueueTrackerConfig(cleared);

    ConfigManager::InvalidateCache();
    const TrackerConfig onDisk = ConfigManager::Load();
    CHECK(onDisk.GitHubPat == "user-real-pat");
    CHECK(onDisk.Domain == "repair-domain");

    smatchet::config_repair::UnregisterTrackerConfigRepair(token);
}

TEST_CASE("enqueue-time repair survives an owner that unregisters before the queue drains") {
    // The reason EnqueueTrackerConfig repairs on the way IN rather than relying on the Save
    // chokepoint alone: the coalescing slot can be filled inside the pin window and drained after
    // the scenario has unwound, by which point Save would see an empty registry.
    const int token = smatchet::config_repair::RegisterTrackerConfigRepair(
        [](TrackerConfig& cfg) { cfg.GitHubPat = "user-real-pat"; });

    TrackerConfig cleared;
    cleared.GitHubPat.clear();
    smatchet::config_repair::ApplyTrackerConfigRepairs(cleared); // what enqueue does to the snapshot

    smatchet::config_repair::UnregisterTrackerConfigRepair(token); // scenario unwinds

    // The snapshot already carries the user's value, so the later (hook-free) write is harmless.
    CHECK(cleared.GitHubPat == "user-real-pat");
}

TEST_CASE("ApplyTrackerConfigRepairs is a no-op with no hooks and stacks independent owners") {
    TrackerConfig cfg;
    cfg.GitHubPat = "untouched";
    smatchet::config_repair::ApplyTrackerConfigRepairs(cfg);
    CHECK(cfg.GitHubPat == "untouched");

    // Two independent owners (a capture scenario + the shared capture-quiesce) pin disjoint fields
    // over overlapping windows, so both hooks must survive and both must run.
    const int a =
        smatchet::config_repair::RegisterTrackerConfigRepair([](TrackerConfig& c) { c.GitHubPat = "from-a"; });
    const int b =
        smatchet::config_repair::RegisterTrackerConfigRepair([](TrackerConfig& c) { c.UpdateCheckEnabled = true; });

    TrackerConfig both;
    both.GitHubPat.clear();
    both.UpdateCheckEnabled = false;
    smatchet::config_repair::ApplyTrackerConfigRepairs(both);
    CHECK(both.GitHubPat == "from-a");
    CHECK(both.UpdateCheckEnabled == true);

    // Dropping one leaves the other armed — the two owners unwind independently.
    smatchet::config_repair::UnregisterTrackerConfigRepair(a);
    TrackerConfig onlyB;
    onlyB.GitHubPat.clear();
    onlyB.UpdateCheckEnabled = false;
    smatchet::config_repair::ApplyTrackerConfigRepairs(onlyB);
    CHECK(onlyB.GitHubPat.empty());
    CHECK(onlyB.UpdateCheckEnabled == true);

    smatchet::config_repair::UnregisterTrackerConfigRepair(b);
    // Idempotent: an already-dropped token (and token 0) is a no-op, so an owner can unregister
    // unconditionally from its unwind path.
    smatchet::config_repair::UnregisterTrackerConfigRepair(b);
    smatchet::config_repair::UnregisterTrackerConfigRepair(0);
    // An empty std::function registers nothing and yields the no-op token.
    CHECK(smatchet::config_repair::RegisterTrackerConfigRepair(nullptr) == 0);

    TrackerConfig after;
    after.GitHubPat = "still-mine";
    smatchet::config_repair::ApplyTrackerConfigRepairs(after);
    CHECK(after.GitHubPat == "still-mine");
}

TEST_CASE("a hook that unregisters itself from inside a save neither deadlocks nor corrupts") {
    smatchet_tests::TestEnvGuard env;

    // The capture unwind can race a save: ApplyTrackerConfigRepairs must therefore run hooks with
    // the registry lock released, and must not be iterating the live vector while a hook mutates it.
    static int selfToken = 0;
    selfToken = smatchet::config_repair::RegisterTrackerConfigRepair([](TrackerConfig& cfg) {
        cfg.GitHubPat = "user-real-pat";
        smatchet::config_repair::UnregisterTrackerConfigRepair(selfToken);
    });
    REQUIRE(selfToken != 0);

    TrackerConfig cleared;
    cleared.TrackerType = "Jira";
    cleared.GitHubPat.clear();
    ConfigManager::Save(cleared);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().GitHubPat == "user-real-pat");

    // The hook dropped itself, so the next save is unrepaired.
    TrackerConfig second;
    second.TrackerType = "Jira";
    second.GitHubPat = "second-write";
    ConfigManager::Save(second);
    ConfigManager::InvalidateCache();
    CHECK(ConfigManager::Load().GitHubPat == "second-write");
}
