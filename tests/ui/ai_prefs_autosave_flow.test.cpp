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
//   4. AssistantCloseGuard_DiscardDropsStagedEdit and
//   5. AssistantCloseGuard_SaveCommitsStagedEdit — closing Preferences with a staged
//      Assistant edit raises the unsaved-changes modal on the real window; Discard
//      drops the edit, Save & Sync commits it to the live config and to disk.
//   6. AssistantSaveShortcut_CtrlSSavesButNotThroughGuardModal — Ctrl+S in the focused
//      Preferences window commits a staged Assistant edit and leaves the window open;
//      the same chord pressed while the unsaved-changes modal is up saves nothing.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "AiClientFactory.h"
#include "AiPrefsTestConnection.h"
#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "ConfigManager.h"
#include "IAiClient.h"
#include "MainThreadDispatcher.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#if defined(SMATCHET_WITH_MCP)
#include "McpServerStatus.h"
#include "PluginHost.h"
#include "SmatchetDefaults.h"
#endif

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName, ImHashStr
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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

// --- Variants 4-6: the Preferences close gate and Ctrl+S cover staged Assistant edits ---
//
// The Assistant page stages its edits into a working copy and commits them only on Save. Closing
// Preferences used to drop that working copy without a word; the close gate now asks first
// (docs/audits/INPUT_UX_AUDIT.md #74), and Ctrl+S saves it from the keyboard (#72). The variants
// drive the REAL window: the real close gate, the real modal, a real click or key chord.

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

bool PreferencesFullyClosed() { return !g_ui.showPreferences && !g_ui.preferencesBuffersLoaded; }

// The unsaved-changes modal. The id after "###" is all that is hashed, so the lookup does not
// depend on the translated title.
const ImGuiWindow* CloseGuardModal() {
    const ImGuiWindow* modal = ImGui::FindWindowByName("###PrefsTrackerCloseGuard");
    return (modal != nullptr && modal->Active) ? modal : nullptr;
}

// `sourceLabel` is the English label the draw code passes; the localized Button wrapper turns it
// into the submitted label the same way.
ImGuiTestRef CloseGuardButton(const char* sourceLabel) {
    const ImGuiWindow* modal = CloseGuardModal();
    return ImGuiTestRef(modal != nullptr ? ImHashStr(SmatchetLocalization::LabelFromSource(sourceLabel), 0, modal->ID)
                                         : 0);
}

// A reasoning-effort value that differs from the saved one. Not a secret, so nothing the test
// writes needs protecting.
const char* StagedEffortFor(const TrackerConfig& cfg) { return cfg.AiReasoningEffort == "high" ? "low" : "high"; }

// What the variants below change outside the window itself, captured before the first change.
struct StagedEditTestBackup {
    TrackerConfig cfg;
    PreferencesCategory category;
    std::string search;
};

StagedEditTestBackup CaptureStagedEditTestBackup() {
    return StagedEditTestBackup{g_ui.cfg, g_ui.preferencesCategory, std::string(g_ui.prefsSearchBuf)};
}

// The window's save (the modal's Save & Sync button, Ctrl+S) re-applies the MCP settings to the
// plugin host. This spawned app serves the test driver over an MCP server the command line forced
// on, which the profile's config knows nothing about; re-applying that config as it stands would
// stop the server the whole run is driven through. Describe the running server in the config
// first, so the re-apply finds nothing to change. Only while the window is closed: it loads its
// MCP buffers from cfg on open, and an open window would read the change as an unsaved MCP edit.
void DescribeLiveMcpServerInConfig(AppController* app) {
#if defined(SMATCHET_WITH_MCP)
    g_ui.cfg.McpEnabled = false;
    const PluginHost* host = app->RuntimePluginHost();
    if (host == nullptr) {
        return;
    }
    const McpServerStatus status = host->GetMcpServerStatus();
    if (status.PluginRegistered) {
        g_ui.cfg.McpEnabled = true;
        g_ui.cfg.McpPort = status.ListenPort;
        g_ui.cfg.McpAllowRemote = status.BindHost == SmatchetDefaults::Mcp::kBindAny;
    }
#else
    (void)app;
#endif
}

// The driver's MCP server as the plugin host sees it: the port it listens on (0 when this app has
// none) and how many times the host has logged starting or stopping the plugin. The description
// above covers the port and the bind address only; the host also restarts the server when the
// token, the Lua switch or the rate limits in cfg differ from what it started with, on the same
// port. The start/stop lines are what shows that.
struct LiveMcpServerState {
    int port = 0;
    int startStopLines = 0;
};

LiveMcpServerState ReadLiveMcpServerState(AppController* app) {
    LiveMcpServerState out;
#if defined(SMATCHET_WITH_MCP)
    const PluginHost* host = app->RuntimePluginHost();
    if (host != nullptr) {
        const McpServerStatus status = host->GetMcpServerStatus();
        out.port = status.PluginRegistered ? status.ListenPort : 0;
    }
    for (const std::string& line : app->CopyMcpActivityLog()) {
        if (line.find("MCP: plugin started") != std::string::npos ||
            line.find("MCP: plugin stopped") != std::string::npos) {
            ++out.startStopLines;
        }
    }
#else
    (void)app;
#endif
    return out;
}

// True while the save left the driver's MCP server alone: same port, and not stopped or restarted.
// (The activity log is capped, so old lines can drop out; a restart can only add to the count.)
bool LiveMcpServerUntouched(AppController* app, const LiveMcpServerState& before) {
    const LiveMcpServerState now = ReadLiveMcpServerState(app);
    return now.port == before.port && now.startStopLines <= before.startStopLines;
}

// One line of the state that decides whether Preferences opens, seeds and closes, so a red run says
// which wait failed and why instead of only that the helper returned false.
void LogPreferencesState(ImGuiTestContext* ctx, const char* what) {
    const ImGuiWindow* prefsWindow = ImGui::FindWindowByName("Preferences");
    const bool windowLive = prefsWindow != nullptr && prefsWindow->Active;
    ctx->LogError("%s: showPreferences=%d buffersLoaded=%d seeded=%d closeGuardOpen=%d windowLive=%d "
                  "backendReached=%d category=%d search='%s' modal=%d",
                  what, g_ui.showPreferences ? 1 : 0, g_ui.preferencesBuffersLoaded ? 1 : 0,
                  g_ui.assistantPrefsWorkingSeeded ? 1 : 0, g_ui.prefsTrackerCloseGuardOpen ? 1 : 0, windowLive ? 1 : 0,
                  g_ui.cfg.BackendHasBeenReachable ? 1 : 0, static_cast<int>(g_ui.preferencesCategory),
                  g_ui.prefsSearchBuf, ImGui::GetCurrentContext()->OpenPopupStack.Size);
}

// Opens Preferences on the Assistant page, waits until the page has seeded its working copy from
// cfg, then stages one edit into it — the state a user is in after changing a setting there
// without pressing Save. `savingApp` is the app when the variant goes on to run a real save (see
// DescribeLiveMcpServerInConfig), null otherwise.
bool OpenAssistantPageWithStagedEdit(ImGuiTestContext* ctx, const char* stagedEffort,
                                     AppController* savingApp = nullptr) {
    g_ui.cfg.BackendHasBeenReachable = true; // without it the first-run lock re-opens the window
    g_ui.showPreferences = false;
    if (!YieldUntil(ctx, [] { return PreferencesFullyClosed(); })) {
        LogPreferencesState(ctx, "the window never closed");
        return false;
    }
    if (savingApp != nullptr) {
        DescribeLiveMcpServerInConfig(savingApp);
    }
    g_ui.preferencesCategory = PreferencesCategory::AiVoice;
    g_ui.prefsSearchBuf[0] = '\0';
    g_ui.showPreferences = true;
    // Re-arm the focus request each frame: a docked Preferences tab only draws its body once it
    // is the selected tab, and the draw code consumes the request in one frame.
    const bool seeded = YieldUntil(ctx, [] {
        g_ui.requestPreferencesFocus = true;
        return g_ui.assistantPrefsWorkingSeeded;
    });
    // Stop asking for focus before the modal comes up: a focus request raises the Preferences
    // window every frame, over the modal.
    g_ui.requestPreferencesFocus = false;
    if (!seeded) {
        LogPreferencesState(ctx, "the Assistant page never seeded its working copy");
        return false;
    }
    ctx->Yield(3);
    g_ui.assistantPrefsWorking.AiReasoningEffort = stagedEffort;
    ctx->Yield();
    if (g_ui.assistantPrefsWorking.AiReasoningEffort != stagedEffort) {
        LogPreferencesState(ctx, "the staged edit did not stay in the working copy");
        return false;
    }
    return true;
}

// Asks the window to close and waits for the modal. False when the window closed, or stayed open
// without asking.
bool CloseRaisesGuardModal(ImGuiTestContext* ctx) {
    g_ui.showPreferences = false;
    return YieldUntil(ctx, [] { return CloseGuardModal() != nullptr; }, 120);
}

// Leaves the window closed and the UI's own state as the test found it, whichever step failed.
// The copy on disk is the caller's to restore: only the saving variants write it.
void RestoreAfterStagedEditTest(ImGuiTestContext* ctx, const StagedEditTestBackup& backup) {
    ctx->PopupCloseAll();
    g_ui.assistantPrefsWorking.AiReasoningEffort = g_ui.cfg.AiReasoningEffort; // nothing left staged
    g_ui.showPreferences = false;
    YieldUntil(ctx, [] { return PreferencesFullyClosed(); });
    g_ui.cfg = backup.cfg;
    g_ui.preferencesCategory = backup.category;
    std::snprintf(g_ui.prefsSearchBuf, sizeof(g_ui.prefsSearchBuf), "%s", backup.search.c_str());
}

// The saving variants ran a real Save & Sync. Let the sync it started and the debounced config
// write it armed both finish while the config they read is still the saved one; only then is it
// safe to put the test's backups back, in memory and on disk. Closing the window and swapping the
// config back arm that debounced write once more (the window-open flag is persisted), so the disk
// copy goes back last, after that write has landed too.
void SettleSaveAndRestoreDisk(ImGuiTestContext* ctx, AppController* app, const StagedEditTestBackup& backup,
                              const TrackerConfig& diskBackup) {
    YieldUntil(ctx, [app] { return !app->IsSyncPendingOrActive() && !g_ui.prefsDirty; }, 1200);
    RestoreAfterStagedEditTest(ctx, backup);
    ctx->Yield(3);
    YieldUntil(ctx, [] { return !g_ui.prefsDirty; }, 600);
    ConfigManager::Save(diskBackup);
    ConfigManager::InvalidateCache();
}

AppController* StagedEditTestApp(ImGuiTestContext* ctx) {
    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("skip: SmatchetActiveUiTestAppController() returned nullptr");
    }
    return app;
}

void RegisterAssistantCloseGuardDiscardVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefs", "AssistantCloseGuard_DiscardDropsStagedEdit");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (StagedEditTestApp(ctx) == nullptr) {
            return;
        }
        const StagedEditTestBackup backup = CaptureStagedEditTestBackup();
        const std::string savedEffort = backup.cfg.AiReasoningEffort;
        const char* stagedEffort = StagedEffortFor(backup.cfg);
        ConfigManager::InvalidateCache();
        const std::string diskEffort = ConfigManager::Load().AiReasoningEffort;

        const bool staged = OpenAssistantPageWithStagedEdit(ctx, stagedEffort);
        IM_CHECK_NO_RET(staged);
        if (staged) {
            const bool asked = CloseRaisesGuardModal(ctx);
            IM_CHECK_NO_RET(asked);                // the close did not go through silently
            IM_CHECK_NO_RET(g_ui.showPreferences); // the gate put the window back
            IM_CHECK_NO_RET(g_ui.assistantPrefsWorking.AiReasoningEffort == stagedEffort); // edit still there
            if (asked) {
                ctx->ItemClick(CloseGuardButton("Discard"));
                const bool closed = YieldUntil(ctx, [] { return PreferencesFullyClosed(); });
                IM_CHECK_NO_RET(closed);
                IM_CHECK_NO_RET(g_ui.cfg.AiReasoningEffort == savedEffort); // nothing was committed
                IM_CHECK_NO_RET(!g_ui.assistantPrefsWorkingSeeded);         // the next open re-seeds from cfg
                ConfigManager::InvalidateCache();
                IM_CHECK_NO_RET(ConfigManager::Load().AiReasoningEffort == diskEffort); // nor written to disk
            }
        }

        RestoreAfterStagedEditTest(ctx, backup);
    };
}

void RegisterAssistantCloseGuardSaveVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefs", "AssistantCloseGuard_SaveCommitsStagedEdit");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = StagedEditTestApp(ctx);
        if (app == nullptr) {
            return;
        }
        const StagedEditTestBackup backup = CaptureStagedEditTestBackup();
        const char* stagedEffort = StagedEffortFor(backup.cfg);
        ConfigManager::InvalidateCache();
        const TrackerConfig diskBackup = ConfigManager::Load();
        const LiveMcpServerState mcpBefore = ReadLiveMcpServerState(app);

        const bool staged = OpenAssistantPageWithStagedEdit(ctx, stagedEffort, app);
        IM_CHECK_NO_RET(staged);
        if (staged) {
            const bool asked = CloseRaisesGuardModal(ctx);
            IM_CHECK_NO_RET(asked);
            if (asked) {
                ctx->ItemClick(CloseGuardButton("Save & Sync"));
                const bool closed = YieldUntil(ctx, [] { return PreferencesFullyClosed(); });
                IM_CHECK_NO_RET(closed);
                IM_CHECK_NO_RET(g_ui.cfg.AiReasoningEffort == stagedEffort); // committed to the live config
                ConfigManager::InvalidateCache();
                IM_CHECK_NO_RET(ConfigManager::Load().AiReasoningEffort == stagedEffort); // and to disk
                IM_CHECK_NO_RET(LiveMcpServerUntouched(app, mcpBefore));
            }
        }

        SettleSaveAndRestoreDisk(ctx, app, backup, diskBackup);
    };
}

void RegisterAssistantSaveShortcutVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefs", "AssistantSaveShortcut_CtrlSSavesButNotThroughGuardModal");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = StagedEditTestApp(ctx);
        if (app == nullptr) {
            return;
        }
        const StagedEditTestBackup backup = CaptureStagedEditTestBackup();
        const std::string savedEffort = backup.cfg.AiReasoningEffort;
        const std::string stagedEffort = StagedEffortFor(backup.cfg);
        ConfigManager::InvalidateCache();
        const TrackerConfig diskBackup = ConfigManager::Load();
        const LiveMcpServerState mcpBefore = ReadLiveMcpServerState(app);

        const bool staged = OpenAssistantPageWithStagedEdit(ctx, stagedEffort.c_str(), app);
        IM_CHECK_NO_RET(staged);
        if (staged) {
            // The window was just raised and focused by the open; no click, the chord alone.
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_S);
            const bool saved =
                YieldUntil(ctx, [&stagedEffort] { return g_ui.cfg.AiReasoningEffort == stagedEffort; }, 60);
            IM_CHECK_NO_RET(saved);                // committed to the live config
            IM_CHECK_NO_RET(g_ui.showPreferences); // a save, not a close
            ConfigManager::InvalidateCache();
            IM_CHECK_NO_RET(ConfigManager::Load().AiReasoningEffort == stagedEffort); // and to disk
            IM_CHECK_NO_RET(LiveMcpServerUntouched(app, mcpBefore));

            // Stage the opposite edit and ask to close: with the unsaved-changes modal up the
            // chord must not reach the window behind it and answer the question on its own.
            g_ui.assistantPrefsWorking.AiReasoningEffort = savedEffort;
            ctx->Yield();
            const bool asked = saved && CloseRaisesGuardModal(ctx);
            IM_CHECK_NO_RET(asked);
            if (asked) {
                ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_S);
                ctx->Yield(5);
                IM_CHECK_NO_RET(g_ui.cfg.AiReasoningEffort == stagedEffort); // nothing new committed
                IM_CHECK_NO_RET(CloseGuardModal() != nullptr);               // still asking
                ctx->ItemClick(CloseGuardButton("Discard"));
                IM_CHECK_NO_RET(YieldUntil(ctx, [] { return PreferencesFullyClosed(); }));
                IM_CHECK_NO_RET(g_ui.cfg.AiReasoningEffort == stagedEffort);
            }
        }

        SettleSaveAndRestoreDisk(ctx, app, backup, diskBackup);
    };
}

} // namespace

extern "C" void SmatchetRegisterAiPrefsAutosaveFlowTests(ImGuiTestEngine* engine) {
    RegisterAutosaveDebounceVariant(engine);
    RegisterVerifyOnSaveTestConnectionVariant(engine);
    RegisterVerifyOnSaveCancelOnCloseVariant(engine);
    RegisterAssistantCloseGuardDiscardVariant(engine);
    RegisterAssistantCloseGuardSaveVariant(engine);
    RegisterAssistantSaveShortcutVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
