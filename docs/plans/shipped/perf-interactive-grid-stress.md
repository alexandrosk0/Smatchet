# Plan — interactive-grid-stress (active 8-pane mixed-backend perf probe)

> **Slug**: `perf-interactive-grid-stress` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

`side-by-side-grids` (`SideBySideNGridScenario`) is a **passive** N-pane probe: it clones one offline backend and draws, answering "how many grids before the *render* budget blows." It cannot answer the operator question "does 8 panes hold the 6.94 ms / 144 Hz budget while a user actually *uses* it" — there is no scroll, no focus churn, no view switching, and no real per-backend sync contention.

This plan adds `interactive-grid-stress`: an **active-load** sibling that drives a worst-case interactive load (per-frame vertical + horizontal scroll, periodic cross-backend pane-focus switching, a few cross-backend view switches) across a **mixed 8-pane set (50 % Jira — 4 Jira / 2 GitHub / 2 Plane)**, each pane on its own live `GridLiveContext` so the 4 Jira panes force real network sync. It is a pure input injector measured by the existing real-GL `FpsMeasure` path. After this lands, an operator can reproduce the active-load measurement with one env-gated launch and quantify the interaction tail (p99 / worst-frame) separately from steady-state draw cost.

Prompted by a user perf request ("make the test more active … scroll vertically and horizontally … switch between the panes … switch views a couple of times in multiple backends"). First measurement run (uncommitted) showed avg 3.0–3.5 ms (within budget) but a reproducible p99 of 10.8–13.2 ms breaching the 10.0 ms / 100 Hz floor, driven by the synchronous `ConfigManager::Save` + `ViewState` reload on each cross-backend focus switch — captured here as a reusable probe.

## Approach

Mirror the `SideBySideNGridScenario` structure (synthetic-pane prefix, `PurgeSyntheticPanes` reverse-erase, `EnsurePaneContextLive` per pane, global-scope `MakeXScenario()` factory + registry wiring) but diverge on three axes: (1) **mixed backends with own live contexts** instead of one cloned offline backend; (2) **per-frame interaction injection** in `OnFrame` — `Triangle()` ramps drive `CurrentScrollY()`/`g_ui.scenarioScrollTargetX`, and `g_ui.focusedPaneId` + `g_ui.gridPaneFocusReassigned` drive focus + view switches through the host's existing reassign path; (3) **effectively perpetual** (`frames_` defaults huge) so it injects for the whole `FpsMeasure` window, which closes the window.

Horizontal scroll had no host hook (only `SetScrollY` existed), so add a sibling `g_ui.scenarioScrollTargetX` session field + a mirrored `ImGui::SetScrollX` in the grid's table-inner window, gated on the focused pane — the minimal faithful path vs fragile IO injection. A small env-gated autostart hook in `main.cpp` (`SMATCHET_AUTORUN_SCENARIO`) starts any named scenario once at first frame, mirroring the existing `SMATCHET_FPS_MEASURE_SECONDS` tooling so the probe runs head-lessly under real GL.

Trade-off named: the scenario is **not** added to the PR-fast perf gate — like `side-by-side-grids` it is a variable-N operator probe with no fixed baseline, so it would only add CI noise, not regression signal.

## Files to modify

1. `Source/Core/src/Commands/Scenarios/InteractiveGridStressScenario.cpp` (new) — the scenario: mixed 8-pane build, per-frame scroll/focus/view injection, cleanup-on-finish/cancel.
2. `Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp` — extern decl + `RegisterFactory("interactive-grid-stress", …)`, anchored after `side-by-side-grids`.
3. `Source/Core/include/Ui/SmatchetUiSession.h` — new `int scenarioScrollTargetX = -1;` session field (horizontal-scroll target; no `ScenarioRunner` out-param — written directly by injection scenarios).
4. `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp` — mirrored `ImGui::SetScrollX` hook beside the existing `SetScrollY`, gated on `ctx.pane.focused`.
5. `Source/Standalone/main.cpp` — `SMATCHET_AUTORUN_SCENARIO` env-gated one-shot scenario start inside the render loop (no-op when unset); `#include "Commands/Command.h"`.
6. `tests/Core/SmatchetScenarioRegistry.stubs.cpp` — linker stub for `MakeInteractiveGridStressScenario` (lock-step with the registry externs).
7. `tests/Core/SmatchetScenarioRegistry.test.cpp` — `expected.insert("interactive-grid-stress")` so the exact-set snapshot test stays green.

## Existing utilities reused

- `AppController::EnsurePaneContextLive` (`Source/Core/include/AppController.h`) — spins a live `GridLiveContext` per pane so each backend's sync runs; same call `side-by-side-grids` / `concurrent-sync` use.
- `FindGridPaneById` (`Source/Core/include/Ui/GridPane.h`) — defensive pane-id lookup for switch targets + collision guard.
- `UiPerfMonitor::Instance()` (`Source/Core/include/Ui/UiPerfMonitor.h`) — `Reset()` at start, `GetLastFrameRows(includeP99=true)` for the `OnFinish` JSON payload.
- `FpsMeasure` (`Source/Standalone/main.cpp`) — real-GL per-frame wall-clock measurement + auto window-close; the scenario is a pure injector on top of it.
- `g_ui.scenarioScrollActive` / `scenarioScrollTarget` + `gridPaneFocusReassigned` reassign path — existing host plumbing for scenario-driven scroll/focus.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: this *measures* Pillar 1 under active load; it introduces no steady-state cost to ship builds (scenario TU only runs when explicitly started; the grid `SetScrollX` hook is a single gated call on the focused pane, identical cost shape to the existing `SetScrollY`).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new blocking path — the scenario drives the host's *existing* focus-reassign + sync code; the synchronous `ConfigManager::Save` on cross-backend focus switch it exercises is pre-existing host behaviour (`SmatchetGridPaneWindows.cpp:165`), surfaced for measurement, not introduced here. (Follow-up to debounce/async that Save is out of scope — see § Out of scope.)
- **Pillar 3 (never crash)**: synthetic panes are purged on start (crash-recovery) + finish/cancel; switch targets are bounds-checked (`paneIndex < paneIds_.size()`, `FindGridPaneById` null-guard); RAII throughout (`make_unique` factory, no raw `new`).
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — headless perf tooling, no user-facing UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`. For each gate, declare: **fires** (one-line how) or **N/A** (one-line reason). No restating mechanics — link out for the canonical text.

1. **PR-fast CI** — **N/A (intentional)**: `interactive-grid-stress` is a variable-N operator probe with no fixed baseline, deliberately **not** added to `scripts/dev/perf-pr-fast-set.json` (same rationale as `side-by-side-grids`). The fast-set scenario most directly covering the changed grid render path remains `side-by-side-2-grid`, which this diff does not alter.
2. **Pillar 2 static scanner** — **fires clean**: no new sync-I/O reachable from `ImGui::*`. The `SetScrollX` hook is a pure ImGui call; `EnsurePaneContextLive` + the autostart `Scenarios().Start` run on existing worker-dispatched sync, not inline in a render frame.
3. **Dispatcher drain** — **N/A**: does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — **N/A**: adds no new sync-stall code path > 100 ms; the measured switch tail is existing host behaviour.
5. **Marker inventory** — **N/A**: adds no `SMATCHET_UI_PERF_SCOPE` markers (reuses the host's existing `pane.render` / `drawActiveProjectWindow` scopes; `OnFinish` only reads existing rows).

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the named scenario(s) before opening the PR — N/A here (no baseline-gated scenario added; the probe is operator-run).

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — intentional regression + baseline-bump PR queued only. Not used (no baseline change).

## Risks / non-goals

- **Risk — synthetic panes leak on hard crash.** Mitigation: `OnStart` purges by `perf-interactive-grid-` prefix before building; the launch harness also backs up + restores the user's persisted pane set.
- **Risk — view-switch reverts mid-frame.** A focused pane's `viewId` change is reverted by the steady-state branch unless `gridPaneFocusReassigned` is also set; the scenario sets it on every scheduled switch. Accepted (matches host contract).
- **Risk — autostart hook fires in a normal session.** Mitigation: gated entirely on a non-empty `SMATCHET_AUTORUN_SCENARIO` env var; absent → zero behaviour change.
- **Non-goal — fixing the p99 breach.** This plan only *measures* the interaction tail; debouncing/async-ing the cross-backend `ConfigManager::Save` is a separate `spike-hunter` follow-up.
- **Non-goal — a regression baseline.** Variable-N probe; intentionally excluded from the PR-fast gate.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: `tests/Core/SmatchetScenarioRegistry.test.cpp` exact-set snapshot now includes `interactive-grid-stress`; the stub keeps the test target linking. Run via `ninja-test-msvc` + ctest.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A — no new interactive UI surface; the scenario *is* the headless driver.
- **Bash-driver scenario / screenshot / sanitizer**: manual operator run — `SMATCHET_AUTORUN_SCENARIO=interactive-grid-stress SMATCHET_FPS_MEASURE_SECONDS=12 Smatchet.exe`, read the `[fps-measure]` stderr line. (No golden artefact: variable wall-clock by design.)
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target). The scenario TU is Linear-agnostic and auto-globbed into `SmatchetCore`.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: lightweight — this is measurement tooling with no new domain-model concept or terminology to sharpen; reviewed against the `side-by-side-grids` precedent it mirrors. Record any divergence here.
- **Manual residue**: the operator FPS run above is inherently manual (variable wall-clock, no fixed baseline); deferred-automation action plan = none required (it is an on-demand probe, not a gate). No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them. (Nothing deferred from a prior plan; this is net-new tooling.)

- **Debounce/async the cross-backend `ConfigManager::Save`** — the dominant p99 contributor this probe surfaces. Follow-up: `spike-hunter` job, separate PR. No-action here.
- **Adding `interactive-grid-stress` to a CI baseline** — intentionally excluded (variable-N). No follow-up planned.

## Implementation log
- `6a0d3d4b` · wip(plan): committed this plan doc before coding (template-conformant).
- `be16611c` · scenario + 4 product edits + 2 test edits — new `InteractiveGridStressScenario.cpp`; `scenarioScrollTargetX` session field + mirrored `ImGui::SetScrollX` grid hook; `SMATCHET_AUTORUN_SCENARIO` one-shot in `main.cpp` (extracted `MaybeStartAutorunScenario` helper); registry extern + `RegisterFactory`; lock-step stub + exact-set snapshot entry.
- `5f0db76b` · squash-merged to develop as PR #1492 (8 files, +416/-2).

## Deviations from plan
- **`RunFrameLoop` over the 120-line cap.** Adding the autorun block inline pushed `main.cpp`'s `RunFrameLoop` to 136 lines (`function-too-long`). Extracted the autorun logic into a free `MaybeStartAutorunScenario(app, started)` helper above the loop (no behaviour change) → in-loop footprint = one call, back under cap. Same file as planned, finer shape.
- **Deviation comments needed single-line + `clang-format off` wrap.** clang-format wraps long `//` lines, which breaks the `SMATCHET_DEVIATION` escape parser. Two `duplication` WARNs (deliberate ACTIVE-load sibling of the side-by-side-grids perf family, kept byte-identical per ADR-0015 calibration) and one `app-controller-fan-in` escape were each written as one physical line fenced in `// clang-format off/on`.
- **PR opened without `## Intent`.** The `Intent section` doc-gate went red on `opened`; self-healed by adding the section (the `edited` re-run trigger shipped in #1483). No code impact.

## Verification (actual)
- **Bucket A (ctest snapshot)**: registry↔stub↔test↔factory lock-step verified; exact-set snapshot includes `interactive-grid-stress`. Authoritative compile = CI `Windows + MSVC` + Coverage lanes.
- **Lint gate** (`test-lint-rules.sh --diff origin/develop`): PASS after the three fixes above (function-length extraction, comment-noise reword, single-line deviation wrap).
- **CI on #1492**: 36 pass / 3 skip / **0 fail** (Windows MSVC dual-target, Coverage, Perf PR-fast, CodeQL, Android, doc-validation). Merged 2026-06-20, squash `5f0db76b`.
- **Operator FPS run** (manual by design — variable wall-clock, no golden): `SMATCHET_AUTORUN_SCENARIO=interactive-grid-stress SMATCHET_FPS_MEASURE_SECONDS=12 Smatchet.exe`. First run: avg ~3.0–3.5 ms (within the 6.94 ms steady budget) but reproducible **p99 10.8–13.2 ms breaching the 10.0 ms / 100 Hz floor**, driven by the synchronous cross-backend `ConfigManager::Save`. Tail-fix deferred to a `spike-hunter` follow-up (§ Out of scope).
