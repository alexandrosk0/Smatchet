# Plan — ImGui-linked TSan target (g_ui hand-off + AiAssistant cancel/state machine)

> **Slug**: `tsan-imgui-linked-target` (matches this file's basename without `.md`).
>
> **Status**: `active` — slice 1 (keystone dispatcher) shipped; the ImGui-linked target (slices 2–3) is designed-not-started.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.
>
> **Origin**: the "single largest dynamic-coverage gap" from `docs/security/DEEPER_AUDIT_PLAYBOOK.md` (Lane 1, target #3 — "MCP-thread vs UI-thread data race on `g_ui`", "AI streaming cancel/submit race") and the deferred TSan-expansion item in `docs/plans/active/build-quality-velocity-hardening.md` (§ Verification (actual) → #10).

## Context

The headless `SmatchetTsanTests` target (`tests/CMakeLists.txt`, preset `ninja-tsan-linux`) has grown well past its 2026-06-08 MVP: it now instruments `LocalCacheManager`, `GridLiveContext` + `TicketSyncService` (the ADR-0012 atomic backend-swap race), `EditMetaCacheService`, `ConnectivityMonitorService`, `OfflineQueueService` (replay / backend-swap / config-save), and `ConfigSaveWorker` — every ImGui-free cross-thread service in `Source/Core`. That subset is closed.

What it structurally **cannot** reach is the one surface the deeper-audit playbook flagged as the single largest gap: the cross-thread hand-off into the UI-owned `g_ui` (`UiDrawSession`, defined `Source/Core/src/Ui/SmatchetUI.cpp:66`). `SMATCHET_BUILD_APP=OFF` (the preset's whole point — no GLFW/X11/GL on the runner) excludes `SmatchetUI.cpp`, `AiAssistantController.cpp`, the MCP plugin, and the Lua automation worker. So the playbook's two headline candidates — the MCP-thread→`g_ui` write race and the AiAssistant streaming cancel/submit race — were never *observed*, only reasoned about.

**A read-level finding from authoring this plan, which reshapes its goal:** those `g_ui` writes are **not raw cross-thread writes**. `AiAssistantController::MakeOnDelta` wraps every `g_ui.assistantStreamBuf.append(...)` in `app->mainThreadDispatcher.PostToMainThread(...)` with a `turnGen` staleness guard (`AiAssistantController.cpp:442-444`); the merge-watch notify HTTP thread posts its toast identically (`SmatchetMergeWatchNotifyServer.cpp:109-112`); sync-status toasts run on the UI thread by contract (`GridContextDepsAdapter.cpp` + `ITicketSyncDeps` `SyncNotifyLevel`). So the surface is governed by a **consistently-applied marshalling discipline**, and the dominant risk is *regression of that discipline*, not a currently-live race. This target therefore exists to (a) make TSan **prove** the discipline holds end-to-end and (b) lock it against future regressions — not to chase a known bug.

**Intended outcome (one sentence):** TSan instruments the real `MainThreadDispatcher` → `g_ui` hand-off and the `AiAssistantController` cancel/state machine on a runnable target, converting the playbook's two reasoned-only `g_ui` candidates into observed-race-free (or, if the discipline is broken anywhere, observed-and-fixed).

## Slice 1 — keystone dispatcher (SHIPPED — PR #1567, merged)

`tests/Core/MainThreadDispatcherConcurrent.test.cpp` + the `UiPerfMonitor.cpp` link line in `SmatchetTsanTests`. `MainThreadDispatcher` is header-only and ImGui-free, so the keystone of the *entire* marshalling discipline lands in the existing headless target with no ImGui dependency. The case hammers `PostToMainThread` from N producers + a concurrent `QueueLen()` reader against one `Drain()`-ing thread, using a **non-atomic canary** (a plain counter + `std::string::push_back`, mirroring `assistantStreamBuf.append`) that is data-race-free *only if* `Drain()` serialises every task onto the single draining thread — plus a `BeginShutdown()`-races-posters case for the shutdown re-check path. If any task ever executed off the drain thread, or two drains overlapped, TSan flags the non-atomic writes. This is the foundation slices 2–3 build on: if the dispatcher is race-free, every `g_ui` post that rides it is too.

## Approach (slices 2–3 — the ImGui-linked target)

The blocker is purely link-surface: reaching `g_ui` / `AiAssistantController` pulls ImGui, and on desktop `ImGuiLib` (`CMakeLists.txt:1048`) bundles the GLFW + OpenGL3 backends (`imgui_impl_glfw.cpp` / `imgui_impl_opengl3.cpp`, appended `CMakeLists.txt:1037`), so a naive link drags GLFW/GL onto a headless runner. Two viable shapes — **resolve which at execution time (slice 2's first task), it is the load-bearing decision**:

**User picked Option A (2026-06-27).** A coupling probe done at execution time made A *cheaper than the plan first assumed* and removed the need for `ImGuiLibHeadless` entirely:

- `AiAssistantController`'s only use of `AppController` is `app_.mainThreadDispatcher` (4 sites: ctor + `MakeOnDelta` + `MakeOnError` + the model-change clear). The dispatcher seam is therefore trivial (slice 2a, done).
- Its only other UI coupling is the global `g_ui`: ~27 sites touching **8 fields** (`assistantStreamBuf`, `assistantHistory`, `assistantHistoryRowIds`, `assistantHistoryHydrated`, `assistantTurnGen`, `assistantInFlight`, `assistantLastError`, `cfg.AssistantHistoryMaxRows`), **all inside `PostToMainThread` lambdas** (already marshalled). Extracting those 8 fields behind a small `IAiAssistantUiState` seam — mirroring the existing `ITicketSyncDeps` / `IEditMetaDeps` pattern — makes the controller **both `g_ui`-free and ImGui-free**.

**Consequence:** with the controller depending only on `MainThreadDispatcher&` + `IAiAssistantUiState&` (both ImGui-free), the TSan test needs **neither a test-provided `g_ui` nor `ImGuiLibHeadless` nor a headless ImGui context** — it links straight into the existing headless `SmatchetTsanTests`. The original "Option A — headless ImGui-core variant" and "Option B — Mesa-on-runner app link" are both **obsoleted** by the seam; no new preset, no new CMake target, no Mesa. (Kept here for provenance: B would have linked the whole app under Mesa; A would have built `ImGuiLibHeadless` + a test `g_ui`. The seam is strictly cheaper and cleaner than either.)

**Recommendation:** the UI-state seam (slice 2b) is the load-bearing, perf-sensitive, architecturally-significant step (it changes how the controller writes results to the UI) — design-review the interface before rewiring the streaming hot path. It is a focused decoupling, NOT an `AppController` decomposition; keep it to the 8 fields + 2 behaviours (history-append-with-rowid, bounded-trim).

## Slices

- **Slice 2a — dispatcher seam (DONE, this PR).** `AiAssistantController` now takes `MainThreadDispatcher&` instead of `AppController& app_`; the 4 `app_.mainThreadDispatcher` sites → `dispatcher_`, construction at `AppController.cpp:2313` passes `mainThreadDispatcher`. Pure decoupling, no behaviour change — removes the AppController god-object dependency and lets the controller be constructed in a test with just a dispatcher. (Still links `g_ui` until 2b.)
- **Slice 2b — `IAiAssistantUiState` seam (architecturally significant — design-review first).** Extract the 8 `g_ui` chat fields behind a narrow interface; production wires it to `g_ui`, the test uses a fake. Makes the controller `g_ui`-free and ImGui-free. Touches the streaming hot path → carries the Perf-gate section. Mirror `GridContextDepsAdapter`/`ITicketSyncDeps`.
- **Slice 2c — the TSan test.** In the existing `SmatchetTsanTests` (no new target): a fake `IAiClient` streams N deltas from a worker thread (→ `MakeOnDelta` → `PostToMainThread`) while the test thread spins `Drain()` and reads the fake UI-state — the worker→UI hand-off, TSan-instrumented. Assert the streamed text assembles intact and TSan is clean.
- **Slice 3 — cancel/submit race.** Extend 2c with `Submit()` / `Cancel()` racing the worker: per-turn `currentCancel_` shared_ptr swap (`AiAssistantController.cpp:208-222`), `state_` atomic transitions, the `turnGen` staleness drop. Drives the playbook's "AI streaming cancel/submit race" candidate. Assert no race + Cancel deterministically halts a turn.
- **Slice 4 (optional, gated) — MCP-thread→g_ui.** The playbook's #3 (MCP dispatch thread writing `g_ui` via a builtin command vs the UI read). Needs its own seam (the builtin-command → g_ui path); defer unless it proves cheap after 2b/2c.

## Files to modify

The seam approach needs **no** new preset, CMake target, or CI-Mesa wiring (that was Options A/B, now obsoleted):

- `Source/Core/include/AiAssistantController.h` + `…/src/AiAssistantController.cpp` — 2a: dispatcher seam (done). 2b: depend on `IAiAssistantUiState&`; replace the 27 `g_ui.*` sites with seam calls.
- `Source/Core/include/IAiAssistantUiState.h` (new) — 2b: the 8-field + 2-behaviour interface.
- `Source/Core/src/AppController.cpp` — 2a: construction passes `mainThreadDispatcher` (done). 2b: provide the production adapter binding the interface to `g_ui` (akin to `GridContextDepsAdapter`).
- `tests/CMakeLists.txt` — 2c: add the new test (+ link `AiAssistantController.cpp` once it is `g_ui`/ImGui-free) into the existing `SmatchetTsanTests` — no new target.
- `tests/support/FakeAiAssistantUiState.h`, `tests/support/FakeAiClient.h` (new) — 2c: a streaming `IAiClient` emitting deltas from a real thread + the fake UI-state.
- `tests/Core/AiAssistantStreamHandoff.test.cpp`, `tests/Core/AiAssistantCancelRace.test.cpp` (new) — 2c / slice 3.
- `.github/workflows/tsan-linux-nightly.yml` — add `Source/Core/src/AiAssistantController.cpp` to the paths-scoped `pull_request` trigger.

## Existing utilities reused

- `tests/Core/MainThreadDispatcherConcurrent.test.cpp` (slice 1) — the dispatcher race-canary pattern slices 2–3 extend with real `g_ui` state.
- `tests/Core/EditMetaCacheConcurrent.test.cpp` / `LocalCacheConcurrentSeed.test.cpp` — the real-`std::thread` worker-vs-reader idiom + `JoinLaunchedThreads` discipline.
- `.github/actions/install-mesa-gl/action.yml` — the bucket-E Mesa software-GL provisioning Option B reuses.
- The existing AI test doubles under `tests/support/` + `IAiClient.h` — the streaming-fake base.

## UX Pillar callouts

- **Pillar 1/4**: N/A — test-only infra, no product runtime change (Option A adds no product code; the AppController seam, if any, is compile-time only).
- **Pillar 2 (UI never blocks)**: this target *protects* Pillar 2 — it certifies the off-UI-thread→UI marshalling that keeps blocking work off the render thread.
- **Pillar 3 (never crash)**: a TSan-caught race here would be a latent crash/corruption class; the target converts reasoning into proof.

## Perf-review-system gates

`N/A` for slice 1 (test-only, no `Source/Core/` product TU changed — only a `.cpp` added to a test link list + a new `tests/` file). Slices 2–3: `N/A` unless Option A requires an `AppController` seam extraction that touches a `Source/Core/` TU — in which case the touched-file perf scenario applies and the slice carries the mandatory Perf-gate section.

## Risks / non-goals

- **Risk — the target balloons into AppController decomposition.** *Mitigation:* prefer Option A's narrow test-double seam; if the cut exceeds ~a day, take Option B (app link under Mesa) instead of refactoring the god-object.
- **Risk — authored blind (no local build in the dev session — egress blocks FetchContent).** *Mitigation:* slice 1 was syntax-checked against the real headers under C++14 clang `-Wall -Wextra`; every slice's real verification is the paths-scoped `tsan-linux-nightly.yml` `pull_request` lane, which compiles + runs it on CI.
- **Non-goal — making the lane required.** It stays advisory (not in `project.config.json` required_contexts), like the existing TSan + fuzz lanes.
- **Non-goal — re-verifying the sync layer.** Already covered; this target is strictly the `g_ui`/AiAssistant surface.

## Verification

- **Slice 1**: the `pull_request` TSan lane builds `SmatchetTsanTests` (the test touches `tests/CMakeLists.txt` → triggers the path-scoped lane) and runs `ctest` under `TSAN_OPTIONS=halt_on_error=1`; the two new cases pass race-clean. Local syntax-check: `clang++ -std=c++14 -fsyntax-only -Wall -Wextra` against the real headers (done, clean).
- **Slices 2–3**: the new cases run race-clean on the lane; a deliberately-introduced un-posted `g_ui` write (temporary) must make TSan red (negative control), proving the instrumentation reaches the surface.
- **Doc validation**: `scripts/dev/test-docs.sh` green for this plan doc.
- **Plan stress-test (`grill-with-docs`)**: run before slice 2 to lock the Option A/B decision against the domain model.

## Out of scope (flagged, not designed)

- Lua automation-worker shutdown (the playbook's #4 / security.md `#13`) — already part-shipped (#1271 cooperative cancel + bounded join); its residual UI-thread-sync tracker call is a tracker-backend follow-up, not this target.
- Promoting any TSan lane to a required merge gate — separate branch-protection decision.

## Implementation log
- *(slice 1)* PR #1567 (merged, squash `aeb3bd5`) — `tests/Core/MainThreadDispatcherConcurrent.test.cpp` + `UiPerfMonitor.cpp` linked into `SmatchetTsanTests`; the `TSan Linux subset` CI lane went green (compiled + ran race-clean).
- *(slice 2a)* dispatcher seam — `AiAssistantController(MainThreadDispatcher&)` replaces `AppController& app_` (4 sites + ctor + member + fwd-decl; construction at `AppController.cpp:2313`). Pure decoupling, perf-neutral, no behaviour change. Probe finding: the controller's *only* AppController coupling was `mainThreadDispatcher`, and its only other UI coupling is 8 `g_ui` fields — which is why the 2b seam obsoletes the `ImGuiLibHeadless`/Mesa options.

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
