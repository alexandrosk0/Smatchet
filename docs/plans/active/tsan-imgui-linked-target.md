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

## Slice 1 — keystone dispatcher (SHIPPED in this PR)

`tests/Core/MainThreadDispatcherConcurrent.test.cpp` + the `UiPerfMonitor.cpp` link line in `SmatchetTsanTests`. `MainThreadDispatcher` is header-only and ImGui-free, so the keystone of the *entire* marshalling discipline lands in the existing headless target with no ImGui dependency. The case hammers `PostToMainThread` from N producers + a concurrent `QueueLen()` reader against one `Drain()`-ing thread, using a **non-atomic canary** (a plain counter + `std::string::push_back`, mirroring `assistantStreamBuf.append`) that is data-race-free *only if* `Drain()` serialises every task onto the single draining thread — plus a `BeginShutdown()`-races-posters case for the shutdown re-check path. If any task ever executed off the drain thread, or two drains overlapped, TSan flags the non-atomic writes. This is the foundation slices 2–3 build on: if the dispatcher is race-free, every `g_ui` post that rides it is too.

## Approach (slices 2–3 — the ImGui-linked target)

The blocker is purely link-surface: reaching `g_ui` / `AiAssistantController` pulls ImGui, and on desktop `ImGuiLib` (`CMakeLists.txt:1048`) bundles the GLFW + OpenGL3 backends (`imgui_impl_glfw.cpp` / `imgui_impl_opengl3.cpp`, appended `CMakeLists.txt:1037`), so a naive link drags GLFW/GL onto a headless runner. Two viable shapes — **resolve which at execution time (slice 2's first task), it is the load-bearing decision**:

- **Option A — headless ImGui-core variant (preferred if the AppController cut is bounded).** Add an `ImGuiLibHeadless` STATIC lib = the core ImGui TUs only (no `imgui_impl_*` backend sources), create a context with `ImGui::CreateContext()` + a built font atlas and `io.DisplaySize` set, never calling a backend. Then link `AiAssistantController.cpp` + a **test-provided `g_ui`** + the dispatcher + a fake `IAiClient` that streams deltas from a real `std::thread`. Keeps the runner dependency-free (no GLFW/X11/GL/Mesa), consistent with the headless design intent of `ninja-tsan-linux`. **Risk/cost:** `AiAssistantController` takes `AppController& app_` and references `g_ui` directly; this option needs the controller exercised without linking the full `AppController` god-object — i.e. a narrow seam (a test double exposing only `mainThreadDispatcher` + the `g_ui` fields the delta path touches), or a small extraction. `UiDrawSession` itself hard-includes `imgui.h` + `AppController.h` (`SmatchetUiSession.h:3,29`), so a test `g_ui` still compiles ImGui but does not need a window.

- **Option B — Mesa-on-runner app link (pragmatic, reuses existing infra).** A new `ninja-tsan-app-linux` preset (`SMATCHET_BUILD_APP=ON` + `SMATCHET_SANITIZER=tsan`) links the real app closure + `ImGuiLib` + `glfw`, provisions Mesa software GL on the runner exactly as the bucket-E lanes do (`.github/actions/install-mesa-gl`), boots a headless context, and drives the AiAssistant worker + dispatcher drain + `g_ui` reads under TSan. **Cost:** whole-app link + Mesa + GLFW build time on the runner, and it departs from the headless-no-GL design. **Benefit:** no seam extraction; closest to production link.

**Recommendation:** attempt Option A; fall back to B only if the AppController seam proves larger than the target's value. Given slice 1 already certifies the dispatcher and the marshalling is verified-by-reading, the marginal value of the full app-linked target is real but bounded — keep the slice small and do not let it balloon into an AppController decomposition.

## Slices

- **Slice 2 — target scaffold + g_ui delta hand-off.** Resolve Option A/B. Stand up the new target (`SmatchetTsanAppTests` or an extension of the chosen preset) + a concurrent test: a fake `IAiClient` streams N deltas from a worker thread (→ `MakeOnDelta` → `PostToMainThread`), while the test thread spins `mainThreadDispatcher.Drain()` and reads `g_ui.assistantStreamBuf` — the exact worker→UI hand-off, now TSan-instrumented. Assert the streamed text assembles intact and TSan is clean.
- **Slice 3 — cancel/submit race.** Extend with `Submit()` / `Cancel()` racing the worker: per-turn `currentCancel_` shared_ptr swap (`AiAssistantController.cpp:208-222`), `state_` atomic transitions, and the `turnGen` staleness drop. Drives the playbook's "AI streaming cancel/submit race" candidate directly. Assert no race + Cancel deterministically halts a turn.
- **Slice 4 (optional, gated) — MCP-thread→g_ui.** Only if Option B (app link) lands: a fake MCP `tools/call` dispatch on the MCP `std::thread` writing `g_ui` via a builtin command, vs the UI render read. The playbook's #3. Defer unless the app-link path is chosen and cheap.

## Files to modify

- `tests/CMakeLists.txt` — slice 1: the new test + `UiPerfMonitor.cpp` link (done). Slices 2–3: the new ImGui-linked target block (guarded by the chosen preset var).
- `CMakePresets.json` — slices 2–3: `ninja-tsan-app-linux` (Option B) or the headless-core wiring (Option A).
- `CMakeLists.txt` — Option A only: `ImGuiLibHeadless` (core ImGui TUs, no backends).
- `.github/workflows/tsan-linux-nightly.yml` — add the new target to the nightly + the paths-scoped `pull_request` trigger (add `Source/Core/src/AiAssistantController.cpp`, `Source/Core/src/Ui/SmatchetUI.cpp`). Option B also wires `install-mesa-gl`.
- `tests/Core/AiAssistantStreamHandoff.test.cpp`, `tests/Core/AiAssistantCancelRace.test.cpp` (new) — slices 2–3.
- `tests/support/FakeAiClient.h` (new or extend existing AI test doubles) — a streaming `IAiClient` that emits deltas from a real thread.

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
- *(slice 1)* `tests/Core/MainThreadDispatcherConcurrent.test.cpp` + `UiPerfMonitor.cpp` linked into `SmatchetTsanTests`; syntax-checked clean against real headers (C++14, `-Wall -Wextra`). CI TSan lane is the compile/run authority.

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
