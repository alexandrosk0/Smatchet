// UiTestScenario — see header for rationale. This TU is always compiled into
// SmatchetStandalone / SmatchetCore_DX12 so the `ui_test.run` command surface
// stays consistent regardless of build gate; runtime behaviour pivots on
// SMATCHET_BUILD_UI_TESTS.

#include "Commands/Scenarios/UiTestScenario.h"

#include "AppController.h"
#include "Logger.h"

#if defined(SMATCHET_BUILD_UI_TESTS)
#include "imgui.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#endif

#include <algorithm>
#include <atomic>
#include <utility>

namespace smatchet {
namespace cmd {

namespace {

#if defined(SMATCHET_BUILD_UI_TESTS)
// Active engine pointer surfaced to Target_Standalone/main.cpp via
// SmatchetActiveUiTestEngine(). Atomic for the unlikely case where the swap
// hook runs while the scenario is being torn down on the same thread (both
// are UI thread today; defensive nevertheless).
std::atomic<ImGuiTestEngine*> g_active_engine{nullptr};

// Active AppController pointer surfaced via SmatchetActiveUiTestAppController()
// for bucket-E tests that need `app.mainThreadDispatcher` (e.g. the
// ai_prefs_autosave_flow verify-on-save variants that drive
// AiPrefsTestConnection::TriggerProbe). Same UI-thread semantics as g_active_engine.
std::atomic<AppController*> g_active_app{nullptr};
#endif

// Per-feature registration entry point. Implemented in
// tests/ui/ui_tests_registry.cpp when SMATCHET_BUILD_UI_TESTS is ON; the OFF
// build provides a weak stub below so the link succeeds with zero tests.
#if defined(SMATCHET_BUILD_UI_TESTS)
extern "C" void SmatchetRegisterAllUiTests(ImGuiTestEngine* engine);
#endif

} // namespace

UiTestScenario::UiTestScenario() = default;
UiTestScenario::~UiTestScenario() {
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ != nullptr) {
        // Defensive — OnFinish / OnCancel should have torn down already.
        ImGuiTestEngine_Stop(engine_);
        ImGuiTestEngine_DestroyContext(engine_);
        engine_ = nullptr;
    }
    g_active_engine.store(nullptr, std::memory_order_release);
    g_active_app.store(nullptr, std::memory_order_release);
#endif
}

void UiTestScenario::OnStart(AppController& app, const nlohmann::json& args, std::string& outErr) {
    filter_ = args.value("name", std::string());
    outPath_ = args.value("outPath", std::string());
    const bool all = args.value("all", false);
    if (all) {
        filter_.clear();
    }

#if defined(SMATCHET_BUILD_UI_TESTS)
    ImGuiContext* uiCtx = ImGui::GetCurrentContext();
    if (uiCtx == nullptr) {
        outErr = "ui_test.run: no active ImGuiContext at OnStart";
        return;
    }

    engine_ = ImGuiTestEngine_CreateContext();
    if (engine_ == nullptr) {
        outErr = "ui_test.run: ImGuiTestEngine_CreateContext returned null";
        return;
    }

    // Tune the engine for headless deterministic runs. We never plug a
    // ScreenCaptureFunc, so capture-on-error is a no-op even if the test
    // requests it.
    ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(engine_);
    io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    io.ConfigCaptureEnabled = false;
    io.ConfigCaptureOnError = false;
    io.ConfigStopOnError = false;
    io.ConfigNoThrottle = true;
    // Surface engine errors (item-not-found, assertion text) to stderr so the
    // bucket-E driver script can capture them. Negligible cost — each run is
    // a one-shot ephemeral process.
    io.ConfigLogToTTY = true;
    io.ConfigLogToDebugger = false;

    SmatchetRegisterAllUiTests(engine_);

    ImGuiTestEngine_Start(engine_, uiCtx);
    g_active_engine.store(engine_, std::memory_order_release);
    g_active_app.store(&app, std::memory_order_release);

    // QueueTests with an empty filter runs every registered test in the
    // ImGuiTestGroup_Tests group. The filter is a comma-separated wildcard
    // list — pass the user-supplied --name= verbatim.
    const char* filterArg = filter_.empty() ? nullptr : filter_.c_str();
    ImGuiTestEngine_QueueTests(engine_, ImGuiTestGroup_Tests, filterArg, ImGuiTestRunFlags_None);
    startedQueue_ = true;
    LOG_INFO("ui_test.run: queued tests (filter='%s')", filterArg ? filterArg : "(all)");
#else
    (void)app;
    (void)outErr;
    disabled_ = true;
    disabledReason_ = "build had SMATCHET_BUILD_UI_TESTS=OFF";
#endif
}

void UiTestScenario::OnFrame(AppController& /*app*/, int /*frameIndex*/) {
    // Engine is driven by ImGuiTestEngine_PostSwap from Target_Standalone's
    // glfwSwapBuffers hook (see SmatchetActiveUiTestEngine accessor). Nothing
    // to do here.
}

bool UiTestScenario::IsDone(int frameIndex) const {
    if (disabled_) {
        return true;
    }
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ == nullptr || !startedQueue_) {
        return true;
    }
    // Give the engine at least one frame to dequeue before claiming "done".
    if (frameIndex < 1) {
        return false;
    }
    return ImGuiTestEngine_IsTestQueueEmpty(engine_);
#else
    (void)frameIndex;
    return true;
#endif
}

nlohmann::json UiTestScenario::OnFinish(AppController& /*app*/) {
    nlohmann::json out;
    if (disabled_) {
        out["passed"] = 0;
        out["failed"] = 0;
        out["log"] = disabledReason_;
        return out;
    }
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ != nullptr) {
        ImGuiTestEngine_GetResult(engine_, tested_, passed_);
        ImGuiTestEngine_Stop(engine_);
        g_active_engine.store(nullptr, std::memory_order_release);
        g_active_app.store(nullptr, std::memory_order_release);
        ImGuiTestEngine_DestroyContext(engine_);
        engine_ = nullptr;
    }
    out["passed"] = passed_;
    out["failed"] = std::max(0, tested_ - passed_);
    out["tested"] = tested_;
    out["filter"] = filter_;
    out["outPath"] = outPath_;
    out["log"] = startedQueue_ ? "ui_test.run: completed" : "no tests registered";
#else
    out["passed"] = 0;
    out["failed"] = 0;
    out["log"] = "build had SMATCHET_BUILD_UI_TESTS=OFF";
#endif
    return out;
}

void UiTestScenario::OnCancel(AppController& /*app*/) {
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ != nullptr) {
        ImGuiTestEngine_Stop(engine_);
        g_active_engine.store(nullptr, std::memory_order_release);
        g_active_app.store(nullptr, std::memory_order_release);
        ImGuiTestEngine_DestroyContext(engine_);
        engine_ = nullptr;
    }
#endif
}

} // namespace cmd
} // namespace smatchet

// Factory for ScenarioRunner registration. Mirrors the
// MakePriorityGridScrollScenario pattern in AppController.cpp.
std::unique_ptr<smatchet::cmd::IScenario> MakeUiTestScenario() {
    return std::make_unique<smatchet::cmd::UiTestScenario>();
}

#if defined(SMATCHET_BUILD_UI_TESTS)
extern "C" ImGuiTestEngine* SmatchetActiveUiTestEngine() {
    return smatchet::cmd::g_active_engine.load(std::memory_order_acquire);
}

AppController* SmatchetActiveUiTestAppController() {
    return smatchet::cmd::g_active_app.load(std::memory_order_acquire);
}

// Weak default. When SMATCHET_BUILD_UI_TESTS is ON but no test sources are
// linked (e.g. someone disabled tests/ui/CMakeLists.txt enrolment by hand),
// the queue starts empty and IsDone returns true on frame 1. This stub
// prevents an undefined-reference link error in that configuration.
#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((weak)) void SmatchetRegisterAllUiTests(ImGuiTestEngine* /*engine*/) {
    // No-op weak default; the strong definition lives in tests/ui/ui_tests_registry.cpp.
}
#endif
#endif
