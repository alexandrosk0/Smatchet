// UiTestScenario — see header for rationale. This TU is always compiled into
// SmatchetStandalone / SmatchetCore_DX12 so the `ui_test.run` command surface
// stays consistent regardless of build gate; runtime behaviour pivots on
// SMATCHET_BUILD_UI_TESTS.

#include "Commands/Scenarios/UiTestScenario.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Logger.h"

#if defined(SMATCHET_BUILD_UI_TESTS)
#include "imgui.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

namespace {

#if defined(SMATCHET_BUILD_UI_TESTS)
// Number of consecutive frames with a non-zero ImGui DisplaySize that must
// present before we queue the test bodies. The test engine only begins
// executing queued tests from the PostSwap after the queueing frame, so by the
// time the first item-probe runs the UI has rendered + presented this many
// real frames and the windows are laid out at a genuine size. Chosen as a
// small floor that tolerates Mesa llvmpipe's slow first frames in CI while
// staying snappy on a real GPU (the engine itself drains the queue in a single
// PostSwap once probing can succeed). Not a wall-clock sleep — a slower backend
// just needs more frames to reach the floor.
const int kUiTestSettleFrames = 3;

// Active engine pointer surfaced to Source/Standalone/main.cpp via
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

#if defined(SMATCHET_BUILD_UI_TESTS)
void AppendUiTestFailures(ImGuiTestEngine* engine, nlohmann::json& out) {
    nlohmann::json failures = nlohmann::json::array();
    if (engine != nullptr) {
        ImVector<ImGuiTest*> tests;
        ImGuiTestEngine_GetTestList(engine, &tests);
        for (int i = 0; i < tests.Size; ++i) {
            ImGuiTest* test = tests[i];
            if (test == nullptr || test->Output.Status != ImGuiTestStatus_Error) {
                continue;
            }

            ImGuiTextBuffer log;
            test->Output.Log.ExtractLinesForVerboseLevels(ImGuiTestVerboseLevel_Error, ImGuiTestVerboseLevel_Trace,
                                                          &log);
            std::string logText = log.empty() ? std::string(test->Output.Log.GetText()) : std::string(log.c_str());
            if (logText.size() > 2000) {
                logText.resize(2000);
                logText += "...";
            }
            failures.push_back({{"category", test->Category ? test->Category : ""},
                                {"name", test->Name ? test->Name : ""},
                                {"log", logText}});
        }
    }
    out["failures"] = failures;
}

// Emit the per-test KO list + a pass/fail summary + each failing test's log
// excerpt to STDOUT, then flush. The spawn driver
// (CliCommandRunner.cpp::LaunchEphemeralInstance) redirects the ephemeral
// child's stdout+stderr into the spawn-log; but the ImGui Test Engine's own
// output channels do NOT reliably populate that log on a FAILING run:
//   * ConfigLogToTTY (which this scenario sets) only writes per-log-line through
//     ImGuiTestContext::LogEx while a test BODY executes — when the engine marks
//     a test Error before / without the body running through the context (the
//     settle/probe failure mode this scenario is tuned against), little or
//     nothing routes to the TTY; and
//   * ImGuiTestEngine_PrintResultSummary (the upstream one-shot summary that
//     lists KO tests + a pass/fail line) is never called by this scenario —
//     results travel only over the MCP wire into the JSON envelope.
// So on a FAILING spawned run the child can write nothing to the spawn-log,
// which is exactly the diagnosability blocker (docs/self-improvement infra:
// "Bucket-E --spawn flake ... 0-byte child-log on failure"). This dump walks
// the engine's recorded test STATUSES directly (no dependence on per-line TTY
// routing), so it is robust to the timeout / kill / engine-marked-error paths.
// stdout is used (not stderr) because the engine's own LogToTTY writes — which
// DO reach the redirected spawn-log — go to stdout, confirming that channel is
// wired through to the file in the GUI-subsystem child. fflush guarantees the
// bytes hit the handle before the child is quit. Cheap one-shot at end-of-run.
void DumpUiTestOutcomeToStdout(ImGuiTestEngine* engine, int tested, int passed, const char* filter, bool queued) {
    std::fprintf(stdout, "[ui_test] run complete: filter='%s' tested=%d passed=%d failed=%d%s\n",
                 (filter != nullptr && filter[0] != '\0') ? filter : "(all)", tested, passed,
                 (std::max)(0, tested - passed), queued ? "" : " (no tests queued/registered)");

    if (engine != nullptr) {
        ImVector<ImGuiTest*> tests;
        ImGuiTestEngine_GetTestList(engine, &tests);

        bool anyFailure = false;
        for (int i = 0; i < tests.Size; ++i) {
            ImGuiTest* test = tests[i];
            if (test == nullptr || test->Output.Status != ImGuiTestStatus_Error) {
                continue;
            }
            anyFailure = true;
            std::fprintf(stdout, "[ui_test] KO: %s/%s\n", test->Category ? test->Category : "",
                         test->Name ? test->Name : "");

            ImGuiTextBuffer log;
            test->Output.Log.ExtractLinesForVerboseLevels(ImGuiTestVerboseLevel_Error, ImGuiTestVerboseLevel_Trace,
                                                          &log);
            const char* logText = log.empty() ? test->Output.Log.GetText() : log.c_str();
            if (logText != nullptr && logText[0] != '\0') {
                std::fprintf(stdout, "%s\n", logText);
            }
        }
        if (!anyFailure && tested > passed) {
            // Defensive: counts say a failure happened but no test carries an
            // Error status (e.g. queued-but-undrained). Surface it rather than
            // leave the log silent.
            std::fprintf(stdout, "[ui_test] failure reported by counts but no Error-status test found\n");
        }
    }

    std::fprintf(stdout, "[ui_test] Tests Result: %s (%d/%d passed)\n", (tested == passed) ? "OK" : "Errors", passed,
                 tested);
    std::fflush(stdout);
}

bool HasQueuedOrRunningUiTest(ImGuiTestEngine* engine) {
    if (engine == nullptr) {
        return false;
    }
    ImVector<ImGuiTest*> tests;
    ImGuiTestEngine_GetTestList(engine, &tests);
    for (int i = 0; i < tests.Size; ++i) {
        ImGuiTest* test = tests[i];
        if (test != nullptr &&
            (test->Output.Status == ImGuiTestStatus_Queued || test->Output.Status == ImGuiTestStatus_Running)) {
            return true;
        }
    }
    return false;
}
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
    runAll_ = filter_.empty();

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

    LOG_INFO("ui_test.run: engine started (filter='%s')", filter_.empty() ? "(all)" : filter_.c_str());
#else
    (void)app;
    (void)outErr;
    disabled_ = true;
    disabledReason_ = "build had SMATCHET_BUILD_UI_TESTS=OFF";
#endif
}

void UiTestScenario::OnFrame(AppController& /*app*/, int frameIndex) {
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ == nullptr || startedQueue_) {
        return;
    }
    if (frameIndex < 1) {
        return;
    }
    // Only count frames that are actually rendering at a real size. Under Mesa
    // llvmpipe the reported display extent can still be zero-by-zero on the
    // earliest frames, so counting only DisplaySize-valid frames keeps the
    // settle floor honest (a zero-area frame lays out nothing, so probing it
    // would still fail).
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        settledFrames_ = 0;
        return;
    }
    if (++settledFrames_ < kUiTestSettleFrames) {
        return;
    }
    // Queue after the engine's NewFrame hook has synchronized its frame counter
    // AND a few real frames have presented. Queueing immediately in OnStart can
    // run before that hook in a long-lived spawned app, and queueing on the
    // first frame lets the engine probe items before the windows have laid out
    // at a non-zero size under slow software GL — both make the engine mark
    // tests as errors before any test body meaningfully executes.
    const char* filterArg = filter_.empty() ? nullptr : filter_.c_str();
    ImGuiTestEngine_QueueTests(engine_, ImGuiTestGroup_Tests, filterArg, ImGuiTestRunFlags_None);
    startedQueue_ = true;
    LOG_INFO("ui_test.run: queued tests (filter='%s') after %d settled frames", filterArg ? filterArg : "(all)",
             settledFrames_);
#else
    (void)frameIndex;
#endif
}

bool UiTestScenario::IsDone(int frameIndex) const {
    if (disabled_) {
        return true;
    }
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ == nullptr) {
        return true;
    }
    if (!startedQueue_) {
        return false;
    }
    // Give the engine at least one frame to dequeue before claiming "done".
    if (frameIndex < 1) {
        return false;
    }
    if (runAll_) {
        return ImGuiTestEngine_IsTestQueueEmpty(engine_);
    }
    return !HasQueuedOrRunningUiTest(engine_) && ImGuiTestEngine_IsTestQueueEmpty(engine_);
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
        // Surface the disabled reason to the --spawn child-log too — otherwise a
        // SMATCHET_BUILD_UI_TESTS=OFF spawn produces a silent 0-byte log.
        std::fprintf(stdout, "[ui_test] disabled: %s\n", disabledReason_.c_str());
        std::fflush(stdout);
        return out;
    }
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ != nullptr) {
        ImGuiTestEngine_GetResult(engine_, tested_, passed_);
        AppendUiTestFailures(engine_, out);
        // Mirror the outcome to stdout (captured by the --spawn child-log) BEFORE
        // tearing the engine down — see DumpUiTestOutcomeToStdout.
        DumpUiTestOutcomeToStdout(engine_, tested_, passed_, filter_.empty() ? nullptr : filter_.c_str(),
                                  startedQueue_);
        ImGuiTestEngine_Stop(engine_);
        g_active_engine.store(nullptr, std::memory_order_release);
        g_active_app.store(nullptr, std::memory_order_release);
        ImGuiTestEngine_DestroyContext(engine_);
        engine_ = nullptr;
    } else {
        // No engine at OnFinish (creation failed in OnStart, or already torn
        // down). Without this the spawn-log stays empty on that failure path.
        std::fprintf(stdout, "[ui_test] run complete: no active test engine at OnFinish (creation failed?)\n");
        std::fflush(stdout);
    }
    out["passed"] = passed_;
    out["failed"] = (std::max)(0, tested_ - passed_);
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
        // Cancellation is the timeout / watchdog path — the most diagnostically
        // important one for the flake hunt. Capture whatever the engine recorded
        // (queued / running / partial errors) to the --spawn child-log before
        // teardown, so a cancelled run is never a silent 0-byte log.
        int tested = 0;
        int passed = 0;
        ImGuiTestEngine_GetResult(engine_, tested, passed);
        std::fprintf(stdout, "[ui_test] CANCELLED (timeout/watchdog) — dumping partial results:\n");
        DumpUiTestOutcomeToStdout(engine_, tested, passed, filter_.empty() ? nullptr : filter_.c_str(), startedQueue_);
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
