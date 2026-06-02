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
- `std::thread::join()` anywhere outside shutdown / destructor paths.
- `std::this_thread::sleep_for` on the UI thread — never legal.
- Long lambdas posted to `MainThreadDispatcher` — `Drain()` blocks the frame; chunk + repost instead.
- Holding a `std::mutex` across an HTTP / SQLite / p4 / file-I/O call from any thread (the UI thread waiting on that mutex = a spike).
- New owners of `std::thread` / `std::async` futures missing the join contract in their destructor — `~AppController` (with `BeginShutdown()` + join) is the reference pattern; a missing join → `std::terminate`.

## Steady-state perf (Pillar 1)

- Steady-state UI work ≤ 6.94 ms (144 Hz); p99 ≤ 16.67 ms. Profile with `SMATCHET_UI_PERF_SCOPE` markers, not by eye.

## Before you edit

- A `Draw*` / `Render*` function approaching 200 lines uses the section-helper pattern (`DrawCtx` + `DrawHeader`/`DrawBody`/`DrawFooter`/`DrawModals`/`HandleHotkeys`) per [`docs/guides/imgui-draw-pattern.md`](../../../../docs/guides/imgui-draw-pattern.md). Existing monoliths are ride-along only.
- Any new sync-stall path > 100 ms needs a visible cue (spinner/disabled state) and bucket-E coverage.
