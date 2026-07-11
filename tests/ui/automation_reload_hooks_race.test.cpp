// automation_reload_hooks_race.test.cpp — bucket-E regression for Issue #1693, extending
// the mcp_lua_fresh_state_race.test.cpp pattern to a distinct sibling hazard.
//
// THE RACE (pre-fix):
//   `automation.reload-hooks` (Source/Core/src/Commands/Builtin/BuiltinCommands_Automation.cpp)
//   called AppController::RunLuaSetupScript("SmatchetHooks.lua") directly from whatever thread
//   dispatched the command. CommandRegistry::Dispatch has no thread affinity, and the MCP
//   server dispatches commands from its own httplib worker thread — so an MCP client calling
//   automation.reload-hooks while the UI thread is mid-frame (DrawLuaWindows / Lua
//   cell-providers) raced two threads through the SAME shared `impl_->lua` sol::state. This
//   is a distinct hazard from the already-fixed mcp_lua_fresh_state_race bug (that one was
//   `run_lua` / registered-tool handlers, fixed by giving those a FRESH per-call sol::state).
//   A fresh-state fix does NOT work here: `RunLuaSetupScript`'s whole point is to re-register
//   `ui.register_global_action` / `ui.register_window` / etc. against the LIVE main state that
//   DrawLuaWindows evaluates every frame — a throwaway state's registrations evaporate the
//   instant the call returns and could never be invoked safely from the UI thread's state
//   (a sol::function is tied to the lua_State it was captured on).
//
// THE FIX (under test):
//   automation.reload-hooks now wraps its handler body in RunOnUiThreadAsCommandResult (the
//   established idiom already used by AppViewCommands.cpp / PaneCommands.cpp /
//   BuiltinCommands_Debug.cpp's dock.*/window.* commands): RegisterAutomationCommands gained
//   an IMainThreadPoster& parameter (mirroring RegisterDebugCommands /
//   RegisterScenarioCommands), and the reload-hooks handler now marshals the
//   RunLuaSetupScript call onto the UI thread instead of running it inline on whatever thread
//   dispatched the command.
//
// THE TEST (real-AppController, two-thread, deterministic):
//   - UI thread (this is the bucket-E TestFunc thread): registers a real Lua window on the
//     main `lua` state and then drives the GENUINE DrawLuaWindows path every frame via
//     ctx->Yield() (same as mcp_lua_fresh_state_race.test.cpp) — this also drains
//     app.mainThreadDispatcher every real frame (SmatchetUI::Draw runs alongside GuiFunc in
//     the live bucket-E process), which is what lets the worker's UI-thread hop complete.
//   - Worker thread: dispatches the REAL `automation.reload-hooks` command via
//     app->Commands().Dispatch(...) with CommandSource::Mcp (mirroring the actual MCP
//     dispatch path) in a tight bounded loop, concurrently with the UI thread's frames.
//   - A two-party spin barrier releases both sides at the same instant to maximise
//     interleaving, same as the sibling test.
//
// PASS CONDITION: built + run under AddressSanitizer, the process exits with ZERO sanitizer
//   reports, and every dispatched command reports Ok==true with the expected "reloaded" field
//   (proving the reload actually landed on the main state, not just that the call returned).
//   Pre-fix this body races the Lua heap; post-fix the UI-thread hop serializes every
//   RunLuaSetupScript call against DrawLuaWindows so there genuinely are never two threads in
//   the lua_State at once.
//
// Drift warning — IF YOU CHANGE the automation.reload-hooks UI-thread-hop contract
//   (Source/Core/src/Commands/Builtin/BuiltinCommands_Automation.cpp) or RunLuaSetupScript's
//   effect on the main `lua` state, re-confirm this test still drives both the worker
//   Dispatch(...) path AND the UI-thread main-`lua` DrawLuaWindows path concurrently.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_LUA_AUTOMATION)

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

// SmatchetActiveUiTestAppController — defined in
// Source/Core/src/Commands/Scenarios/UiTestScenario.cpp; returns the live
// AppController driving the bucket-E run.
AppController* SmatchetActiveUiTestAppController();

namespace {

// Bounded iteration counts. Each automation.reload-hooks dispatch round-trips through the
// UI thread (one frame at ~60Hz+ under the bucket-E harness), so this loop is intentionally
// far smaller than the fresh-state test's 600 — a blocking cross-thread hop per iteration
// keeps the test itself sub-second while still reliably interleaving several UI-thread
// DrawLuaWindows frames against several worker dispatches.
constexpr int kWorkerIterations = 40;
constexpr int kUiFrames = 200;

// Two-party spin barrier: both threads arrive, the last to arrive releases both with a single
// release-store. Mirrors mcp_lua_fresh_state_race.test.cpp's SpinBarrier exactly.
struct SpinBarrier {
    std::atomic<int> arrived{0};
    std::atomic<bool> released{false};

    void Arrive() { arrived.fetch_add(1, std::memory_order_acq_rel); }
    void ReleaseWhenReady(int expected) {
        while (arrived.load(std::memory_order_acquire) < expected) {
            // busy-wait; both parties arrive within microseconds.
        }
        released.store(true, std::memory_order_release);
    }
    void Wait() const {
        while (!released.load(std::memory_order_acquire)) {
            // busy-wait for the release-store.
        }
    }
};

struct WorkerResult {
    std::atomic<int> dispatchCalls{0};
    std::atomic<int> okSeen{0};
    std::atomic<bool> done{false};
};

void RunWorker(AppController* app, SpinBarrier* barrier, WorkerResult* out) {
    barrier->Arrive();
    barrier->Wait();

    for (int i = 0; i < kWorkerIterations; ++i) {
        // Real command dispatch, CommandSource::Mcp — mirrors McpPlugin.cpp calling
        // Commands().Dispatch(...) directly from its httplib worker thread. Pre-fix this
        // ran RunLuaSetupScript inline on THIS thread against the shared main `lua`, racing
        // the UI thread's concurrent DrawLuaWindows. Post-fix the handler hops to the UI
        // thread and this call blocks on that hop's future.
        smatchet::cmd::CommandContext ctx;
        ctx.Source = smatchet::cmd::CommandSource::Mcp;
        const smatchet::cmd::CommandResult result =
            app->Commands().Dispatch("automation.reload-hooks", nlohmann::json::object(), ctx);
        out->dispatchCalls.fetch_add(1, std::memory_order_relaxed);
        if (result.Ok) {
            out->okSeen.fetch_add(1, std::memory_order_relaxed);
        }
    }
    out->done.store(true, std::memory_order_release);
}

struct RaceTestState {
    AppController* app = nullptr;
    SpinBarrier barrier;
    WorkerResult worker;
    bool windowRegistered = false;
};

RaceTestState g_reloadHooksRaceState;

// Registers a real Lua window on the MAIN `lua` state via the console-snippet path (same
// helper shape as mcp_lua_fresh_state_race.test.cpp) so DrawLuaWindows allocates Lua heap
// on the UI thread every frame — the race partner for the worker's concurrent reload-hooks
// dispatch (which itself re-runs SmatchetHooks.lua against that same main state).
void RegisterMainLuaDriver(RaceTestState& s) {
    if (s.app == nullptr || s.windowRegistered) {
        return;
    }
    std::string err;
    std::string summary;
    const bool ok = s.app->ExecuteLuaConsoleSnippet("ui.register_window('__reload_hooks_race_window', function(draw)\n"
                                                    "  local total = 0\n"
                                                    "  for i = 1, 16 do total = total + i end\n"
                                                    "  local t = { a = total, b = 'race-' .. tostring(total) }\n"
                                                    "  draw:text('sum=' .. tostring(t.a))\n"
                                                    "  draw:text_unformatted(t.b)\n"
                                                    "  draw:separator()\n"
                                                    "end)\n",
                                                    err, summary);
    s.windowRegistered = ok;
}

void RegisterAutomationReloadHooksRaceVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "AutomationReloadHooksRace", "ConcurrentReloadHooks_ConcurrentDrawLuaWindows");
    t->UserData = &g_reloadHooksRaceState;

    // GuiFunc runs INSIDE the ImGui frame on the UI thread every yielded frame — the exact
    // production DrawLuaWindows path (mirrors LuaConsolePlugin.cpp's draw-loop call).
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<RaceTestState*>(ctx->Test->UserData);
        if (s->app == nullptr) {
            return;
        }
        RegisterMainLuaDriver(*s);
        s->app->DrawLuaWindows();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<RaceTestState*>(ctx->Test->UserData);
        s->app = SmatchetActiveUiTestAppController();
        if (s->app == nullptr) {
            ctx->LogWarning("skip: SmatchetActiveUiTestAppController() returned nullptr");
            return;
        }

        // Reset per-run shared state (tests may be re-queued in one process).
        s->barrier.arrived.store(0, std::memory_order_relaxed);
        s->barrier.released.store(false, std::memory_order_relaxed);
        s->worker.dispatchCalls.store(0, std::memory_order_relaxed);
        s->worker.okSeen.store(0, std::memory_order_relaxed);
        s->worker.done.store(false, std::memory_order_relaxed);
        s->windowRegistered = false;

        // Prime one frame so GuiFunc registers the window on main `lua` BEFORE the worker
        // starts dispatching.
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(s->windowRegistered);

        std::thread worker(RunWorker, s->app, &s->barrier, &s->worker);

        s->barrier.Arrive(); // UI thread is a barrier party too.
        s->barrier.ReleaseWhenReady(2);

        // Drive real frames (each Yield -> GuiFunc -> DrawLuaWindows on main `lua`, PLUS the
        // real SmatchetUI::Draw's MainThreadDispatcher::Drain() elsewhere in the same live
        // process loop) while the worker concurrently dispatches automation.reload-hooks.
        // Stop once the worker is done AND a representative number of frames have run.
        int frame = 0;
        while (frame < kUiFrames || !s->worker.done.load(std::memory_order_acquire)) {
            ctx->Yield();
            ++frame;
            // Safety valve: never spin frames unboundedly if the worker wedged (e.g. a
            // regression that reintroduces the deadlock/hang class documented in
            // MainThreadDispatch.h's DR15 note).
            if (frame > kUiFrames * 8) {
                break;
            }
        }

        // A worker that's still not `done` here means the safety valve tripped — the
        // UI-thread hop wedged (the exact regression this test exists to catch). Log it
        // loudly BEFORE the blocking join() below: if the wedge is real, join() itself
        // can't be made non-blocking (no-detach is an absolute rule), so the actual
        // CI backstop is the process-level wall-clock timeout in
        // scripts/dev/test-ui-automation-reload-hooks-race.sh (SMATCHET_RUN_TIMEOUT_SECS)
        // — this log line is what a human sees in the truncated output once that timeout
        // kills the process (CodeRabbit PR #1695 review).
        if (!s->worker.done.load(std::memory_order_acquire)) {
            ctx->LogError("automation.reload-hooks worker did not complete within the frame "
                          "budget — the UI-thread hop appears wedged; joining now may block "
                          "until the process-level test timeout kills it.");
        }

        worker.join();

        // Liveness / sanity (the sanitizer is the real oracle). Both bounded loops must have
        // fully completed without a crash or a wedged UI-thread hop.
        IM_CHECK(s->worker.done.load(std::memory_order_acquire));
        IM_CHECK_EQ(s->worker.dispatchCalls.load(std::memory_order_relaxed), kWorkerIterations);
        // Every dispatch must have actually succeeded — proves the UI-thread hop landed the
        // reload against the live main state each time, not merely that Dispatch() returned.
        IM_CHECK_EQ(s->worker.okSeen.load(std::memory_order_relaxed), kWorkerIterations);

        // Tidy: drop the probe window from main `lua` so a re-queue starts clean.
        std::string err;
        std::string summary;
        s->app->ExecuteLuaConsoleSnippet("ui.unregister_window('__reload_hooks_race_window')\n", err, summary);
    };
}

} // namespace

extern "C" void SmatchetRegisterAutomationReloadHooksRaceTests(ImGuiTestEngine* engine) {
    RegisterAutomationReloadHooksRaceVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_LUA_AUTOMATION
