#ifndef SMATCHET_COMMANDS_SCENARIOS_UITESTSCENARIO_H
#define SMATCHET_COMMANDS_SCENARIOS_UITESTSCENARIO_H

// UiTestScenario — drives the ImGui Test Engine (bucket E) lifecycle through
// the existing IScenario / ScenarioRunner contract.
// Two reasons to wrap the engine in a scenario rather than free functions:
//   1. The engine has multi-frame lifetime — Start, tick across frames via
//      PostSwap, finish when the queue drains. IScenario already models that.
//   2. The unified Command System runs scenarios on the UI thread under
//      `RunOnUiThreadAsCommandResult` for free, side-stepping the
//      ImGuiContext-touching race that any other dispatch path would create.
// Build gate: this header is always included so the command surface stays
// consistent across builds, but the runtime body is gated on
// SMATCHET_BUILD_UI_TESTS. When the gate is OFF, OnStart returns the
// "build had SMATCHET_BUILD_UI_TESTS=OFF" sentinel and IsDone immediately
// returns true.

#include "Commands/Scenarios/IScenario.h"

// json named only by const-reference (OnStart) and as an out-of-line override
// return-type declaration (OnFinish) — forward decl suffices (matches IScenario.h).
#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string>

#if defined(SMATCHET_BUILD_UI_TESTS)
struct ImGuiTestEngine; // opaque
#endif

namespace smatchet {
namespace cmd {

class UiTestScenario : public IScenario {
  public:
    UiTestScenario();
    ~UiTestScenario() override;

    std::string Name() const override { return "ui-test"; }

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& outErr) override;
    void OnFrame(IAppScenarioHost& app, int frameIndex) override;
    bool IsDone(int frameIndex) const override;
    nlohmann::json OnFinish(IAppScenarioHost& app) override;
    void OnCancel(IAppScenarioHost& app) override;

  private:
#if defined(SMATCHET_BUILD_UI_TESTS)
    ImGuiTestEngine* engine_ = nullptr;
    bool startedQueue_ = false;
    bool runAll_ = false;
    // Count of consecutive frames observed with a non-zero ImGui DisplaySize
    // before we queue the tests. Under a slow software-GL backend (Mesa
    // llvmpipe in CI) the first frames can present with DisplaySize == 0 (the
    // GLFW framebuffer-size callback / window-show has not settled), so the
    // windows never lay out — every itemPresent/visible probe then fails and
    // the whole suite collapses to 0 passed. Gating the queue on N settled,
    // really-rendered frames wins that race without a wall-clock sleep: a
    // slower machine simply takes more frames to satisfy the floor.
    int settledFrames_ = 0;
    // Result counters — only populated/read by the gated OnFrame/OnFinish body
    // (ImGuiTestEngine_GetResult). Kept inside the gate so the non-UI-test build
    // doesn't carry unread private fields (-Wunused-private-field under clang).
    int passed_ = 0;
    int tested_ = 0;
#endif
    bool disabled_ = false;
    std::string disabledReason_;
    std::string filter_;
    std::string outPath_;
    // --outLog=<path>: when non-empty, OnFinish writes EVERY test's verbose
    // Output.Log (not just failures) to this file so the bucket-E bash drivers can
    // `cat` it on failure. Empty = no dump (default).
    std::string outLog_;
};

} // namespace cmd
} // namespace smatchet

// Accessor consumed by Source/Standalone/main.cpp to drive
// ImGuiTestEngine_PostSwap from the swap-buffer hook. Returns nullptr when no
// UI test is active. Defined in UiTestScenario.cpp; declared at namespace
// scope so the standalone TU can call it without including the scenario
// header (which would otherwise leak nlohmann/json into main.cpp).
#if defined(SMATCHET_BUILD_UI_TESTS)
extern "C" ImGuiTestEngine* SmatchetActiveUiTestEngine();

// Bucket-E test functions that need AppController (e.g. for
// `app.mainThreadDispatcher`) call this from inside TestFunc. Returns nullptr
// outside an active ui_test.run scenario. Stored at OnStart, cleared at
// OnFinish / OnCancel / dtor. UI thread only.
class AppController;
AppController* SmatchetActiveUiTestAppController();
#endif

#endif // SMATCHET_COMMANDS_SCENARIOS_UITESTSCENARIO_H
