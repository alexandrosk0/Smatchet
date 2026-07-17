// ai_prefs_autosave_flow.test.cpp — bucket-E coverage for the actual shipped
// AI Preferences flow: autosave (MarkPrefsDirty + ~100 ms debounce) and the
// async Test-connection verify probe.
//
// Drift warning — IF YOU CHANGE
//   - Source/Core/include/SmatchetUiSession.h:546-568 (prefsDirty +
//     prefsSaveDueAt + MarkPrefsDirty inline helper), OR
//   - Source/Core/src/SmatchetUI.cpp:768-776 (debounce-and-save drain at
//     end of frame), OR
//   - Source/Core/src/SmatchetPreferencesUi.cpp:198-205 (cancel-on-close
//     short-circuit) or :520-680 (runProbe async path),
// UPDATE THIS REPLICA / HOST-COUPLED CHECKS to match.
//
// The test.md P2 item (4) "Save / Discard + 'Assistant *' dirty-tab label"
// describes a UI surface that was never shipped. This TU covers the actual
// shipped behaviour (autosave + Test-connection verify). Stale-spec
// disposition recorded in docs/self-improvement/categories/test.md.
//
// Three variants:
//   1. Autosave_DebouncesThenSaves (PRIMARY, deterministic) — replica drives
//      MarkPrefsDirty + simulated time advance + the debounce-drain dispatch
//      and asserts save-call count.
//   2. VerifyOnSave_TestConnection_SetsResult (SECONDARY, deterministic) —
//      drives the REAL AiPrefsTestConnection::TriggerProbe against a stub
//      IAiClient (success), then waits on the dispatcher queue for the worker's
//      post-back and Drains it to force the async chain to completion before
//      asserting the "Verified" result line. No frame-budget poll, no host
//      coupling — only the AppController-availability skip guard remains.
//   3. VerifyOnSave_CancelOnClose_ShortCircuits (SECONDARY, informational) —
//      kicks off the probe per V2, sets the cancel atom, then deterministically
//      joins the worker post-back and Drains the dispatcher (same hard-sync as V2,
//      never a frame-budget yield) before closing Preferences; asserts the result
//      line stays at the initial "Testing..." (type 0) per the cancel short-circuit.
//      Skip-with-log on host-coupling failure.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "AiClientFactory.h"
#include "AiPrefsTestConnection.h"
#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "ConfigManager.h"
#include "IAiClient.h"
#include "MainThreadDispatcher.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

// --- Variant 1: autosave replica state -----------------------------------

struct AutosaveState {
    char field[64] = {0};
    bool prefsDirty = false;
    std::chrono::steady_clock::time_point prefsSaveDueAt{};
    int saveCalls = 0;
};

AutosaveState g_autosaveState;

void ResetAutosaveState() {
    std::memset(g_autosaveState.field, 0, sizeof(g_autosaveState.field));
    g_autosaveState.prefsDirty = false;
    g_autosaveState.prefsSaveDueAt = std::chrono::steady_clock::time_point{};
    g_autosaveState.saveCalls = 0;
}

// Mirrors SmatchetUiSession.h MarkPrefsDirty — idempotent within a window.
void LocalMarkPrefsDirty(AutosaveState& s) {
    if (!s.prefsDirty) {
        s.prefsDirty = true;
        s.prefsSaveDueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }
}

// Mirrors SmatchetUI.cpp:772-776 debounce-drain dispatch.
void LocalDrainSaveIfDue(AutosaveState& s) {
    if (s.prefsDirty && std::chrono::steady_clock::now() >= s.prefsSaveDueAt) {
        ++s.saveCalls;
        s.prefsDirty = false;
    }
}

// Faithful replica of an AI Preferences InputText that arms MarkPrefsDirty
// on edit. SmatchetPreferencesUi.cpp wraps every AI buffer mutation in the
// same MarkPrefsDirty(d) call after the InputText returns true.
void DrawAutosaveReplica(AutosaveState& s) {
    if (ImGui::InputText("##AiPrefsField", s.field, sizeof(s.field))) {
        LocalMarkPrefsDirty(s);
    }
}

void RegisterAutosaveDebounceVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefs", "Autosave_DebouncesThenSaves");
    t->UserData = &g_autosaveState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<AutosaveState*>(ctx->Test->UserData);
        ImGui::SetNextWindowSize(ImVec2(360, 100), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::AiPrefsAutosave", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            DrawAutosaveReplica(*s);
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<AutosaveState*>(ctx->Test->UserData);
        ResetAutosaveState();
        ctx->SetRef("SmatchetTest::AiPrefsAutosave");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("##AiPrefsField");
        ctx->KeyChars("a");
        ctx->Yield();
        IM_CHECK(s->prefsDirty);
        IM_CHECK_EQ(s->saveCalls, 0);

        // Simulate time advance: pull the due-at backward so the next drain
        // dispatch fires. Mirrors the SmatchetUI::Draw end-of-frame logic
        // without waiting 100 ms wall-clock.
        s->prefsSaveDueAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        LocalDrainSaveIfDue(*s);
        IM_CHECK_EQ(s->saveCalls, 1);
        IM_CHECK(!s->prefsDirty);

        // Coalesce check: type 5 chars rapidly and only the single
        // post-burst drain should commit one save. Buffer needs to still
        // have room (cap 64) — sequential KeyChars on the already-active
        // input appends.
        ctx->ItemClick("##AiPrefsField");
        ctx->KeyChars("bcdef");
        ctx->Yield();
        IM_CHECK(s->prefsDirty);
        IM_CHECK_EQ(s->saveCalls, 1);

        s->prefsSaveDueAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        LocalDrainSaveIfDue(*s);
        IM_CHECK_EQ(s->saveCalls, 2);
        IM_CHECK(!s->prefsDirty);
    };
}

// --- Variants 2 & 3: live coverage via AiPrefsTestConnection seam --------
//
// Lifted from deferred placeholders once the seam landed
// (`AiClientFactory::SetTestOverride` + `AiPrefsTestConnection::TriggerProbe`
// extraction). Both variants bypass the Preferences UI entirely — the probe
// runs against a stub IAiClient injected via the factory override, and we
// drive `TriggerProbe` directly without traversing
// `Preferences -> Assistant tab -> Test connection click` (engine-side
// ItemClick on `BeginTabItem("Assistant")` sub-items is not reachable in the
// current bucket-E harness).

class StubAiClient : public IAiClient {
  public:
    enum class Mode { Success, Gated };
    StubAiClient(Mode mode, std::atomic<bool>* release) : mode_(mode), release_(release) {}
    std::string GetProviderName() const override { return "stub"; }
    std::string ProbeReachability(const AiClientConfig& /*cfg*/) override { return std::string(); }
    void SendStreaming(const AiClientConfig& /*cfg*/, const AiChatRequest& /*req*/, const DeltaCallback& onDelta,
                       const ErrorCallback& /*onError*/, const CancelToken& /*cancel*/) override {
        if (mode_ == Mode::Gated && release_ != nullptr) {
            for (int i = 0; i < 10000; ++i) {
                if (release_->load(std::memory_order_acquire)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        AiStreamDelta d;
        d.TokenChunk = "pong";
        d.IsFinal = true;
        onDelta(d);
    }

  private:
    Mode mode_;
    std::atomic<bool>* release_;
};

std::atomic<bool> g_stubRelease{false};

std::unique_ptr<IAiClient> MakeStubSuccess(AiProvider /*provider*/) {
    return std::unique_ptr<IAiClient>(new StubAiClient(StubAiClient::Mode::Success, nullptr));
}

std::unique_ptr<IAiClient> MakeStubGated(AiProvider /*provider*/) {
    return std::unique_ptr<IAiClient>(new StubAiClient(StubAiClient::Mode::Gated, &g_stubRelease));
}

void ResetPrefsTestState() {
    g_ui.cfg.AiProviderKind = 0;
    g_ui.cfg.AiBaseUrl = "http://127.0.0.1:1234";
    g_ui.cfg.AiModelOpenAi = "stub-model";
    g_ui.cfg.AiApiKey = "stub-key";
    g_ui.assistantPrefsTestInFlight = false;
    g_ui.assistantPrefsTestResult.clear();
    g_ui.assistantPrefsTestResultType = 0;
    g_ui.assistantPrefsTestCancel.reset();
    // SmatchetDrawPreferencesPanel's close-handler clears these fields every
    // frame when showPreferences=false (cancel-on-close path,
    // SmatchetPreferencesUi.cpp:197-206). Force it true so the probe flow
    // can land its result.
    g_ui.showPreferences = true;
}

void RegisterVerifyOnSaveTestConnectionVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefs", "VerifyOnSave_TestConnection_SetsResult");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("skip: SmatchetActiveUiTestAppController() returned nullptr");
            return;
        }
        ResetPrefsTestState();
        AiClientFactory::SetTestOverride(&MakeStubSuccess);

        AiPrefsTestConnection::TriggerProbe(g_ui, *app, AiProvider::OpenAi);

        // Deterministic completion (replaces the old 240-yield poll, which fired
        // the asserts on incomplete state whenever the worker hadn't posted back
        // within the yield budget). TriggerProbe's completion is a two-step async
        // chain (AiPrefsTestConnection.cpp:231-236):
        //   1. app.LaunchBackgroundTask runs RunProbe on a JOINED pool thread
        //      (AppController.cpp:1099 — a real std::thread tracked in
        //      backgroundWorkers_, never detached, exception-firewalled so the
        //      worker body always runs to completion), whose LAST act is
        //      dispatcher.PostToMainThread(PublishProbeResult).
        //   2. PublishProbeResult only runs — and only then clears
        //      assistantPrefsTestInFlight + sets the result fields — when
        //      mainThreadDispatcher.Drain() executes the queued lambda
        //      (SmatchetUI.cpp:645 is its sole production caller; the engine's
        //      Yield does not drive that production Draw, which is why the old
        //      poll raced).
        // We collapse both steps with a hard synchronization, not a frame budget:
        //   - Block on QueueLen() until the worker's PostToMainThread has landed.
        //     This is a wait on the POST EVENT itself, not a bounded yield loop:
        //     it cannot fall through on incomplete state the way the 240-yield
        //     poll did. Termination is guaranteed — the worker is joined-not-
        //     detached and firewalled, so it always reaches the post. With the
        //     fully-synchronous MakeStubSuccess client the post is imminent; we
        //     spin std::this_thread::yield() (not ctx->Yield(), which would pump
        //     the engine frame loop) purely to relinquish the core to the worker.
        //   - Drain() then runs the now-queued PublishProbeResult on this thread,
        //     flipping assistantPrefsTestInFlight=false and setting "Verified".
        // After the drain the post-conditions are provably established — there is
        // nothing left in flight to race.
        // A generous wall-clock deadline guards a FUTURE regression that breaks
        // the post-back: the spin then fails cleanly via the assert below instead
        // of hanging CI. It is a SAFETY bound, not a frame budget — 5s is enormous
        // for the synchronous stub, so a healthy run never approaches it and it
        // cannot reintroduce the flake the old 240-yield poll had.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (app->mainThreadDispatcher.QueueLen() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        IM_CHECK(app->mainThreadDispatcher.QueueLen() > 0); // worker posted back
        app->mainThreadDispatcher.Drain();

        AiClientFactory::SetTestOverride(nullptr);
        g_ui.showPreferences = false;

        IM_CHECK(!g_ui.assistantPrefsTestInFlight);
        IM_CHECK_EQ(g_ui.assistantPrefsTestResultType, 1);
        IM_CHECK_STR_EQ(g_ui.assistantPrefsTestResult.c_str(), "Verified");
    };
}

void RegisterVerifyOnSaveCancelOnCloseVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefs", "VerifyOnSave_CancelOnClose_ShortCircuits");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("skip: SmatchetActiveUiTestAppController() returned nullptr");
            return;
        }
        ResetPrefsTestState();
        g_stubRelease.store(false, std::memory_order_release);
        AiClientFactory::SetTestOverride(&MakeStubGated);

        AiPrefsTestConnection::TriggerProbe(g_ui, *app, AiProvider::OpenAi);

        IM_CHECK(g_ui.assistantPrefsTestCancel != nullptr);
        g_ui.assistantPrefsTestCancel->store(true, std::memory_order_release);
        g_stubRelease.store(true, std::memory_order_release);

        // Deterministic completion — the same hard synchronization the sibling
        // VerifyOnSave_TestConnection variant uses (lines above), NOT a frame-budget
        // yield loop. The background task ALWAYS posts PublishProbeResult back
        // (AiPrefsTestConnection.cpp:143-147) even when cancelled — the cancel atom
        // only short-circuits INSIDE PublishProbeResult (AiPrefsTestConnection.cpp:91-93).
        // The old 240-`ctx->Yield()` loop returned WITHOUT waiting for that post or
        // draining it: the worker could still be inside RunProbe (reading the factory
        // override) when SetTestOverride(nullptr) cleared it below, and the queued
        // callback outlived the test — a teardown race that abort()s the whole
        // bucket-E child under llvmpipe software-GL timing (a dead-harness parse-miss,
        // not a real per-test failure). Block on the post event, then Drain so nothing
        // is left in flight to race at teardown. The 5s deadline is a SAFETY bound (the
        // gated stub posts within ~1 ms of the release store above), never a frame budget.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (app->mainThreadDispatcher.QueueLen() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        IM_CHECK(app->mainThreadDispatcher.QueueLen() > 0); // worker posted back (even when cancelled)
        app->mainThreadDispatcher.Drain();                  // runs PublishProbeResult (cancel short-circuits)

        AiClientFactory::SetTestOverride(nullptr);
        g_ui.showPreferences = false;

        // DR29: the previous body set assistantPrefsTestInFlight=false itself and then
        // asserted it was false — a self-referential guard. Assert the production-observable
        // outcome instead: the dispatcher callback short-circuited at the cancel guard, so the
        // result line stays as the initial "Testing..." TriggerProbe set (type=0); the success
        // branch's "Verified." (type=1) is never reached.
        IM_CHECK_EQ(g_ui.assistantPrefsTestResultType, 0);
    };
}

} // namespace

extern "C" void SmatchetRegisterAiPrefsAutosaveFlowTests(ImGuiTestEngine* engine) {
    RegisterAutosaveDebounceVariant(engine);
    RegisterVerifyOnSaveTestConnectionVariant(engine);
    RegisterVerifyOnSaveCancelOnCloseVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
