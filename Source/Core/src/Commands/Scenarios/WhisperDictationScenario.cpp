// WhisperDictationScenario — end-to-end regression gate for the whisper-dictation
// pipeline. See docs/plans/shipped/whisper-dictation.md § Phase G.
// Exercises the press → capture → transcribe → insert path WITHOUT mic
// hardware and WITHOUT an OpenAI key by installing a mock-transcription
// seam in WhisperPlugin (see Source/Plugins/Whisper/WhisperPlugin.h §
// `SetMockTranscription`). The seam diverts the hotkey worker thread to a
// LaunchBackgroundTask that sleeps for the requested delay and then posts
// the canned text into `g_dictationRouter.InsertIntoFocusedInputText` via
// `MainThreadDispatcher`. The scenario registers a test-only char buffer
// with the router before pressing, then polls the buffer each frame until
// the inserted text matches the canned mock — at which point IsDone returns
// true and OnFinish reports {passed, expected_text, observed_text}.
// Threading: scenario callbacks run on the UI thread inside the scenario
// runner's per-frame Tick. Workers and the MainThreadDispatcher drain are
// driven by the surrounding SmatchetUI::Draw frame, so each OnFrame sees
// the state mutations from the prior frame's worker post-back. No
// scenario-side dispatcher pumping is needed (and would deadlock).
// The TU is gated by source-list conditional inclusion in CMakeLists.txt
// (only added to CORE_SOURCES when SMATCHET_WITH_WHISPER=ON); the
// scenarioRunner_->RegisterFactory site in AppController.cpp wraps the
// factory call in `#if defined(SMATCHET_WITH_WHISPER)` so the OFF build
// has no dangling reference to MakeWhisperDictationScenario.

#include "Commands/Scenarios/IScenario.h"
#include "Ui/UiPerfRowsJson.h"

#include "Interfaces/IAppScenarioHost.h"
#include <nlohmann/json.hpp> // the scenario headers expose only json_fwd; this TU uses nlohmann::json directly.
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "DictationInsertionRouter.h"
#include "Logger.h"
#include "UiPerfMonitor.h"
#include "WhisperPlugin.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

// Buffer sized for the canonical mock text (~64 chars) plus a safety
// margin. Storage is file-scope static so it outlives the scenario instance
// — the router holds a raw pointer for the duration of registration, and
// OnFinish unregisters before the scenario unwinds. 256 bytes is far below
// any per-frame allocation we care about.
constexpr std::size_t kTestBufferCap = 256;
char g_testBuffer[kTestBufferCap] = {0};
int g_testCursor = 0;

// Default mock transcription. Picked so the assertion is unambiguous (no
// accidental match against an empty buffer or a typical real transcription).
const char* kDefaultMockText = "the quick brown fox jumps over the lazy dog";

} // namespace

class WhisperDictationScenario : public IScenario {
  public:
    std::string Name() const override { return "whisper-dictation-roundtrip"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        expectedText_ = args.value("text", std::string(kDefaultMockText));
        delayMs_ = (std::max)(0, args.value("delayMs", 50));
        frameLimit_ = (std::max)(60, args.value("frames", 300));
        pressFrame_ = (std::max)(1, args.value("pressFrame", 1));
        releaseFrame_ = (std::max)(pressFrame_ + 1, args.value("releaseFrame", pressFrame_ + 3));

        // Zero the buffer and register with the router. The router stores the
        // raw pointer; we must keep g_testBuffer alive until OnFinish runs and
        // calls UnregisterInputText. Static storage covers that lifetime.
        std::memset(g_testBuffer, 0, kTestBufferCap);
        g_testCursor = 0;
        g_dictationRouter.RegisterInputText(g_testBuffer, kTestBufferCap, &g_testCursor);

        // Arm the mock seam. The press / release worker pair will route through
        // the bypass branch in WhisperPlugin and post the canned text after the
        // requested delay.
        WhisperPlugin::MockTranscription mock;
        mock.text = expectedText_;
        mock.delay = std::chrono::milliseconds(delayMs_);
        WhisperPlugin::SetMockTranscription(mock);

        LOG_INFO("WhisperDictationScenario: armed mock (expected text size=%zu, pressFrame=%d, "
                 "releaseFrame=%d, delayMs=%d)",
                 expectedText_.size(), pressFrame_, releaseFrame_, delayMs_);
        // Press is now deferred to OnFrame so caller-supplied pressFrame_ values
        // actually take effect. Previously simulate-press was dispatched here
        // unconditionally on OnStart, which made pressFrame_ a no-op.
        state_ = State::Initial;
    }

    void OnFrame(IAppScenarioHost& app, int frameIndex) override {
        if (state_ == State::Initial && frameIndex >= pressFrame_) {
            // clang-format off
            // SMATCHET_DEVIATION(rule=duplication; reason=the simulate-press/simulate-release dispatch shell is a deliberate grandfathered twin of WhisperAiAssistantAutosendScenario's press/release frames — same command pair driven at different assert points, dispatched straight through the scenario-host facet the hook receives — not a real copy-paste; owner=orchestrator; revisit=when the two whisper scenarios share a dispatch helper)
            // clang-format on
            CommandContext ctx;
            ctx.ScenarioHost = &app;
            ctx.Source = CommandSource::Internal;
            const CommandResult pressRes =
                app.Commands().Dispatch("whisper.simulate-press", nlohmann::json::object(), ctx);
            if (pressRes.Error.Code != ErrorCode::None) {
                LOG_WARN("WhisperDictationScenario: simulate-press dispatch failed: %s",
                         pressRes.Error.Message.c_str());
                state_ = State::Asserted; // bail with passed_ = false
                return;
            }
            state_ = State::WaitingForReleaseFrame;
            return;
        }
        if (state_ == State::WaitingForReleaseFrame && frameIndex >= releaseFrame_) {
            // clang-format off
            // SMATCHET_DEVIATION(rule=duplication; reason=the simulate-press/simulate-release dispatch shell is a deliberate grandfathered twin of WhisperAiAssistantAutosendScenario's press/release frames — same command pair driven at different assert points, dispatched straight through the scenario-host facet the hook receives — not a real copy-paste; owner=orchestrator; revisit=when the two whisper scenarios share a dispatch helper)
            // clang-format on
            CommandContext ctx;
            ctx.ScenarioHost = &app;
            ctx.Source = CommandSource::Internal;
            const CommandResult releaseRes =
                app.Commands().Dispatch("whisper.simulate-release", nlohmann::json::object(), ctx);
            if (releaseRes.Error.Code != ErrorCode::None) {
                LOG_WARN("WhisperDictationScenario: simulate-release dispatch failed: %s",
                         releaseRes.Error.Message.c_str());
            }
            state_ = State::WaitingForInsertion;
            return;
        }
        if (state_ == State::WaitingForInsertion) {
            // The mock worker thread sleeps for `delayMs_` then posts onto the
            // MainThreadDispatcher; the dispatcher drains at the top of each
            // SmatchetUI::Draw frame, so the buffer mutation lands on a later
            // OnFrame call. Read the buffer (NUL-terminated by the router's
            // splice path); compare against the canned text.
            const std::string observed(g_testBuffer);
            if (!observed.empty()) {
                observedText_ = observed;
                passed_ = (observedText_ == expectedText_);
                state_ = State::Asserted;
                LOG_INFO("WhisperDictationScenario: observed text after %d frames "
                         "(passed=%s, observed='%s')",
                         frameIndex, passed_ ? "true" : "false", observedText_.c_str());
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
        out["passed"] = passed_;
        out["state"] = StateName(state_);
        out["press_frame"] = pressFrame_;
        out["release_frame"] = releaseFrame_;
        out["delay_ms"] = delayMs_;
        out["frame_limit"] = frameLimit_;
        // Perf-rows emit so `scripts/dev/perf-baseline.sh init whisper-dictation-roundtrip`
        // captures a baseline (per the 8-of-15 retrofit in
        // docs/self-improvement/categories/tooling.md). Pattern mirrors
        // PriorityGridScrollScenario::OnFinish.
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = UiPerfRowsToJson(rows);
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    enum class State {
        Initial,
        WaitingForReleaseFrame,
        WaitingForInsertion,
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
        g_dictationRouter.UnregisterInputText(g_testBuffer);
        std::memset(g_testBuffer, 0, kTestBufferCap);
        g_testCursor = 0;
    }

    State state_ = State::Initial;
    std::string expectedText_;
    std::string observedText_;
    bool passed_ = false;
    bool teardownDone_ = false;
    int delayMs_ = 50;
    int frameLimit_ = 300;
    int pressFrame_ = 1;
    int releaseFrame_ = 4;
};

} // namespace cmd
} // namespace smatchet

/// Factory function callable from AppController::Initialize. Wrapped in
/// `#if defined(SMATCHET_WITH_WHISPER)` at the call site so the OFF build
/// (which excludes this TU from CORE_SOURCES) has no dangling extern.
std::unique_ptr<smatchet::cmd::IScenario> MakeWhisperDictationScenario() {
    return std::make_unique<smatchet::cmd::WhisperDictationScenario>();
}
