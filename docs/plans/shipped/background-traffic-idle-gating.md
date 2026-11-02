<!-- index-summary: stop background tracker traffic when the app sits unfocused + idle — idle-gated connectivity probe, live-success probe push, outage backoff, Unreal focus feed, minimized frame throttle -->
# Plan — Background-traffic idle gating
<!-- plan-date: 2026-11-02 -->

> **Slug**: `background-traffic-idle-gating` (matches this file's basename without `.md`).
>
> **Status**: `shipped`

## Context

User question (2026-11-01): does Smatchet stop background updating once the app has had **no input and no focus for some time**? It does not. An audit of every periodic network source found:

| Source | Interval | Pauses unfocused? | Pauses idle? |
|---|---|---|---|
| Ticket-change monitor (`AppController::TickChangeMonitors`) | 120 s (30–3600) | Standalone only — `main.cpp` feeds `GLFW_FOCUSED` into `SetWindowFocused` | no |
| Same monitor, Unreal host | 120 s | **no** — nothing calls `SetWindowFocused`; `windowFocused_` defaults `true` | no |
| Connectivity probe (`ConnectivityMonitorService::TickTrackerConnectivityMonitor`) | 90 s healthy / **20 s** degraded, forever | **no** | no |
| Offline / pending-action replay | 30 s / 300 s, only while the queue is non-empty | n/a (intended) | n/a |
| Standalone frame loop when minimized | uncapped: `glfwPollEvents` + DX12 skips `Present` at 0×0 | no | no |

Android is already covered: `android_main.cpp` skips the whole frame (and therefore every tick) while unfocused or paused.

**Intended outcome**: after this lands, an app left unfocused with no input for 5 minutes sends **no** periodic tracker requests (the change monitor already stops on unfocus; the probe now stops on background-idle), the probe is skipped whenever real traffic already proved reachability, an outage backs the probe off instead of hammering at 20 s, the Unreal host stops change polls when the editor is not the foreground app, and a minimized standalone window stops spinning a core.

## Approach

A tiny GLFW-free **activity hub** (`smatchet::activity`, header-only, modelled on `VsyncControl.h`) records, per frame, whether the host window is focused and whether ImGui processed any input event this frame (`ImGuiContext::InputEventsTrail` — covers mouse, keys, wheel, chars, focus, and ImGui Test Engine input). *Background-idle* = a host has reported at least once **and** the window is unfocused **and** the last focused-or-input frame is older than 5 minutes. A host that never reports (Android, headless CLI) is never idle, so nothing regresses where no signal exists.

Command dispatch (`CommandRegistry::Dispatch` — palette, MCP, CLI, Lua) also stamps activity, so an agent driving an unfocused window keeps it awake.

`ConnectivityMonitorService` consumes it: no scheduled probe launches while background-idle (an in-flight probe is still collected); the elapsed `nextProbeAt_` makes the probe fire on the first active frame. An explicit request — `RequestProbeNow`, or a `SetNextProbeAt` deadline already due (TicketSyncService after a transport failure) — passes the gate once. Two schedule changes live in a pure header (`ConnectivityProbeSchedule.h`) so they are unit-testable: (a) a successful live backend request while already healthy pushes `nextProbeAt_` out to `now + 90 s` (the probe only runs when nothing else has talked to the tracker); a degraded prior state does not push, so the recovery latch keeps its current behaviour; (b) consecutive down probe results (TransportDown / ServiceUnavailable) back off 20 → 40 → 80 → 120 s (cap) instead of a flat 20 s, reset on the first healthy result or live success. The streak keys on the probe's own result, not the sync/catalog warning flags: those are cleared later in the frame by `ConsumeTrackerConnectivityRecovery`, so they would otherwise keep a recovered streak alive.

Hosts: the standalone loop reports `(GLFW_FOCUSED, trail non-empty)` and throttles a minimized window with `glfwWaitEventsTimeout` (≈30 Hz, so the dispatcher drain and streaming apply keep ticking). The Unreal host (`SmatchetImGuiHost::DrawUI`, Core-side, Windows-only TU) derives focus from "the foreground window belongs to this process" (Win32 `GetForegroundWindow` + `GetWindowThreadProcessId`, the same test as `FPlatformApplicationMisc::IsThisApplicationForeground`) — no plugin, C-ABI, or packaged-header change — and feeds both the hub and `AppController::SetWindowFocused`.

**Sequencing trade-off**: three related reductions are deliberately deferred because their files are owned by open PRs (#2375 owns `AppController.h` / `AppController_PaneContexts.cpp` / `GridLiveContext.h`; #2372 owns `SmatchetUI.cpp` / `ConfigManager*`). See § Out of scope.

## Files to modify

1. `Source/Core/include/AppActivity.h` *(new)* — header-only hub: `NoteFrame(focused, hadInput, now)`, `NoteActivity(now)`, `IsBackgroundIdle(now)`, pure `IsBackgroundIdleAt(...)`, `ResetForTest()`, `kBackgroundIdleAfter = 5 min`. GLFW/ImGui-free (compiles into `SmatchetCore_DX12` and the TSan subset).
1a. `Source/Core/src/Commands/CommandRegistry.cpp` — `Dispatch` stamps `NoteActivity` (added after code review).
2. `Source/Core/include/ConnectivityProbeSchedule.h` *(new)* — pure probe-interval helpers: the two existing interval constants move here, plus `DegradedProbeInterval(consecutive)` and `NextProbeAfterLiveSuccess(next, now, wasHealthy, probeInFlight)`.
3. `Source/Core/include/ConnectivityMonitorService.h` — `consecutiveDegradedProbes_`, `pausedForIdle_` members; `GetNextProbeAt()` test seam beside the existing `SetNextProbeAt`.
4. `Source/Core/src/ConnectivityMonitorService.cpp` — idle gate in `TickTrackerConnectivityMonitor`; backoff in `ApplyTrackerConnectivityProbeResult`; live-success push in `applyLiveTrackerReachabilityAfterSuccessfulBackendRequest_`; constants come from the new header.
5. `Source/Standalone/main.cpp` — report the frame to the hub; minimized throttle in `RenderOneFrame` (`glfwWaitEventsTimeout` instead of `glfwPollEvents` while `GLFW_ICONIFIED`).
6. `Source/Core/src/Ui/SmatchetImGuiHost.cpp` — Unreal host: foreground-process focus → `App.SetWindowFocused` + hub report in `DrawUI`.
7. `tests/Core/ConnectivityMonitorService.test.cpp` — backoff, live-success push, idle gate, and hub predicate cases (existing registered TU; no `tests/CMakeLists.txt` change, which #2375 also edits).

## Existing utilities reused

- `smatchet::vsync` hub pattern — `Source/Core/include/VsyncControl.h` (header-only atomics, GLFW-free, dual-target).
- `AppController::SetWindowFocused` — `Source/Core/src/AppController_PaneContexts.cpp:732` (called, not modified).
- `ConnectivityMonitorService::SetNextProbeAt` test seam + `FakeConnectivityDeps` — `tests/support/FakeConnectivityDeps.h`.
- ImGui's per-frame `InputEventsTrail` (imgui_internal) — the input queue is already scanned by `TouchInputRouter::ScanQueue`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no extraction; two interval constants move from an anonymous namespace into a new pure header.

## UX Pillar callouts

- **Pillar 1 (perf)**: per-frame cost is a few relaxed atomic stores/loads plus one `GetForegroundWindow` call on the Unreal host; the minimized throttle only runs while nothing is visible.
- **Pillar 2 (UI never blocks)**: `glfwWaitEventsTimeout` blocks ≤ 33 ms and only while minimized; no new sync I/O.
- **Pillar 3 (never crash)**: no new allocation or threading; the hub is lock-free atomics.
- **Pillar 4 (accessibility)**: no UI change.
- **Pillar 6 (offline-first)**: offline replay is not gated (queued writes still replay in the background); an idle-paused probe resumes on the first active frame, so connectivity truth is refreshed before the user interacts; outage backoff caps at 120 s and a live transport failure still forces `RequestProbeNow`.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires: `idle` scenario covers the steady-state frame path the hub report sits on.
2. **Pillar 2 static scanner** — N/A: no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new stall path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

**Pre-push local check**: the `idle` scenario per `docs/guides/perf-workflow.md` § Gate-check vs baseline when a local baseline exists; otherwise PR-fast CI is the gate.

## Risks / non-goals

- **Second-monitor watcher**: a user watching an unfocused Smatchet window without input stops getting probe-driven connectivity updates after 5 min → accepted; the change monitor already stopped at unfocus (existing behaviour), and the first input/focus probes immediately.
- **Outage-recovery latency** rises from ≤ 20 s to ≤ 120 s in a long outage → mitigated: live transport failures and focus return still probe immediately; cap kept low.
- **Unreal foreground test** treats any window of the editor process as focused (including non-Smatchet editor panels) → accepted; it matches the standalone "app focused" semantic.
- Non-goal: no Preferences/config key for the idle threshold in this PR (see § Out of scope).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `ConnectivityMonitorService.test.cpp` — backoff sequence + cap + reset; live-success push only when healthy and no probe in flight; no probe launch while background-idle and immediate launch after activity; hub predicate truth table (never-reported, focused, unfocused-recent, unfocused-stale).
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface; the activity report is exercised by every bucket-E frame (test-engine input lands in `InputEventsTrail`).
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no scenario surface.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target; `SmatchetImGuiHost.cpp` is DX12-only).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against `CONTEXT-MAP.md` terms before finalising; outcome recorded in § Verification (actual).
- **Manual residue**: the minimized-window CPU drop is observed by hand (Task Manager) — deferred automation: a `--spawn` CLI probe that iconifies the window and samples process CPU; backlog entry `docs/self-improvement/categories/tooling/2026-11-02-minimized-frame-throttle-cpu-unautomated.md`.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray references to anything deferred here before finalising.

- **Adaptive change-monitor interval** (double while polls return nothing; poll only the focused pane while idle) — needs `GridLiveContext.h` + `AppController_PaneContexts.cpp`; follow-up after #2375 merges.
- **Change-monitor success as a reachability signal** (lets the probe push apply to monitor polls too) — same files; after #2375.
- **Idle-threshold config key + recovery-refetch debounce** — `ConfigManager*` + `SmatchetUI.cpp`; after #2372 merges.
- **HTTP transport**: gzip (`CURL_ZLIB OFF` today, `cmake/SmatchetThirdParty.cmake:144`) and connection reuse (one-shot `cpr::Get`/`Post` per request in `TrackerHttpUtils.cpp`) — own plan with `build-doctor`; adds a zlib dependency to both targets.
- **Persisted update-check timestamp** (skip the startup check if < 24 h) — low value, no-action for now.

## Implementation log

- `cb8e0be8c` · wip(plan): this plan.
- `a40ace972` · feat(connectivity): activity hub, idle-gated probe with explicit-request bypass, live-success probe push, down-streak backoff, Unreal foreground focus feed, command-dispatch activity, minimized frame throttle, tests, backlog entry for the minimized-CPU automation.

## Deviations from plan

- **Added after code review**: `CommandRegistry::Dispatch` stamps activity (`NoteActivity`), so an agent driving an unfocused window over MCP/CLI/Lua keeps it awake; an explicit probe request (`RequestProbeNow`, or a `SetNextProbeAt` deadline already due) passes the idle gate once.
- **Changed after code review**: the backoff streak keys on the probe's own result (TransportDown / ServiceUnavailable), not `IsConnectivityDegradedForProbeInterval`, because the sync/catalog warning flags are cleared later in the frame and kept a recovered streak alive.
- **Deferred (Low)**: `SetNextProbeAt` doubles as the test seam and the TicketSyncService "probe now" signal. Splitting it (the adapter calling `RequestProbeNow`) touches `GridContextDepsAdapter.cpp`, owned by #2375 — follow-up after it merges. Today every non-test caller passes `now` from a transport-failure path.
- **Deferred (Low)**: a live success on a non-focused pane's backend pushes the focused backend's probe; inherited from the single global connectivity FSM. Follow-up with the per-pane connectivity work.
- `main.cpp` sits at 1,209 lines, over the advisory 1,200 `tu-line-ceiling` (was 1,191); not blocking.

## Verification (actual)

- **Bucket A**: `SmatchetTests` (ninja-test-msvc) full suite 4,015 / 4,015 passed, including 11 new cases (hub predicate + `NoteFrame`/`NoteActivity`, `DegradedProbeInterval`, `NextProbeAfterLiveSuccess`, backoff sequence + reset, streak reset after a real loss, live-success push healthy / not after outage, idle no-launch, explicit request through idle).
- **Build gate**: `SmatchetStandalone`, `SmatchetCore_DX12`, and `SmatchetImGuiHost_DX12` (the Unreal host TU) build clean with `/WX` on ninja-iter-msvc.
- **Lint**: `test-lint-rules.sh --diff origin/develop` — the one blocking hit (comment-noise false positive in `AppActivity.h`) fixed; advisory `tu-line-ceiling` WARN remains (see Deviations).
- **Code review**: `code-review` agent, two rounds — round 1: 0 code Critical/High, 1 process Critical (manual residue), 2 Medium; all fixed; round 2: 0 Critical/High/Medium.
- **Not run**: live 5-minute idle observation in the running app, the minimized-window CPU measurement (backlog entry), the Unreal editor itself (only the host library is compiled here), `grill-with-docs` (not run — no domain-term changes; recorded as not done).
