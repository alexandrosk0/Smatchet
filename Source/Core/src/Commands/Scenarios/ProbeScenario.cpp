// ProbeScenario — see Source/Core/include/Commands/Scenarios/ProbeScenario.h for
// the behaviour contract. A lambda-driven IScenario plus the ProbeScope RAII that
// installs/removes its factory on a ScenarioRunner for a scope.

#include "Commands/Scenarios/ProbeScenario.h"

#include "Commands/Scenarios/IScenario.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

namespace smatchet {
namespace cmd {

namespace {

class ProbeScenario : public IScenario {
  public:
    ProbeScenario(std::string name, ProbeFrameFn onFrame, ProbeSetupFn setup, int frames)
        : name_(std::move(name)), onFrame_(std::move(onFrame)), setup_(std::move(setup)),
          frames_((std::max)(1, frames)) {}

    std::string Name() const override { return name_; }

    void OnStart(AppController& app, const nlohmann::json& /*args*/, std::string& /*outErr*/) override {
        if (setup_) {
            setup_(app);
        }
    }

    void OnFrame(AppController& /*app*/, int frameIndex) override {
        if (onFrame_) {
            onFrame_(frameIndex);
        }
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        nlohmann::json out;
        out["name"] = name_;
        out["frames"] = frames_;
        return out;
    }

  private:
    std::string name_;
    ProbeFrameFn onFrame_;
    ProbeSetupFn setup_;
    int frames_;
};

} // namespace

std::unique_ptr<IScenario> MakeProbeScenario(std::string name, ProbeFrameFn onFrame, ProbeSetupFn setup, int frames) {
    return std::make_unique<ProbeScenario>(std::move(name), std::move(onFrame), std::move(setup), frames);
}

ProbeScope::ProbeScope(ScenarioRunner& runner, std::string name, ProbeFrameFn onFrame, ProbeSetupFn setup, int frames)
    : runner_(runner), name_(std::move(name)) {
    // Capture the lambdas + frame count by value into the factory so each Start()
    // within the scope builds a fresh ProbeScenario. The factory itself out-lives
    // every Start because it lives in runner_.factories_ until this scope's dtor.
    const std::string factoryName = name_;
    ProbeFrameFn frameFn = std::move(onFrame);
    ProbeSetupFn setupFn = std::move(setup);
    runner_.RegisterFactory(name_, [factoryName, frameFn, setupFn, frames]() -> std::unique_ptr<IScenario> {
        return MakeProbeScenario(factoryName, frameFn, setupFn, frames);
    });
}

ProbeScope::~ProbeScope() { runner_.UnregisterFactory(name_); }

} // namespace cmd
} // namespace smatchet
