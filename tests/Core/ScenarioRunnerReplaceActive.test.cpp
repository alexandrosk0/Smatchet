// DR7 regression — ScenarioRunner replacing a still-running scenario.
//
// Before the fix, ScenarioRunner::Start move-assigned the new scenario over a
// live active_ (`active_ = std::move(scenario)`), destroying the outgoing
// scenario in place WITHOUT driving its OnCancel/OnFinish. The AI streaming
// scenarios own a joinable std::thread, so ~std::thread on a still-joinable
// worker called std::terminate; the outgoing scenario's AiClientFactory test
// override was also left dangling into freed state.
//
// The fix has two independent guards, both exercised here with a lightweight
// thread-owning stand-in scenario (the real AI scenarios are SMATCHET_WITH_AI
// gated and pull the AI client stack, so a local mirror keeps this in the
// header-light Core rig):
//   1. Start() tears the outgoing scenario down via the Cancel path (OnCancel +
//      join) BEFORE the replacement's OnStart / the move-assign.
//   2. A scenario that owns a joinable worker joins it in its own destructor,
//      so even a destroy-without-cancel can never reach ~std::thread joinable.

#include "Commands/Scenarios/IScenario.h"

#include "Interfaces/IAppScenarioHost.h"

#include "Commands/Command.h"
#include "ConfigManager.h"

#include <nlohmann/json.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using smatchet::cmd::CommandContext;
using smatchet::cmd::IScenario;
using smatchet::cmd::ScenarioRunner;

namespace {

// Shared observation record so the test can inspect a scenario instance after
// the runner has already destroyed it.
struct ReplaceProbe {
    std::atomic<int> onCancelCalls{0};
    std::atomic<int> workerIterations{0};
    std::atomic<bool> workerJoined{false};
};

// A minimal IScenario that owns a joinable worker thread — the exact shape that
// tripped DR7. OnCancel signals + joins (mirrors the AI streaming scenarios),
// and the destructor is a joining backstop.
class ThreadedProbeScenario : public IScenario {
  public:
    explicit ThreadedProbeScenario(std::shared_ptr<ReplaceProbe> probe) : probe_(std::move(probe)) {}

    ~ThreadedProbeScenario() override { StopAndJoin(); }

    std::string Name() const override { return "dr7-threaded-probe"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& /*args*/, std::string& /*outErr*/) override {
        cancel_ = std::make_shared<std::atomic<bool>>(false);
        worker_ = std::thread([this]() {
            while (!cancel_->load(std::memory_order_acquire)) {
                probe_->workerIterations.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {}

    bool IsDone(int /*frameIndex*/) const override { return false; }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override { return nlohmann::json::object(); }

    void OnCancel(IAppScenarioHost& /*app*/) override {
        probe_->onCancelCalls.fetch_add(1, std::memory_order_relaxed);
        StopAndJoin();
    }

  private:
    void StopAndJoin() {
        if (cancel_) {
            cancel_->store(true, std::memory_order_release);
        }
        if (worker_.joinable()) {
            worker_.join();
            probe_->workerJoined.store(true, std::memory_order_release);
        }
    }

    std::shared_ptr<ReplaceProbe> probe_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::thread worker_;
};

// ScenarioRunner::Start requires a non-null ctx.App: it binds *ctx.App to the
// scenario's OnStart(IAppScenarioHost&). ThreadedProbeScenario ignores that
// reference, so a placeholder pointer is never dereferenced — this keeps the
// test in the header-light Core rig (a real AppController lives only in the
// heavy tests/ui binary). Address of a live stack object, so the pointer is a
// valid, non-null address that is simply never read through.
AppController* PlaceholderApp(int& anchor) { return reinterpret_cast<AppController*>(&anchor); }

// Real no-op host for the direct-OnStart test below: the hook takes
// IAppScenarioHost&, and a live stub (unlike the placeholder-pointer trick)
// stays valid even if a future scenario edit touches the reference. Keeps the
// rig header-light — the facet header is AppController-free.
class NullScenarioHost : public IAppScenarioHost {
  public:
    GridLiveContext* EnsurePaneContextLive(const std::string&, const std::string&) override { return nullptr; }
    bool ScenarioRegisterLuaCachedProvider(const std::string&, const std::string&, const std::vector<std::string>&,
                                           std::string&) override {
        return false;
    }
    bool ScenarioRegisterLuaCachedProvider(const std::string&, const std::string&, std::string&) override {
        return false;
    }
    void ScenarioUnregisterLuaCachedProvider(const std::string&) override {}
    void ScenarioInvalidateLuaFieldCache() override {}
};

} // namespace

TEST_CASE("ScenarioRunner::Start tears the outgoing scenario down (OnCancel + join) before replacing it") {
    // Keep Start's default outPath resolution off the real user-data dir.
    ConfigManager::SetUserDataDirectory("scenario_runner_dr7_tmp");

    ScenarioRunner runner;

    std::shared_ptr<ReplaceProbe> nextProbe;
    runner.RegisterFactory("dr7-threaded-probe", [&nextProbe]() -> std::unique_ptr<IScenario> {
        return std::unique_ptr<IScenario>(std::make_unique<ThreadedProbeScenario>(nextProbe));
    });

    int appAnchor = 0;
    CommandContext ctx;
    ctx.App = PlaceholderApp(appAnchor);

    auto probeA = std::make_shared<ReplaceProbe>();
    auto probeB = std::make_shared<ReplaceProbe>();

    // Start scenario A — spawns its joinable worker.
    nextProbe = probeA;
    smatchet::cmd::CommandResult startA = runner.Start("dr7-threaded-probe", nlohmann::json::object(), ctx);
    REQUIRE(startA.Ok);
    REQUIRE(runner.Active());
    CHECK(probeA->onCancelCalls.load() == 0);

    // Replace with scenario B. This is the DR7 path: the runner must first tear
    // A down (OnCancel + worker join) rather than destroy it in place.
    nextProbe = probeB;
    smatchet::cmd::CommandResult startB = runner.Start("dr7-threaded-probe", nlohmann::json::object(), ctx);
    REQUIRE(startB.Ok);
    REQUIRE(runner.Active()); // B is now the active scenario

    // A's teardown hook ran exactly once and its worker was joined — no
    // std::terminate, no leaked thread.
    CHECK(probeA->onCancelCalls.load() == 1);
    CHECK(probeA->workerJoined.load());

    // B was left untouched by A's teardown (it is the live scenario now).
    CHECK(probeB->onCancelCalls.load() == 0);

    // Clean up the live scenario B.
    runner.Cancel();
    CHECK_FALSE(runner.Active());
    CHECK(probeB->onCancelCalls.load() == 1);
    CHECK(probeB->workerJoined.load());
}

TEST_CASE("A scenario owning a joinable worker joins it in its destructor (no std::terminate)") {
    auto probe = std::make_shared<ReplaceProbe>();

    {
        ThreadedProbeScenario scenario(probe);
        std::string err;
        NullScenarioHost host;
        scenario.OnStart(host, nlohmann::json::object(), err);
        CHECK(err.empty());
        // Deliberately do NOT call OnCancel/OnFinish: the destructor alone must
        // join the still-joinable worker. If it did not, ~std::thread would call
        // std::terminate and this process would abort before the CHECK below.
    }

    CHECK(probe->workerJoined.load());
    CHECK(probe->onCancelCalls.load() == 0); // proves the join came from the destructor path
}
