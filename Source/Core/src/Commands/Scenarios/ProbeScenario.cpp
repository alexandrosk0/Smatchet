// ProbeScenario — see Source/Core/include/Commands/Scenarios/ProbeScenario.h for
// the behaviour contract. A lambda-driven IScenario plus the ProbeScope RAII that
// installs/removes its factory on a ScenarioRunner for a scope.

#include "Commands/Scenarios/ProbeScenario.h"

#include "Commands/Scenarios/IScenario.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

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

    void OnStart(IAppScenarioHost& app, const nlohmann::json& /*args*/, std::string& /*outErr*/) override {
        if (setup_) {
            setup_(app);
        }
    }

    void OnFrame(IAppScenarioHost& /*app*/, int frameIndex) override {
        if (onFrame_) {
            onFrame_(frameIndex);
        }
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
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
    : runner_(runner), name_(std::move(name)), registered_(false) {
    // Name-collision safety: if a factory under name_ already exists, this scope
    // must NOT overwrite it (RegisterFactory does factories_[name] = ..., which
    // would clobber the prior entry) and must NOT claim ownership — otherwise the
    // dtor's UnregisterFactory would erase a factory this scope did not create.
    const std::vector<std::string> existing = runner_.ListNames();
    if (std::find(existing.begin(), existing.end(), name_) != existing.end()) {
        return; // registered_ stays false; dtor leaves the pre-existing factory intact.
    }

    // Capture the lambdas + frame count by value into the factory so each Start()
    // within the scope builds a fresh ProbeScenario. The factory itself out-lives
    // every Start because it lives in runner_.factories_ until this scope's dtor.
    const std::string factoryName = name_;
    ProbeFrameFn frameFn = std::move(onFrame);
    ProbeSetupFn setupFn = std::move(setup);
    runner_.RegisterFactory(name_, [factoryName, frameFn, setupFn, frames]() -> std::unique_ptr<IScenario> {
        return MakeProbeScenario(factoryName, frameFn, setupFn, frames);
    });
    registered_ = true;
}

ProbeScope::~ProbeScope() {
    // Only unregister the factory THIS scope installed. A scope that hit a name
    // collision (registered_ == false) never owned the entry and must leave it.
    if (registered_) {
        runner_.UnregisterFactory(name_);
    }
}

} // namespace cmd
} // namespace smatchet
