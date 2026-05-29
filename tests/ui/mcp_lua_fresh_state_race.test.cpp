// mcp_lua_fresh_state_race.test.cpp — bucket-E regression for the HIGH-severity
// cross-thread `lua_State` race fixed on fix/mcp-lua-fresh-state-race
// (commit 610b6aa5). Plan: docs/design/mcp-lua-fresh-state-race.md.
//
// THE RACE (pre-fix):
//   MCP `run_lua` snippet/script handlers + the registered-Lua-tool handler run
//   on httplib worker threads and previously executed against the SHARED main
//   `sol::state lua` member — the same lua_State the UI thread drives every
//   frame via DrawLuaWindows / Lua cell-providers. Two threads through one
//   lua_State is undefined behaviour; the realistic failure mode is Lua-heap
//   corruption (an interpreter allocation interleaved across both threads),
//   which AddressSanitizer surfaces as a heap-buffer-overflow / use-after-free.
//
// THE FIX (under test):
//   AppController::ExecuteLuaSnippetForMcp / ExecuteLuaScriptForMcp /
//   ExecuteLuaMcpTool now each build a FRESH per-call sol::state via
//   PrepareFreshLuaState (AppController_LuaBindings_Draw.cpp) — they never touch
//   the main `lua` state, so the UI thread and the MCP worker no longer share a
//   lua_State.
//
// THE TEST (real-AppController, two-thread, deterministic):
//   - UI thread (this is the bucket-E TestFunc thread): registers a real Lua
//     window on the main `lua` state and then drives the GENUINE DrawLuaWindows
//     path every frame via ctx->Yield() — the GuiFunc calls app.DrawLuaWindows()
//     which evaluates the window draw fn against the main `lua` state, allocating
//     Lua heap exactly as production does.
//   - Worker thread: fires the REAL app.ExecuteLuaSnippetForMcp(...) and
//     app.ExecuteLuaMcpTool(...) in a tight bounded loop. Each call builds a
//     fresh sol::state via PrepareFreshLuaState. Pre-fix these touched main
//     `lua`; post-fix they do not.
//   - A two-party spin barrier (atomic, no sleeps) releases both sides at the
//     same instant to MAXIMISE interleaving. Iterations are bounded; the worker
//     is always joined before the test returns.
//
// PASS CONDITION: built + run under AddressSanitizer (ninja-ui-test-asan-clang /
//   -msvc), the process exits with ZERO sanitizer reports. Pre-fix this body
//   races the Lua heap and ASan aborts; post-fix it is clean. The IM_CHECK
//   assertions below are liveness/sanity (both threads completed their bounded
//   work) — the sanitizer is the primary oracle.
//
// Drift warning — IF YOU CHANGE the fresh-state contract in
//   Source_Core/src/AppController_LuaBindings_Draw.cpp (ExecuteLuaSnippetForMcp /
//   ExecuteLuaMcpTool) or Source_Core/src/AppController_LuaBindings.cpp
//   (PrepareFreshLuaState / ReplayActiveSetupScripts), re-confirm this test still
//   drives both the worker fresh-state path AND the UI-thread main-`lua` path.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_LUA_AUTOMATION)

#include "AppController.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

// SmatchetActiveUiTestAppController — defined in
// Source_Core/src/Commands/Scenarios/UiTestScenario.cpp; returns the live
// AppController driving the bucket-E run.
AppController* SmatchetActiveUiTestAppController();

namespace {

// Bounded iteration counts — large enough that a pre-fix concurrent-lua_State
// build reliably interleaves a Lua-heap allocation across both threads (ASan
// then aborts), small enough that the run stays a sub-second one-shot under the
// instrumented build. Tuned alongside the Fast/NoThrottle engine config in
// UiTestScenario::OnStart.
constexpr int kWorkerIterations = 600;
constexpr int kUiFrames = 600;

// Distinct so a stray name collision in the shared registry is obvious.
const char* const kRaceProbeToolName = "__race_probe_tool";

// Two-party spin barrier: both threads arrive, the last to arrive releases both
// with a single release-store. No sleeps, no condvar — deterministic and cheap,
// and it removes the "worker finished before the UI loop even started" failure
// mode that would let the race go unexercised.
struct SpinBarrier {
    std::atomic<int> arrived{0};
    std::atomic<bool> released{false};

    void Arrive() { arrived.fetch_add(1, std::memory_order_acq_rel); }
    // Spin until `expected` parties have arrived, then release everyone.
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

// Worker-thread result: completion flag + per-call counts the UI side reads to
// confirm both bounded loops fully ran (liveness alongside the sanitizer oracle).
struct WorkerResult {
    std::atomic<int> snippetCalls{0};
    std::atomic<int> toolCalls{0};
    std::atomic<bool> snippetOkSeen{false};
    std::atomic<bool> done{false};
};

void RunWorker(AppController* app, SpinBarrier* barrier, WorkerResult* out) {
    barrier->Arrive();
    barrier->Wait();

    const nlohmann::json emptyArgs = nlohmann::json::object();
    for (int i = 0; i < kWorkerIterations; ++i) {
        // (1) run_lua snippet — fresh state + sandbox + load + execute. The
        //     canonical race exerciser; identical PrepareFreshLuaState path the
        //     fix introduced. Pre-fix this ran a sandbox over main `lua`.
        {
            std::string err;
            // Return a STRING ("4") rather than a bare number: McpLuaResultToString
            // hands back string returns verbatim, whereas a numeric return routes
            // through LuaToJson which encodes Lua numbers as JSON floats ("4.0").
            // The string form makes the liveness oracle robust to that encoding.
            const std::string ret = app->ExecuteLuaSnippetForMcp("return tostring(2 + 2)", emptyArgs, err);
            out->snippetCalls.fetch_add(1, std::memory_order_relaxed);
            if (err.empty() && ret == "4") {
                out->snippetOkSeen.store(true, std::memory_order_relaxed);
            }
        }

        // (2) registered-tool handler — fresh state + ReplayActiveSetupScripts +
        //     call-local register_tool override. The probe tool was registered
        //     ad-hoc on main `lua` (see GuiFunc), so the rebuild on the fresh
        //     state cleanly reports "not available" (documented degenerate path).
        //     Either way it builds + replays on a fresh state off the UI thread,
        //     which is the race-relevant code we are sanitizing.
        {
            std::string err;
            (void)app->ExecuteLuaMcpTool(kRaceProbeToolName, "{}", err);
            out->toolCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    out->done.store(true, std::memory_order_release);
}

// Per-test shared state, reachable from GuiFunc/TestFunc via ImGuiTest::UserData.
struct RaceTestState {
    AppController* app = nullptr;
    SpinBarrier barrier;
    WorkerResult worker;
    bool windowRegistered = false;
    bool probeToolRegistered = false;
};

RaceTestState g_raceState;

// Registers a real Lua window + an ad-hoc MCP tool on the MAIN `lua` state via
// the console-snippet path (ExecuteLuaConsoleSnippet runs against `lua`). The
// window draw fn does genuine recorder work each frame so DrawLuaWindows
// allocates Lua heap on the UI thread — the race partner for the worker.
void RegisterMainLuaDrivers(RaceTestState& s) {
    if (s.app == nullptr) {
        return;
    }
    if (!s.windowRegistered) {
        std::string err;
        std::string summary;
        // Non-trivial body: locals, a loop, table + string allocation, and
        // recorder calls — representative of a real ui.register_window draw fn
        // and enough interpreter allocation to make a shared-state race bite.
        const bool ok = s.app->ExecuteLuaConsoleSnippet("ui.register_window('__race_probe_window', function(draw)\n"
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
    if (!s.probeToolRegistered) {
        std::string err;
        std::string summary;
        // Populate the shared luaMcpTools_ registry so the worker's
        // ExecuteLuaMcpTool passes the fast-reject and proceeds into the
        // fresh-state rebuild path.
        const bool ok = s.app->ExecuteLuaConsoleSnippet(
            "mcp.register_tool({ name = '__race_probe_tool', description = 'race probe',\n"
            "                    parameters_json = '{}' }, function(p) return '{}' end)\n",
            err, summary);
        s.probeToolRegistered = ok;
    }
}

void RegisterFreshStateRaceVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "McpLuaRace", "FreshState_ConcurrentDrawLuaWindows");
    t->UserData = &g_raceState;

    // GuiFunc runs INSIDE the ImGui frame on the UI thread every yielded frame.
    // After the drivers are registered, calling app.DrawLuaWindows() here is the
    // exact production path (mirrors LuaConsolePlugin.cpp's draw-loop call) and
    // evaluates the registered window's draw fn against the main `lua` state.
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<RaceTestState*>(ctx->Test->UserData);
        if (s->app == nullptr) {
            return;
        }
        RegisterMainLuaDrivers(*s);
        // DrawLuaWindows itself issues ImGui::Begin/End for each registered Lua
        // window, so it must run within frame scope — which GuiFunc is.
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
        s->worker.snippetCalls.store(0, std::memory_order_relaxed);
        s->worker.toolCalls.store(0, std::memory_order_relaxed);
        s->worker.snippetOkSeen.store(false, std::memory_order_relaxed);
        s->worker.done.store(false, std::memory_order_relaxed);
        s->windowRegistered = false;
        s->probeToolRegistered = false;

        // Prime one frame so GuiFunc registers the window + probe tool on main
        // `lua` BEFORE the worker starts — the worker's ExecuteLuaMcpTool needs
        // the tool present in the shared registry to enter the rebuild path.
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(s->windowRegistered);
        IM_CHECK(s->probeToolRegistered);

        // Launch the worker, then release the barrier so both sides start their
        // bounded loops near-simultaneously.
        std::thread worker(RunWorker, s->app, &s->barrier, &s->worker);

        s->barrier.Arrive();            // UI thread is a barrier party too.
        s->barrier.ReleaseWhenReady(2); // wait for the worker, then release both.

        // Drive real frames (each Yield → GuiFunc → DrawLuaWindows on main `lua`)
        // while the worker concurrently builds fresh states. Stop once the worker
        // is done AND we have driven a representative number of frames.
        int frame = 0;
        while (frame < kUiFrames || !s->worker.done.load(std::memory_order_acquire)) {
            ctx->Yield();
            ++frame;
            // Safety valve: never spin frames unboundedly if the worker wedged.
            if (frame > kUiFrames * 4) {
                break;
            }
        }

        worker.join();

        // Liveness / sanity (the sanitizer is the real oracle). Both bounded
        // loops must have fully completed without a crash.
        IM_CHECK(s->worker.done.load(std::memory_order_acquire));
        IM_CHECK_EQ(s->worker.snippetCalls.load(std::memory_order_relaxed), kWorkerIterations);
        IM_CHECK_EQ(s->worker.toolCalls.load(std::memory_order_relaxed), kWorkerIterations);
        // The fresh-state snippet must actually evaluate `tostring(2+2)` → "4",
        // proving the worker path ran real Lua (not an early error-return).
        IM_CHECK(s->worker.snippetOkSeen.load(std::memory_order_relaxed));

        // Tidy: drop the probe window/tool from main `lua` so a re-queue starts
        // clean and the post-run teardown doesn't retain test closures.
        std::string err;
        std::string summary;
        s->app->ExecuteLuaConsoleSnippet("ui.unregister_window('__race_probe_window')\n", err, summary);
    };
}

} // namespace

extern "C" void SmatchetRegisterMcpLuaFreshStateRaceTests(ImGuiTestEngine* engine) {
    RegisterFreshStateRaceVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_LUA_AUTOMATION
