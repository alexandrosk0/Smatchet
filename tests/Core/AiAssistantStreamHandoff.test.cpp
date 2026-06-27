// AiAssistantStreamHandoff — TSan pass over the AiAssistant streaming worker→UI hand-off (slice
// 2c of docs/plans/active/tsan-imgui-linked-target.md; the playbook's "AI streaming cancel/submit
// race" candidate). Slices 2a/2b decoupled AiAssistantController from AppController and g_ui, so it
// now links headless: depends only on MainThreadDispatcher + IAiAssistantUiState (a fake here) and
// a stub IAiClient (via AiClientFactory::SetTestOverride + the FakeAiClientFactory stub that omits
// the real cpr clients).
//
// What ThreadSanitizer instruments: the controller's WORKER thread runs RunRequest → SendStreaming
// (the stub fires onDelta/onError on the worker), each callback PostToMainThread()-s a lambda; the
// test "UI" thread spins Drain() and runs those lambdas, which mutate the fake UI-state. The
// hand-off is race-free ONLY if the dispatcher serialises the posts onto the draining thread and
// the fake UI-state is touched on that thread alone — exactly the production marshalling contract.
// It also exercises the controller's internal cross-thread state (pending_/mutex_, state_ atomic,
// currentCancel_ swap) across Submit + the destructor's shutdown-join.
//
// The turn need not reach the happy stream path to be meaningful: a headless config that errors
// early still drives the worker → MakeOnError → PostToMainThread, so the same hand-off surface is
// instrumented either way. Assertions are intentionally weak (no crash, consistent post-drain
// state) — TSan-cleanliness is the real assertion.

#include "AiAssistantController.h"

#if defined(SMATCHET_WITH_AI)

#include "../support/FakeAiAssistantUiState.h"
#include "../support/StubAiClient.h"

#include "AiClientFactory.h"
#include "AiTypes.h"
#include "MainThreadDispatcher.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Installed via AiClientFactory::SetTestOverride — every MakeAiClient() call returns a fresh stub
// that streams a few deltas on the worker thread (no cpr, no network).
std::unique_ptr<IAiClient> MakeStub(AiProvider /*provider*/) {
    smatchet_tests::StubAiClientScript script;
    script.ProviderName = "stub";
    script.DeltaSequence = {"hel", "lo ", "wor", "ld"};
    script.PerDeltaSleepMs = 1;
    return std::unique_ptr<IAiClient>(new smatchet_tests::StubAiClient(script));
}

} // namespace

TEST_CASE("AiAssistantController: streaming worker→dispatcher→UI-state hand-off is race-free") {
    AiClientFactory::SetTestOverride(&MakeStub);

    MainThreadDispatcher dispatcher;
    smatchet_tests::FakeAiAssistantUiState ui;
    ui.hydrated = false;          // skip the SQLite persist enqueue path (no LocalCacheManager wired)
    ui.turnGen = 1;               // match Submit's turnGen so posted callbacks are not dropped as stale

    {
        AiAssistantController controller(dispatcher, ui);

        // The "UI" thread: spin Drain() so the worker's PostToMainThread() lambdas run (and mutate
        // `ui`) concurrently with the worker producing them. `ui` is touched ONLY here + after the
        // join below — never from the worker directly — mirroring the g_ui UI-thread-only contract.
        std::atomic<bool> stopDrain(false);
        std::thread drainer([&]() {
            while (!stopDrain.load(std::memory_order_acquire)) {
                dispatcher.Drain();
                std::this_thread::yield();
            }
        });

        controller.Submit(1, "hello", std::vector<AiContextBlock>());

        // Let the worker run its turn (stub stream ~4 ms; generous slack for CI jitter).
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        stopDrain.store(true, std::memory_order_release);
        drainer.join();
        dispatcher.Drain(); // sweep anything posted after the drainer stopped
    } // controller dtor: flips its cancel atom + joins the worker (bounded) — no race, no deadlock

    AiClientFactory::SetTestOverride(nullptr);

    // TSan-cleanliness is the assertion; these guard against a crash / wildly-inconsistent state.
    CHECK(ui.history.size() == ui.historyRowIds.size()); // the two parallel vectors stay in lock-step
    CHECK(dispatcher.QueueLen() == 0u);
}

TEST_CASE("AiAssistantController: Submit then Cancel racing the worker is race-free") {
    AiClientFactory::SetTestOverride(&MakeStub);

    MainThreadDispatcher dispatcher;
    smatchet_tests::FakeAiAssistantUiState ui;
    ui.turnGen = 7;

    {
        AiAssistantController controller(dispatcher, ui);

        std::atomic<bool> stopDrain(false);
        std::thread drainer([&]() {
            while (!stopDrain.load(std::memory_order_acquire)) {
                dispatcher.Drain();
                std::this_thread::yield();
            }
        });

        controller.Submit(7, "race", std::vector<AiContextBlock>());
        // Cancel from this thread while the worker streams — exercises currentCancel_ (shared_ptr
        // swap on Submit vs store on Cancel vs load on the worker) + the state_ transitions.
        controller.Cancel();

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stopDrain.store(true, std::memory_order_release);
        drainer.join();
        dispatcher.Drain();
    }

    AiClientFactory::SetTestOverride(nullptr);
    CHECK(ui.history.size() == ui.historyRowIds.size());
    CHECK(dispatcher.QueueLen() == 0u);
}

#endif // SMATCHET_WITH_AI
