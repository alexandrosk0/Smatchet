// UiTestScenario — see header for rationale. This TU is always compiled into
// SmatchetStandalone / SmatchetCore_DX12 so the `ui_test.run` command surface
// stays consistent regardless of build gate; runtime behaviour pivots on
// SMATCHET_BUILD_UI_TESTS.

#include "Commands/Scenarios/UiTestScenario.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/PathConfinement.h"
#include "Commands/Scenarios/UiTestOutcomeFormat.h"
#include "ConfigManager.h"
#include "Logger.h"

#if defined(SMATCHET_BUILD_UI_TESTS)
#include "imgui.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

// Downcast seam declared in IScenario.h. It moved here from the runner TU when
// the command context stopped carrying the concrete controller: this ui-test
// scenario is the sole remaining caller and already includes AppController.h
// for the active-app seam its bucket-E test functions read back.
AppController& RequireConcreteController(IAppScenarioHost& app) { return static_cast<AppController&>(app); }

namespace {

#if defined(SMATCHET_BUILD_UI_TESTS)
// Number of consecutive frames with a non-zero ImGui DisplaySize that must
// present before we queue the test bodies. See header / settle-frame rationale.
const int kUiTestSettleFrames = 3;

// Active engine pointer surfaced to Source/Standalone/main.cpp via
// SmatchetActiveUiTestEngine(). Atomic for the same-thread teardown race.
std::atomic<ImGuiTestEngine*> g_active_engine{nullptr};

// Active AppController pointer surfaced via SmatchetActiveUiTestAppController()
// for bucket-E tests that need app.mainThreadDispatcher.
std::atomic<AppController*> g_active_app{nullptr};
#endif

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

// Walk the engine's recorded test STATUSES and build the POD failure rows the
// pure FormatUiTestOutcome formatter consumes. ImGui-dependent (the only place
// touching ImGuiTest / Output.Status / ExtractLinesForVerboseLevels); the text
// shaping is delegated to the engine-free formatter so it stays unit-testable.
// Walking statuses directly (not per-line TTY routing) makes the dump robust to
// the timeout / kill / engine-marked-error paths the flake hunt cares about.
std::vector<UiTestOutcomeRow> ExtractUiTestFailureRows(ImGuiTestEngine* engine) {
    std::vector<UiTestOutcomeRow> rows;
    if (engine == nullptr) {
        return rows;
    }
    ImVector<ImGuiTest*> tests;
    ImGuiTestEngine_GetTestList(engine, &tests);
    for (int i = 0; i < tests.Size; ++i) {
        ImGuiTest* test = tests[i];
        if (test == nullptr || test->Output.Status != ImGuiTestStatus_Error) {
            continue;
        }
        ImGuiTextBuffer log;
        test->Output.Log.ExtractLinesForVerboseLevels(ImGuiTestVerboseLevel_Error, ImGuiTestVerboseLevel_Trace, &log);
        const char* logText = log.empty() ? test->Output.Log.GetText() : log.c_str();
        UiTestOutcomeRow row;
        row.category = test->Category ? test->Category : "";
        row.name = test->Name ? test->Name : "";
        row.errorLog = (logText != nullptr) ? logText : "";
        rows.push_back(std::move(row));
    }
    return rows;
}

// Dump EVERY registered test's verbose Output.Log (not only failures) to `path`.
// Unlike the stdout summary (failures-only, truncated to 2000 chars) this is the
// full per-test trace the bucket-E bash drivers `cat` on failure. Best-effort:
// a write failure is logged but never fails the run (diagnostics, not a gate).
void DumpAllUiTestLogs(ImGuiTestEngine* engine, const std::string& path) {
    if (engine == nullptr || path.empty()) {
        return;
    }
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN("ui_test.run: --outLog could not open '%s' for writing", path.c_str());
        return;
    }
    ImVector<ImGuiTest*> tests;
    ImGuiTestEngine_GetTestList(engine, &tests);
    for (int i = 0; i < tests.Size; ++i) {
        ImGuiTest* test = tests[i];
        if (test == nullptr) {
            continue;
        }
        const char* category = test->Category ? test->Category : "";
        const char* name = test->Name ? test->Name : "";
        const bool failed = test->Output.Status == ImGuiTestStatus_Error;
        out << "=== " << category << " / " << name << " [" << (failed ? "ERROR" : "ok") << "] ===\n";
        const char* logText = test->Output.Log.GetText();
        if (logText != nullptr && logText[0] != '\0') {
            out << logText;
            out << '\n';
        }
    }
    out.flush();
    LOG_INFO("ui_test.run: wrote per-test log dump to '%s'", path.c_str());
}
#endif

// Emit the formatted ui_test outcome to STDOUT with a SINGLE exempt write, then
// flush. The spawn driver (CliCommandRunner.cpp::LaunchEphemeralInstance)
// redirects the ephemeral child's stdout+stderr into the spawn-log; but the
// ImGui Test Engine's own output channels do NOT reliably populate that log on a
// FAILING run (ConfigLogToTTY only routes while a test BODY runs; the upstream
// ImGuiTestEngine_PrintResultSummary is never called here). So a FAILING spawned
// run can write nothing to the spawn-log: the diagnosability blocker. stdout is
// used (not stderr) because the engine's own LogToTTY writes go to stdout and DO
// reach the redirected spawn-log. fflush guarantees the bytes hit the handle
// before the child quits. The text is built by the pure (engine-free)
// FormatUiTestOutcome so the whole block is ONE write -- the single strict-zone
// printf exemption site for this TU.
void EmitUiTestOutcome(int tested, int passed, const std::string& filter, bool queued,
                       const std::vector<UiTestOutcomeRow>& failures) {
    const std::string text = FormatUiTestOutcome(tested, passed, filter, queued, failures);
    std::fprintf(stdout, "%s", text.c_str()); // CLI stdout -- spawn-log diagnostic capture (LOG_* can't reach the
                                              // spawn-redirected child stdout)
    std::fflush(stdout);
}

#if defined(SMATCHET_BUILD_UI_TESTS)
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
        // Defensive -- OnFinish / OnCancel should have torn down already.
        ImGuiTestEngine_Stop(engine_);
        ImGuiTestEngine_DestroyContext(engine_);
        engine_ = nullptr;
    }
    g_active_engine.store(nullptr, std::memory_order_release);
    g_active_app.store(nullptr, std::memory_order_release);
#endif
}

namespace {
// Opt-in: when SMATCHET_UITEST_WITH_LOCAL_CACHE=1, stand up a throwaway in-memory
// LocalCacheManager so the offline-queue service constructs and the offline-queue
// UI's populated path (DataDependentWindowsSmoke case 8) self-activates. Default
// OFF — existing scenarios see no behaviour change.
bool UiTestWantsLocalCache() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* v = std::getenv("SMATCHET_UITEST_WITH_LOCAL_CACHE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}
} // namespace

void UiTestScenario::OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& outErr) {
    filter_ = args.value("name", std::string());
    outPath_ = args.value("outPath", std::string());
    outLog_ = args.value("outLog", std::string());
    // SECURITY: outPath/outLog are caller-supplied and ui_test.run is reachable from
    // MCP/CLI/Lua. Confine both under a dedicated <userData>/ui-tests/ subdir — not the
    // user-data root, which holds smatchet_config.json — so they cannot write or clobber
    // an arbitrary file.
    {
        const std::string userDataDir = ConfigManager::GetUserDataDirectory();
        std::string resolved, confineErr;
        if (!outPath_.empty()) {
            if (!ConfinePathUnderSubdir(userDataDir, "ui-tests", outPath_, resolved, confineErr)) {
                outErr = "ui_test.run outPath rejected: " + confineErr;
                return;
            }
            outPath_ = resolved;
        }
        if (!outLog_.empty()) {
            if (!ConfinePathUnderSubdir(userDataDir, "ui-tests", outLog_, resolved, confineErr)) {
                outErr = "ui_test.run outLog rejected: " + confineErr;
                return;
            }
            outLog_ = resolved;
        }
    }
    const bool all = args.value("all", false);
    if (all) {
        filter_.clear();
    }

    if (UiTestWantsLocalCache()) {
        // Bucket-E test functions need the concrete pointer via SmatchetActiveUiTestAppController().
        if (RequireConcreteController(app).EnsureLocalCacheForUiTest()) {
            LOG_INFO("ui_test.run: SMATCHET_UITEST_WITH_LOCAL_CACHE=1 — live local cache ensured");
        } else {
            LOG_WARN("ui_test.run: SMATCHET_UITEST_WITH_LOCAL_CACHE=1 but cache init failed");
        }
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

    ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(engine_);
    io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    io.ConfigCaptureEnabled = false;
    io.ConfigCaptureOnError = false;
    io.ConfigStopOnError = false;
    io.ConfigNoThrottle = true;
    io.ConfigLogToTTY = true;
    io.ConfigLogToDebugger = false;
    // The engine is created per ui_test.run and destroyed in OnFinish, i.e. while
    // the app's ImGui context is still alive. With the default (true),
    // ImGuiTestEngine_DestroyContext() asserts "You need to call
    // ImGui::DestroyContext() BEFORE ImGuiTestEngine_DestroyContext()" and aborts
    // the process before OnFinish can write the result JSON — every bucket-E
    // driver then times out with the run already green in the child log. Upstream's
    // documented escape for a runtime-created engine that does not care about its
    // own .ini settings (imgui_te_engine.cpp ImGuiTestEngine_DestroyContext).
    io.ConfigSavedSettings = false;

    SmatchetRegisterAllUiTests(engine_);

    ImGuiTestEngine_Start(engine_, uiCtx);
    g_active_engine.store(engine_, std::memory_order_release);
    g_active_app.store(&RequireConcreteController(app), std::memory_order_release);

    LOG_INFO("ui_test.run: engine started (filter='%s')", filter_.empty() ? "(all)" : filter_.c_str());
#else
    (void)app;
    (void)outErr;
    disabled_ = true;
    disabledReason_ = "build had SMATCHET_BUILD_UI_TESTS=OFF";
#endif
}

void UiTestScenario::OnFrame(IAppScenarioHost& /*app*/, int frameIndex) {
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ == nullptr || startedQueue_) {
        return;
    }
    if (frameIndex < 1) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        settledFrames_ = 0;
        return;
    }
    if (++settledFrames_ < kUiTestSettleFrames) {
        return;
    }
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

nlohmann::json UiTestScenario::OnFinish(IAppScenarioHost& /*app*/) {
    nlohmann::json out;
    if (disabled_) {
        out["passed"] = 0;
        out["failed"] = 0;
        out["log"] = disabledReason_;
        // Surface the disabled reason to the --spawn child-log too -- otherwise a
        // SMATCHET_BUILD_UI_TESTS=OFF spawn produces a silent 0-byte log. One
        // exempt emit through the pure formatter; the disabled reason rides as a
        // note row so it stays in the same single write.
        std::vector<UiTestOutcomeRow> none;
        none.push_back(UiTestOutcomeRow{"disabled", disabledReason_, ""});
        EmitUiTestOutcome(0, 0, std::string(), false, none);
        return out;
    }
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ != nullptr) {
        ImGuiTestEngine_GetResult(engine_, tested_, passed_);
        AppendUiTestFailures(engine_, out);
        // --outLog: full per-test verbose log to disk (before teardown) so the
        // bucket-E bash drivers can `cat` it on failure.
        DumpAllUiTestLogs(engine_, outLog_);
        // Mirror the outcome to stdout (captured by the --spawn child-log) BEFORE
        // tearing the engine down. Extract the failure rows here (the only
        // ImGui-touching step), then emit via the pure formatter with one write.
        EmitUiTestOutcome(tested_, passed_, filter_, startedQueue_, ExtractUiTestFailureRows(engine_));
        ImGuiTestEngine_Stop(engine_);
        g_active_engine.store(nullptr, std::memory_order_release);
        g_active_app.store(nullptr, std::memory_order_release);
        ImGuiTestEngine_DestroyContext(engine_);
        engine_ = nullptr;
    } else {
        // No engine at OnFinish (creation failed in OnStart, or already torn
        // down). Without this the spawn-log stays empty on that failure path.
        std::vector<UiTestOutcomeRow> none;
        none.push_back(UiTestOutcomeRow{"engine", "no active test engine at OnFinish (creation failed?)", ""});
        EmitUiTestOutcome(0, 0, filter_, false, none);
    }
    out["passed"] = passed_;
    out["failed"] = (std::max)(0, tested_ - passed_);
    out["tested"] = tested_;
    out["filter"] = filter_;
    out["outPath"] = outPath_;
    // Surface the RESOLVED, confinement-anchored outLog path (under <userData>/ui-tests/)
    // so the trusted spawn parent (CliCommandRunner) can copy it back to the caller's
    // originally-requested absolute location. The child never writes outside its
    // confinement base; the parent relocates afterwards. Empty when no outLog arg.
    out["outLog"] = outLog_;
    out["log"] = startedQueue_ ? "ui_test.run: completed" : "no tests registered";
#else
    out["passed"] = 0;
    out["failed"] = 0;
    out["log"] = "build had SMATCHET_BUILD_UI_TESTS=OFF";
#endif
    return out;
}

void UiTestScenario::OnCancel(IAppScenarioHost& /*app*/) {
#if defined(SMATCHET_BUILD_UI_TESTS)
    if (engine_ != nullptr) {
        // Cancellation is the timeout / watchdog path -- the most diagnostically
        // important one for the flake hunt. Capture whatever the engine recorded
        // before teardown, so a cancelled run is never a silent 0-byte log. One
        // exempt emit through the pure formatter; the CANCELLED marker rides as
        // the first row so it stays in the same single write.
        int tested = 0;
        int passed = 0;
        ImGuiTestEngine_GetResult(engine_, tested, passed);
        std::vector<UiTestOutcomeRow> rows;
        rows.push_back(UiTestOutcomeRow{"CANCELLED", "timeout/watchdog -- partial results", ""});
        std::vector<UiTestOutcomeRow> failures = ExtractUiTestFailureRows(engine_);
        for (UiTestOutcomeRow& r : failures) {
            rows.push_back(std::move(r));
        }
        EmitUiTestOutcome(tested, passed, filter_, startedQueue_, rows);
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
// linked, the queue starts empty and IsDone returns true on frame 1. This stub
// prevents an undefined-reference link error in that configuration.
#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((weak)) void SmatchetRegisterAllUiTests(ImGuiTestEngine* /*engine*/) {
    // No-op weak default; the strong definition lives in tests/ui/ui_tests_registry.cpp.
}
#endif
#endif
