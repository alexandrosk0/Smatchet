# Ui subsystem — agent rules

Scoped rules for `Source/Core/src/Ui/` (all ImGui render code). Global rules stay in the root [`AGENTS.md`](../../../../AGENTS.md). Draw-function decomposition pattern: [`docs/guides/imgui-draw-pattern.md`](../../../../docs/guides/imgui-draw-pattern.md).

`Ui/` is a **light** lint zone (root `AGENTS.md` § Tiered enforcement zones) — existing inline exemptions apply — but it hosts the richest UX-pillar checklist, because every freeze the user feels originates on this thread.

## UI-thread non-blocking (Pillar 2)

Flag any of these when reachable from `SmatchetUI::Draw` or any ImGui render path. These are **correctness** issues, not "performance" — they cause visible hitches and a CRITICAL review finding:

- `cpr::Get` / `cpr::Post` / `cpr::Put` / `cpr::Delete` directly in render code — must go through `TrackerHttpClient` posted to a worker thread.
- `SQLite::Database` calls inline in a render frame — the work must run on a worker thread; post only the result back via `MainThreadDispatcher::PostToMainThread`, and chunk large writes (`SmatchetChatPersistWorker` is the reference pattern).
- `p4 ...` invocations (any `system()`, `_popen`, child-process spawn) — must run on a `std::thread` worker (see the `AnnotateAnalysisUi.cpp` pattern).
- Synchronous file I/O (image decode + upload, font load, attachment download) on the UI thread — use `std::async(std::launch::async, …)` and poll per frame.
- `std::future::get()` without a prior `wait_for(0s)` ready-check — blocks the frame.
- **Single-slot async loads: get the latch ORDER right, or the pane wedges for the session.** Route them through `LaunchIntoSlot` / `TakeFromSlot` in [`AsyncLoadGatePure.h`](../../include/Ui/AsyncLoadGatePure.h) rather than hand-rolling. Publish the in-flight latch only AFTER the launcher returns a future (`std::async` throws `std::system_error` when threads are exhausted, leaving a latch over an invalid slot), and clear it BEFORE `get()` (a worker exception is rethrown by `get()`, so clearing after it never runs). Either mistake makes every later poll early-out forever on a load that can never land. This has bitten three times in this directory — the plan-doc viewer's read launch (#2056), its poll (#2057), and its index pair (#2108) — and `LuaConsolePlugin::KickScriptLoad`/`PollScriptLoad` documents the same hazard. It is worst where the retry button is itself gated on the latch, which removes the pane's only manual recovery.
- **Report a failed keyed read against the key it was FOR, not the live key.** `get()` destroys the payload on a throw, so the poll has no `result.key` to fall back on — record the key at launch. Reaching for the current selection instead names the wrong document, and parking the load gate on it convinces the gate that document is already loaded, so it never loads (#2108). Reuse `ResultIsCurrent` to decide whether the failure still describes what the user is looking at.
- `std::thread::join()` anywhere outside shutdown / destructor paths.
- `std::this_thread::sleep_for` on the UI thread — never legal.
- Long lambdas posted to `MainThreadDispatcher` — `Drain()` blocks the frame; chunk + repost instead.
- Holding a `std::mutex` across an HTTP / SQLite / p4 / file-I/O call from any thread (the UI thread waiting on that mutex = a spike).
- New owners of `std::thread` / `std::async` futures missing the join contract in their destructor — `~AppController` (with `BeginShutdown()` + join) is the reference pattern; a missing join → `std::terminate`.

## Steady-state perf (Pillar 1)

- Steady-state UI work ≤ 6.94 ms (144 Hz); p99 ≤ 10.0 ms. Profile with `SMATCHET_UI_PERF_SCOPE` markers, not by eye.

## Before you edit

- A `Draw*` / `Render*` function approaching 200 lines uses the section-helper pattern (`DrawCtx` + `DrawHeader`/`DrawBody`/`DrawFooter`/`DrawModals`/`HandleHotkeys`) per [`docs/guides/imgui-draw-pattern.md`](../../../../docs/guides/imgui-draw-pattern.md). Existing monoliths are ride-along only.
- Any new sync-stall path > 100 ms needs a visible cue (spinner/disabled state) and bucket-E coverage.
- Gate per-window mutations on live `IsWindowFocused`/this-frame focus, never a previous-frame flag — same-frame mouse-down focus races stale flags (PR #962 review: 3 of 4 HIGHs were this one pattern).
- Second-order of the rule above: when you OPEN a gate to this-frame focus, re-verify every context object the gated mutation writes through was resolved for THIS pane/window — a newly-admitted frame may still carry a fallback-resolved object (e.g. the other backend's active view), and the mutation lands in the wrong target (PR #962 delta-review HIGH).
- A per-frame override of a window's geometry / dock id / visibility IS that window's live state, and ImGui's settings writer snapshots `Pos`/`Size`/`DockId` **off the live window** (omitting the `DockId=` line while it is 0). The debounced auto-save and the unconditional save in `DestroyContext` therefore persist the override as if the user had put the window there. Say in the header what the app writes at exit, or make the override not be the live state — no test bucket can observe it today (`docs/self-improvement/categories/test/2026-08-05-no-bucket-e-shutdown-relaunch-primitive.md`).
- Any `ViewState` store mutation that resizes `store.Views` (`Create`, `DeleteActive`) goes through a top-of-`Draw` deferral latch (`applyPendingViewCreate`/`applyPendingViewDelete`) — NEVER mid-frame from a click handler: the reallocation/erase dangles every `ViewDefinition*` resolved earlier in the frame (PR #962 user-repro crash). In-place mutations (`UpdateActive`, `Activate`) are pointer-stable and exempt.
