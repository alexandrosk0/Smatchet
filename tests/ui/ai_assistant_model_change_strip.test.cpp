// ai_assistant_model_change_strip.test.cpp — bucket-E coverage for the F2
// "model change auto-clears chat history" rule and the resulting warning strip.
//
// The shipped behaviour: when the active AI provider (or effective model)
// changes between two Assistant turns, AiAssistantController::ResolveModelAndEffort
// (Source/Core/src/AiAssistantController.cpp:337-351) posts a UI-thread task that
//   * clears g_ui.assistantHistory + g_ui.assistantStreamBuf, and
//   * sets g_ui.assistantLastError = "[model changed - chat cleared]"
// The chat window renders that string as an orange warning strip
// (the DeepSeek "[model changed - chat cleared]" strip).
//
// Oracle choice — STATE, not pixels. The canonical signal is the
// g_ui.assistantLastError string set at AiAssistantController.cpp:348, plus the
// cleared history vector. We assert that state directly: it is deterministic and
// robust. We deliberately do NOT scrape the rendered strip text out of the ImGui
// DrawList — ImGui 1.92.8's ImDrawCmd carries no source text, so a pixel/draw
// oracle would be brittle and is the wrong tool here (same constraint the L3
// callstack-tooltip work hit).
//
// Determinism — we drive the controller's own worker thread, then advance the
// MainThreadDispatcher with Drain() (FIFO-until-budget) so the posted clear +
// final-delta tasks land before we read g_ui. State transitions are observed via
// AiAssistantController::CurrentState(); the dispatcher post is consumed by an
// explicit Drain() rather than a fixed yield-count poll (no 240-yield flake).
//
// Drift warning — IF YOU CHANGE the model-change-clear block in
// AiAssistantController::ResolveModelAndEffort (the DetectModelChange call + the
// PostToMainThread body that sets assistantLastError to
// "[model changed - chat cleared]"), UPDATE the expected string below.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "AiAssistantController.h"
#include "AiClientFactory.h"
#include "AiTypes.h"
#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "ConfigManager.h"
#include "IAiClient.h"
#include "MainThreadDispatcher.h"
#include "Ui/SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <memory>
#include <string>

// `g_ui` is the UI-thread-owned UiDrawSession defined in SmatchetUI.cpp. The
// production AiAssistantController TU declares it the same way (see
// AiAssistantController.cpp:33) so the dispatcher callbacks reach the same
// global regardless of the Lua-automation build flag.
extern UiDrawSession g_ui;

namespace {

// Stub IAiClient — ack-streams a single non-empty token then a final delta, all
// synchronously on the worker thread (the IAiClient contract: SendStreaming
// blocks until done/cancelled). No cpr, no network. Injected for BOTH turns via
// AiClientFactory::SetTestOverride, so the provider enum the controller resolves
// (Anthropic, then DeepSeek) selects nothing real — every MakeAiClient returns
// this stub.
class ModelChangeStubAiClient : public IAiClient {
  public:
    std::string GetProviderName() const override { return "stub"; }
    std::string ProbeReachability(const AiClientConfig& /*cfg*/) override { return std::string(); }
    void SendStreaming(const AiClientConfig& /*cfg*/, const AiChatRequest& /*req*/, const DeltaCallback& onDelta,
                       const ErrorCallback& /*onError*/, const CancelToken& /*cancel*/) override {
        if (onDelta) {
            AiStreamDelta token;
            token.TokenChunk = "ack";
            token.IsFinal = false;
            onDelta(token);

            AiStreamDelta done;
            done.IsFinal = true;
            done.FinishReason = "stop";
            onDelta(done);
        }
    }
};

std::unique_ptr<IAiClient> MakeModelChangeStub(AiProvider /*provider*/) {
    return std::unique_ptr<IAiClient>(new ModelChangeStubAiClient());
}

// Persist a config snapshot with the requested provider kind + matching model
// ids so RefreshProviderForTurn() / ResolveModelAndEffort() — both of which read
// ConfigManager::Load() off disk, NOT g_ui.cfg — observe the provider change.
// providerKind: 1 = Anthropic, 4 = DeepSeek (AiTypes.h AiProvider enum).
void PersistProvider(int providerKind) {
    TrackerConfig cfg = ConfigManager::Load();
    cfg.AiProviderKind = providerKind;
    // Per-provider model ids — distinct so the "provider|model" signature
    // differs between the two turns even on the provider axis alone.
    cfg.AiModelAnthropic = "claude-sonnet-4-6";
    cfg.AiModelDeepSeek = "deepseek-chat";
    // Stub serves every provider, so keys/URLs are irrelevant to streaming, but
    // populate them so BuildClientConfig has no empty-field surprises.
    cfg.AiAnthropicApiKey = "stub-key";
    cfg.AiDeepSeekApiKey = "stub-key";
    ConfigManager::Save(cfg);
}

// Drive one turn through the controller's worker thread and settle the
// dispatcher. Returns once the worker has left InFlight and the dispatcher has
// drained the worker-posted tasks (model-change clear + final-delta commit).
void DriveTurnAndSettle(ImGuiTestContext* ctx, AppController& app, AiAssistantController& ctrl,
                        const std::string& prompt) {
    // Mirror dispatchSend's UI-side turn-gen bump: the controller forwards this
    // value back through every dispatcher callback and the callbacks drop unless
    // g_ui.assistantTurnGen matches. Without the bump the final-delta commit
    // no-ops and history never repopulates.
    g_ui.assistantTurnGen += 1;
    const std::uint64_t turnGen = g_ui.assistantTurnGen;
    g_ui.assistantInFlight = true;

    const bool accepted = ctrl.Submit(turnGen, prompt, std::vector<AiContextBlock>());
    IM_CHECK(accepted);

    // Deterministic settle: yield the engine frame loop (which advances the app,
    // including the per-frame dispatcher drain) until the worker reports a
    // terminal state AND the in-flight flag has been cleared by the drained
    // final-delta task. Bounded so a regression fails fast instead of hanging.
    // This is a post-condition wait, not a fixed yield count.
    const int kMaxSettleFrames = 600;
    bool settled = false;
    for (int i = 0; i < kMaxSettleFrames; ++i) {
        // Explicitly drain on the UI thread too — belt-and-braces in case the
        // engine frame's own drain timing races the worker post.
        app.mainThreadDispatcher.Drain();
        const AiAssistantController::State st = ctrl.CurrentState();
        const bool workerDone = (st != AiAssistantController::State::InFlight);
        if (workerDone && !g_ui.assistantInFlight) {
            settled = true;
            break;
        }
        ctx->Yield();
    }
    // Final drain to flush any task posted on the last observed frame.
    app.mainThreadDispatcher.Drain();
    IM_CHECK(settled);
}

void RegisterModelChangeClearsHistoryVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiAssistant", "AssistantModelChange_ClearsHistoryAndSetsStrip");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("skip: SmatchetActiveUiTestAppController() returned nullptr");
            return;
        }
        if (!app->HasAiAssistantController()) {
            ctx->LogInfo("skip: AppController has no AiAssistantController in this build");
            return;
        }
        AiAssistantController& ctrl = app->GetAiAssistantController();

        // --- Arrange ------------------------------------------------------
        AiClientFactory::SetTestOverride(&MakeModelChangeStub);

        // Reset the assistant session to a known baseline, then seed one stub
        // assistant message so there is prior history for the model change to
        // clear.
        g_ui.assistantHistory.clear();
        g_ui.assistantHistoryRowIds.clear();
        g_ui.assistantStreamBuf.clear();
        g_ui.assistantLastError.clear();
        g_ui.assistantInFlight = false;
        // assistantHistoryHydrated stays false so the final-delta commit does
        // NOT enqueue SQLite persist ops (keeps the test off the cache layer).
        g_ui.assistantHistoryHydrated = false;

        AiMessage seeded;
        seeded.Role = "assistant";
        seeded.Content = "seeded-prior-turn";
        g_ui.assistantHistory.push_back(seeded);
        g_ui.assistantHistoryRowIds.push_back(-1);
        IM_CHECK_EQ(static_cast<int>(g_ui.assistantHistory.size()), 1);

        // --- Turn 1: Anthropic --------------------------------------------
        // First turn establishes the "anthropic|claude-sonnet-4-6" signature.
        // DetectModelChange never clears on an empty previous signature, so the
        // seeded message must survive this turn.
        PersistProvider(1 /* Anthropic */);
        DriveTurnAndSettle(ctx, *app, ctrl, "first turn under anthropic");

        IM_CHECK_STR_EQ(g_ui.assistantLastError.c_str(), "");
        // Seeded message + the turn-1 assistant reply both present (no clear).
        IM_CHECK_EQ(static_cast<int>(g_ui.assistantHistory.size()), 2);
        IM_CHECK_STR_EQ(g_ui.assistantHistory[0].Content.c_str(), "seeded-prior-turn");

        // --- Turn 2: DeepSeek (model changed) -----------------------------
        // Provider flips to DeepSeek → "deepseek|deepseek-chat" signature differs
        // → ResolveModelAndEffort posts the clear + the warning-strip string.
        PersistProvider(4 /* DeepSeek */);
        DriveTurnAndSettle(ctx, *app, ctrl, "second turn under deepseek");

        // PRIMARY oracle — the warning-strip string set at
        // AiAssistantController.cpp:348.
        IM_CHECK_STR_EQ(g_ui.assistantLastError.c_str(), "[model changed - chat cleared]");

        // SECONDARY oracle — prior history was cleared. The clear task runs
        // before the turn-2 final-delta commit (FIFO dispatcher: clear posted in
        // ResolveModelAndEffort, commit posted later during streaming), so after
        // settling, history holds ONLY the turn-2 assistant reply and the seeded
        // message is gone.
        IM_CHECK_EQ(static_cast<int>(g_ui.assistantHistory.size()), 1);
        IM_CHECK_STR_EQ(g_ui.assistantHistory[0].Role.c_str(), "assistant");
        IM_CHECK_STR_EQ(g_ui.assistantHistory[0].Content.c_str(), "ack");

        // --- Cleanup ------------------------------------------------------
        AiClientFactory::SetTestOverride(nullptr);
        g_ui.assistantHistory.clear();
        g_ui.assistantHistoryRowIds.clear();
        g_ui.assistantStreamBuf.clear();
        g_ui.assistantLastError.clear();
        g_ui.assistantInFlight = false;
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantModelChangeStripTests(ImGuiTestEngine* engine) {
    RegisterModelChangeClearsHistoryVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
