// WhisperAiAssistantAutosendScenario — end-to-end regression gate for the
// fix at bf9ce67f (`ReloadUserBuf so ImGui rereads spliced buffer`).
// The fix lives across the router + AI Assistant TU pair: the router records
// the focused widget's ItemId in `pendingReloadItemId_` whenever a splice lands
// while the widget is active, and the AI Assistant draw drains that and calls
// `ImGuiInputTextState::ReloadUserBufAndMoveToEnd()` before the next
// InputTextMultiline. Without it, ImGui silently overwrites the freshly-spliced
// `s_inputCharBuf` with its stale internal `state->TextA`.
// The pre-existing `whisper-dictation-roundtrip` scenario can't cover this
// path: it registers its own static buffer (NOT the AI Assistant chat input)
// and never focuses the AI input as an ImGui widget. This scenario closes the
// gap by composing the production registration surface that the AI Assistant
// panel uses:
//   1. `RegisterAiAssistantInputText(buf, cap, cursor)` — panel-level idempotent
//      mirror, sets `aiAssistantBuf_`.
//   2. `RegisterInputTextWithItemId(buf, cap, cursor, NON_ZERO_ID)` — wrapper-
//      level register-with-real-ItemId, so `IsFocusedTargetAiAssistant()`
//      returns true and the splice records a non-zero pending reload id.
//   3. `SetAiAssistantSendCallback(...)` — observes the auto-send trigger.
//   4. `ConfigManager::Save(cfg)` with `WhisperAutoSendOnPunctuation=true`,
//      restored on teardown so the scenario doesn't leak into user preferences.
// Then drives press → release with `SetMockTranscription` carrying a sentence-
// ending text and asserts:
//   - The buf received the mock text (splice landed).
//   - `ConsumePendingReloadItemId()` returned the registered ItemId at least
//     once during the post-insert frames (reload was armed).
//   - The auto-send callback was invoked (cell 3 — auto-send fired).
//   - The router's transcribing flag toggled true then back to false (cell 4 at
//     router level — the menu-bar indicator's source of truth).
// Threading: callbacks run on the UI thread inside the scenario runner's per-
// frame Tick. Workers + MainThreadDispatcher drain are driven by the
// surrounding SmatchetUI::Draw frame, identical to WhisperDictationScenario.

#include "Commands/Scenarios/IScenario.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "DictationInsertionRouter.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include "UiPerfMonitor.h"
#include "WhisperPlugin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

// File-scope static so the buffer outlives the scenario instance — the router
// holds a raw pointer for the duration of registration, and teardown
// unregisters before unwind. Sized to comfortably hold the canonical mock
// text plus auto-send + clear behaviour.
constexpr std::size_t kAutosendBufferCap = 512;
char g_autosendBuffer[kAutosendBufferCap] = {0};
int g_autosendCursor = 0;

// Synthetic item id — non-zero so `IsFocusedTargetAiAssistant()` returns true
// and the splice arms the pending-reload slot. Chosen to be obviously
// scenario-owned in debug logs.
constexpr unsigned int kFakeAiAssistantItemId = 0xA1A551ADu;

// Default text — ends with '.' so the auto-send punctuation gate fires.
const char* kDefaultAutosendText = "test the AI assistant auto send flow.";

std::atomic<int> g_autoSendCallbackFireCount{0};
std::atomic<unsigned int> g_pendingReloadIdLastSeen{0};
std::atomic<bool> g_observedTranscribingTrue{false};

} // namespace

class WhisperAiAssistantAutosendScenario : public IScenario {
  public:
    std::string Name() const override { return "whisper-ai-assistant-autosend"; }

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& outErr) override {
        expectedText_ = args.value("text", std::string(kDefaultAutosendText));
        delayMs_ = (std::max)(0, args.value("delayMs", 50));
        frameLimit_ = (std::max)(120, args.value("frames", 360));
        pressFrame_ = (std::max)(1, args.value("pressFrame", 1));
        releaseFrame_ = (std::max)(pressFrame_ + 1, args.value("releaseFrame", pressFrame_ + 3));

        // Snapshot + force the auto-send-on-punctuation pref so the
        // post-insertion gate fires. Restored in Teardown so user prefs are
        // not mutated permanently. ConfigManager::Save invalidates the
        // in-memory cache the WhisperPlugin re-reads inside the post-back, so
        // the new value is visible.
        const TrackerConfig cfgBefore = ConfigManager::Load();
        autoSendWasEnabled_ = cfgBefore.WhisperAutoSendOnPunctuation;
        if (!autoSendWasEnabled_) {
            TrackerConfig cfgForce = cfgBefore;
            cfgForce.WhisperAutoSendOnPunctuation = true;
            ConfigManager::Save(cfgForce);
        }

        // Force the AI Assistant panel closed for the duration of the scenario
        // so SmatchetDrawAiAssistantPanel doesn't (a) replace aiAssistantBuf_
        // with its TU-local s_inputCharBuf pointer and (b) drain the pending-
        // reload slot before our OnFrame can observe it. Restored in Teardown.
        // SMATCHET_WITH_AI guard: the AI side-panel state lives behind
        // `#if defined(SMATCHET_WITH_AI)` in SmatchetUiSession.h:174 — DX12 /
        // Unreal builds compile this TU with SMATCHET_WITH_AI undefined per
        // CMakeLists.txt:762-766, so the field is absent and the suppression
        // is a no-op (there's no panel to suppress on that target anyway).
#if defined(SMATCHET_WITH_AI)
        prevAssistantPanelOpen_ = g_ui.assistantPanelOpen;
        g_ui.assistantPanelOpen = false;
#endif

        // Reset all observers BEFORE wiring callbacks so the count starts at 0
        // and there's no race with a prior scenario's leftover signal.
        g_autoSendCallbackFireCount.store(0, std::memory_order_release);
        g_pendingReloadIdLastSeen.store(0, std::memory_order_release);
        g_observedTranscribingTrue.store(false, std::memory_order_release);

        // Zero the buffer + register on BOTH the AI flavour and the
        // wrapper flavour. The AI flavour sets aiAssistantBuf_ (cheap
        // identity check for "is this the AI Assistant?"); the wrapper-
        // with-ItemId registration is what makes IsFocusedTargetAiAssistant()
        // return true (entries_.front().ItemId != 0 + buf match).
        std::memset(g_autosendBuffer, 0, kAutosendBufferCap);
        g_autosendCursor = 0;
        g_dictationRouter.RegisterAiAssistantInputText(g_autosendBuffer, kAutosendBufferCap, &g_autosendCursor);
        g_dictationRouter.RegisterInputTextWithItemId(g_autosendBuffer, kAutosendBufferCap, &g_autosendCursor,
                                                      kFakeAiAssistantItemId);

        // Auto-send hook — fires on the UI thread when the post-insertion
        // gate decides to dispatch. The scenario lambda is a strict observer:
        // bumps the counter and returns. No re-entry into router state.
        g_dictationRouter.SetAiAssistantSendCallback(
            []() { g_autoSendCallbackFireCount.fetch_add(1, std::memory_order_acq_rel); });

        // Arm the mock seam — the press / release worker pair will route
        // through the mock branch and call the post-insertion hooks
        // (Transcribing flag + auto-send) just like the real path.
        WhisperPlugin::MockTranscription mock;
        mock.text = expectedText_;
        mock.delay = std::chrono::milliseconds(delayMs_);
        WhisperPlugin::SetMockTranscription(mock);

        LOG_INFO("WhisperAiAssistantAutosendScenario: armed mock (text='%s', delay=%dms, "
                 "pressFrame=%d, releaseFrame=%d)",
                 expectedText_.c_str(), delayMs_, pressFrame_, releaseFrame_);
        state_ = State::Initial;
        (void)outErr;
        (void)app;
    }

    void OnFrame(IAppScenarioHost& app, int frameIndex) override {
        // Always poll the transcribing flag — once we've seen it true, latch
        // for the cell-4 assertion regardless of subsequent toggles.
        if (g_dictationRouter.IsTranscribing()) {
            g_observedTranscribingTrue.store(true, std::memory_order_release);
        }
        // Always poll the pending-reload slot — the production AI Assistant
        // draw drains it via ConsumePendingReloadItemId. We mimic that drain
        // so the assertion is "did the slot ever surface our id?" instead of
        // "is it set right now". The scenario does NOT call
        // ImGuiInputTextState::ReloadUserBufAndMoveToEnd because there's no
        // real InputTextMultiline drawing this buf — the scenario's buf is
        // ours, not ImGui's, so no reload is needed.
        const unsigned int pendingId = g_dictationRouter.ConsumePendingReloadItemId();
        if (pendingId != 0) {
            g_pendingReloadIdLastSeen.store(pendingId, std::memory_order_release);
        }

        if (state_ == State::Initial && frameIndex >= pressFrame_) {
            // Safe downcast: the runner only ever drives scenarios with the owning AppController
            // (sole IAppScenarioHost implementer); CommandContext::App needs the concrete type.
            AppController& concrete = static_cast<AppController&>(app);
            CommandContext ctx;
            ctx.App = &concrete;
            ctx.Source = CommandSource::Internal;
            const CommandResult pressRes =
                concrete.Commands().Dispatch("whisper.simulate-press", nlohmann::json::object(), ctx);
            if (pressRes.Error.Code != ErrorCode::None) {
                LOG_WARN("WhisperAiAssistantAutosendScenario: simulate-press failed: %s",
                         pressRes.Error.Message.c_str());
                state_ = State::Asserted;
                return;
            }
            state_ = State::WaitingForReleaseFrame;
            return;
        }
        if (state_ == State::WaitingForReleaseFrame && frameIndex >= releaseFrame_) {
            // Safe downcast: the runner only ever drives scenarios with the owning AppController
            // (sole IAppScenarioHost implementer); CommandContext::App needs the concrete type.
            AppController& concrete = static_cast<AppController&>(app);
            CommandContext ctx;
            ctx.App = &concrete;
            ctx.Source = CommandSource::Internal;
            const CommandResult releaseRes =
                concrete.Commands().Dispatch("whisper.simulate-release", nlohmann::json::object(), ctx);
            if (releaseRes.Error.Code != ErrorCode::None) {
                LOG_WARN("WhisperAiAssistantAutosendScenario: simulate-release failed: %s",
                         releaseRes.Error.Message.c_str());
            }
            state_ = State::WaitingForInsertion;
            return;
        }
        if (state_ == State::WaitingForInsertion) {
            const std::string observed(g_autosendBuffer);
            if (!observed.empty()) {
                observedText_ = observed;
                state_ = State::WaitingForAutoSend;
            }
        }
        if (state_ == State::WaitingForAutoSend) {
            // Auto-send fires inside the same post-back as the splice, so we
            // expect to see the counter bumped on this frame or the next.
            // Give it a small budget so dispatcher drain ordering is forgiving.
            if (g_autoSendCallbackFireCount.load(std::memory_order_acquire) > 0) {
                state_ = State::Asserted;
                LOG_INFO("WhisperAiAssistantAutosendScenario: auto-send fired at frame %d "
                         "(observed='%s', pendingReloadIdSeen=0x%X, transcribingObserved=%d)",
                         frameIndex, observedText_.c_str(), g_pendingReloadIdLastSeen.load(std::memory_order_acquire),
                         g_observedTranscribingTrue.load(std::memory_order_acquire) ? 1 : 0);
            }
        }
    }

    bool IsDone(int frameIndex) const override { return state_ == State::Asserted || frameIndex >= frameLimit_; }

    void OnCancel(IAppScenarioHost& /*app*/) override { Teardown(); }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        Teardown();
        nlohmann::json out;
        out["scenario"] = Name();
        out["expected_text"] = expectedText_;
        out["observed_text"] = observedText_;
        const bool textMatch = (observedText_ == expectedText_);
        const int autoSendFires = g_autoSendCallbackFireCount.load(std::memory_order_acquire);
        const unsigned int pendingId = g_pendingReloadIdLastSeen.load(std::memory_order_acquire);
        const bool sawTranscribing = g_observedTranscribingTrue.load(std::memory_order_acquire);
        const bool transcribingCleared = !g_dictationRouter.IsTranscribing();

        out["text_match"] = textMatch;
        out["auto_send_fires"] = autoSendFires;
        out["pending_reload_id_seen"] = pendingId;
        out["pending_reload_id_matches"] = (pendingId == kFakeAiAssistantItemId);
        out["observed_transcribing_true"] = sawTranscribing;
        out["transcribing_cleared"] = transcribingCleared;
        out["state"] = StateName(state_);

        // Cell 3 = text_match + auto_send_fires >= 1.
        // Reload-arm cell = pending_reload_id_matches.
        // Cell 4 (router-level) = observed_transcribing_true + transcribing_cleared.
        out["passed"] = textMatch && autoSendFires >= 1 && pendingId == kFakeAiAssistantItemId && sawTranscribing &&
                        transcribingCleared;
        // Perf-rows emit so `scripts/dev/perf-baseline.sh init
        // whisper-ai-assistant-autosend` captures a baseline (per the 8-of-15
        // retrofit in docs/self-improvement/categories/tooling.md). Pattern
        // mirrors PriorityGridScrollScenario::OnFinish.
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = nlohmann::json::array();
        std::transform(rows.begin(), rows.end(), std::back_inserter(rowsJson), [](const UiPerfRow& r) {
            return nlohmann::json{
                {"name", r.name},
                {"lastTotalMs", r.lastTotalMs},
                {"avgPerCallMs", r.avgPerCallMs},
                {"maxMs", r.maxMs},
                {"calls", r.calls},
                {"emaAvgMs", r.emaAvgMs},
                {"p99Ms", r.p99Ms},
            };
        });
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    enum class State {
        Initial,
        WaitingForReleaseFrame,
        WaitingForInsertion,
        WaitingForAutoSend,
        Asserted,
    };

    static const char* StateName(State s) {
        switch (s) {
        case State::Initial:
            return "Initial";
        case State::WaitingForReleaseFrame:
            return "WaitingForReleaseFrame";
        case State::WaitingForInsertion:
            return "WaitingForInsertion";
        case State::WaitingForAutoSend:
            return "WaitingForAutoSend";
        case State::Asserted:
            return "Asserted";
        }
        return "Unknown";
    }

    void Teardown() {
        if (teardownDone_) {
            return;
        }
        teardownDone_ = true;
        WhisperPlugin::ClearMockTranscription();
        // Drop both registrations. Order matters only for the shadow-clear
        // contract — both calls target the same buf, so the last
        // UnregisterInputText clears the shadow.
        g_dictationRouter.UnregisterInputText(g_autosendBuffer);
        g_dictationRouter.UnregisterInputText(g_autosendBuffer);
        // Best-effort drop of the auto-send callback. The router stores
        // std::function; assigning an empty target prevents a later
        // unrelated TriggerAiAssistantSend from re-entering our static
        // observer after the scenario has unwound.
        g_dictationRouter.SetAiAssistantSendCallback({});
        std::memset(g_autosendBuffer, 0, kAutosendBufferCap);
        g_autosendCursor = 0;
        // Restore the pref to its pre-scenario value. If it was already true,
        // the scenario didn't touch disk — skip the redundant write.
        if (!autoSendWasEnabled_) {
            TrackerConfig cfgRestore = ConfigManager::Load();
            cfgRestore.WhisperAutoSendOnPunctuation = false;
            ConfigManager::Save(cfgRestore);
        }
        // Restore the panel-open flag to whatever state it had before the
        // scenario started. Without this, opening the AI Assistant panel
        // from the menu after running this scenario in a long-running
        // dogfood session would silently fail (the flag would be stuck false).
#if defined(SMATCHET_WITH_AI)
        g_ui.assistantPanelOpen = prevAssistantPanelOpen_;
#endif
    }

    State state_ = State::Initial;
    std::string expectedText_;
    std::string observedText_;
    bool teardownDone_ = false;
    bool autoSendWasEnabled_ = false;
#if defined(SMATCHET_WITH_AI)
    bool prevAssistantPanelOpen_ = false;
#endif
    int delayMs_ = 50;
    int frameLimit_ = 360;
    int pressFrame_ = 1;
    int releaseFrame_ = 4;
};

} // namespace cmd
} // namespace smatchet

/// Factory function — registered in AppController.cpp::Initialize behind the
/// same `#if defined(SMATCHET_WITH_WHISPER)` gate as the existing
/// `MakeWhisperDictationScenario` factory.
std::unique_ptr<smatchet::cmd::IScenario> MakeWhisperAiAssistantAutosendScenario() {
    return std::make_unique<smatchet::cmd::WhisperAiAssistantAutosendScenario>();
}
