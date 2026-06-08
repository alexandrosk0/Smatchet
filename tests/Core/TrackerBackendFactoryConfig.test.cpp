// Issue #979 regression coverage — the backend factory must build clients from the
// CALLER's live TrackerConfig, never a ConfigManager::Load() disk re-read. The prefs
// "Save & Sync" path calls SyncWithBackend(&d.cfg, ...) while the debounced config
// write is still pending (~100 ms), so a disk re-read inside the factory constructed
// GitHubClient with an empty PAT and the backend stayed dead for the session.
//
// Tests here pin the PLUMBING ONLY: TicketSyncService::SwapBackendIfTrackerChanged
// passes the exact cfgCopy it received into ITrackerBackendFactory::Create (the
// recording factory below REPLACES DefaultTrackerBackendFactory, so these tests do
// not observe what the default factory does with the cfg). The credential BEHAVIOUR
// — live cfg PAT used per-request, cleared PAT stays cleared, no ctor-snapshot
// fallback — is pinned by the ResolveGitHubRequestAuth cases in
// GitHubClientHelpers.test.cpp; with per-request resolution a disk read inside the
// factory can no longer dead-latch the client (the ctor PAT is log-visibility only).

#include "../support/FakeTicketSyncDeps.h"
#include "../support/FakeTrackerClient.h"
#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"
#include "ITrackerBackendFactory.h"
#include "TicketSyncService.h"

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

using smatchet_tests::FakeTicketSyncDeps;
using smatchet_tests::FakeTrackerClient;

namespace {

/// Records the trackerType + cfg every Create call receives, then serves a
/// FakeTrackerClient of the requested kind (so SwapBackendIfTrackerChanged's
/// kind-compare sees the swap as effective and doesn't re-create per sync).
class RecordingTrackerBackendFactory : public ITrackerBackendFactory {
  public:
    int CreateCalls = 0;
    std::string LastTrackerType;
    TrackerConfig LastCfg;

    std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType, const TrackerConfig& cfg) override {
        ++CreateCalls;
        LastTrackerType = trackerType;
        LastCfg = cfg;
        return std::unique_ptr<ITrackerBackend>(new FakeTrackerClient(trackerType));
    }
};

// Spin until predicate or hard cap (same shape as TicketSyncService.test.cpp) — a
// second SyncWithBackend issued while the previous worker is still active gets
// DEFERRED (no synchronous swap), so back-to-back syncs must drain in between.
template <typename Pred> bool SpinUntil(TicketSyncService& svc, Pred pred, int maxTicks = 200) {
    for (int i = 0; i < maxTicks; ++i) {
        svc.TickStreamingApply();
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("issue #979: factory Create receives the caller's live cfg — GitHub PAT carried, no disk read" *
          doctest::test_suite("[high-risk]")) {
    // Redirect the user-data dir to an empty temp dir: ConfigManager::Load() now
    // returns defaults (empty GitHubPat), so the credentials below can ONLY reach
    // the factory through the cfg parameter.
    smatchet_tests::TestEnvGuard env;
    CHECK(ConfigManager::Load().GitHubPat.empty());

    FakeTicketSyncDeps deps;
    auto ownedFactory = std::make_unique<RecordingTrackerBackendFactory>();
    RecordingTrackerBackendFactory* factory = ownedFactory.get();
    deps.Factory = std::move(ownedFactory);
    TicketSyncService svc(deps);

    TrackerConfig cfg;
    cfg.TrackerType = "GitHub";
    cfg.GitHubPat = "abc";
    cfg.GitHubBaseUrl = "https://api.github.com";
    ViewsStore views;
    svc.SyncWithBackend(&cfg, &views);

    REQUIRE(factory->CreateCalls == 1);
    CHECK(factory->LastTrackerType == "GitHub");
    CHECK(factory->LastCfg.GitHubPat == "abc");
    CHECK(factory->LastCfg.GitHubBaseUrl == "https://api.github.com");

    svc.CancelAndJoinActiveStreamingSync();
}

TEST_CASE("issue #979: factory Create carries live Jira + Plane credentials through the swap path") {
    smatchet_tests::TestEnvGuard env;

    FakeTicketSyncDeps deps;
    auto ownedFactory = std::make_unique<RecordingTrackerBackendFactory>();
    RecordingTrackerBackendFactory* factory = ownedFactory.get();
    deps.Factory = std::move(ownedFactory);
    TicketSyncService svc(deps);

    ViewsStore views;

    TrackerConfig cfg;
    cfg.TrackerType = "Plane";
    cfg.PlaneApiKey = "plane-key";
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(factory->CreateCalls == 1);
    CHECK(factory->LastTrackerType == "Plane");
    CHECK(factory->LastCfg.PlaneApiKey == "plane-key");
    REQUIRE(SpinUntil(svc, [&]() { return !svc.IsActive(); }));

    cfg.TrackerType = "Jira";
    cfg.Email = "dev@example.com";
    cfg.ApiToken = "jira-token";
    svc.SyncWithBackend(&cfg, &views);
    REQUIRE(factory->CreateCalls == 2);
    CHECK(factory->LastTrackerType == "Jira");
    CHECK(factory->LastCfg.Email == "dev@example.com");
    CHECK(factory->LastCfg.ApiToken == "jira-token");

    svc.CancelAndJoinActiveStreamingSync();
}
