# Plan — bucket-C screenshot-diff determinism (user-info-* flake)

> **Slug**: `bucket-c-screenshot-determinism`
>
> **Status**: `shipped`

## Context

The four isolated `user-info-*` bucket-C screenshot scenarios
(`Source/Core/src/Commands/Scenarios/UserInfoScreenshotScenarios.cpp`) flaked against
their goldens on `develop`. The reported symptom was the Whisper "Enable voice
dictation?" first-run banner appearing in ~25 % of ephemeral spawns even though
`OnStart` sets `g_ui.cfg.WhisperSetupCompleted = true`.

Reproduction (`scripts/dev/test-screenshot-diff.sh`, empty `SMATCHET_USER_DATA` tmp
profile, `--frames=20 --warmupFrames=16`, `--spawn`) turned up **three** independent
non-determinism sources behind that one failing predicate, not one:

1. **Config clobber** — the pre-`Draw` `mainThreadDispatcher.Drain()` in the ephemeral
   render loop ran before `SmatchetUI::drawInitConfigOnce`'s wholesale
   `g_ui.cfg = ConfigManager::Load()`, so a scenario's `WhisperSetupCompleted` write
   landed in a `g_ui.cfg` that the load then overwrote from disk. This is the reported
   banner flake (~8 % measured at baseline, 100 % if the drain is moved earlier).
2. **Wrong docked tab** — `User Info` shares a dock node with `Preferences`, and
   `ImGui::SetNextWindowFocus()` does not select a docked tab: `FocusWindow()`'s
   "select in dock node" lines are commented out upstream (imgui.cpp, issue #2304),
   and `DockNodeUpdateTabBar` only mirrors nav focus when `g.NavWindow->RootWindow`
   *is* the node's own window — never true for a docked child. The node kept whatever
   tab it picked on the frame it was **created**, so `User Info` could come up behind
   `Preferences` and stay there for the session.
3. **Update-modal nav-focus theft** — the startup app-update check is an async GitHub
   round-trip. `DrainAppUpdateCheck` opens the `Update Available` modal on whichever
   frame the response lands, the modal takes ImGui nav focus, and it **keeps** that
   focus after the popup stops being submitted. The capture then renders every dock
   node in `ImGuiCol_TitleBg` instead of the golden's `TitleBgActive`, with no modal
   visible in the frame to explain the diff (`L_inf = 190`).

After this lands, the four `user-info-*` scenarios render byte-identically across
repeated ephemeral spawns (measured 0 deviations / 20 runs, twice).

## Approach

Three targeted fixes, one per cause.

**(1)** Gate the pre-`Draw` drain on `g_ui.cfgInitialized`. Until the first `Draw` has
latched config, a dispatched command's `g_ui.cfg` write is doomed; skipping the drain
for those few frames only defers the command by one frame, since the in-`Draw` drain
still runs every frame.

**(2)** New `SmatchetUI::selectDockedTab(const char*)` writes
`window->DockNode->TabBar->NextSelectedTabId` directly and then calls
`ImGui::FocusWindow(window)`. Both halves are needed: selecting the tab alone leaves
the node's chrome in the unfocused `ImGuiCol_TabDimmed*` palette, and
`SetNextWindowFocus()` cannot supply the focus because it is consumed by the docked
child's `Begin()`, which runs *after* `DockNodeUpdateTabBar` has already picked the
frame's colours. The lookup goes through `SmatchetLocalization::WindowTitleFromSource`
so it resolves in non-English locales.

**(3)** New `UiDrawSession::ephemeralSession`, latched at bootstrap from the
`--ephemeral`-derived `forceMcp` flag, suppresses **both** the startup update check and
the modal-open inside `DrainAppUpdateCheck` (the future is still drained; only the UI
surfacing is skipped). The signal has to be **process identity, not scenario state**:
`--spawn` launches the child (`CliSpawn.cpp:236` / `:325`, `"<exe>" --ephemeral
--mcp-port <port>`), waits for MCP readiness, and only *then* sends `scenario.run`, so
no scenario exists in the child while the startup branch first runs. Four successive
scenario-state gates were built and measured before this was understood — see the
ledger below; all four suppressed nothing.

### Measurement ledger

`repro-log.sh N` loops `user-info-desktop-unified` via `--spawn` and counts captures
deviating from the last-known-good pixels.

| Build | Deviations |
|---|---|
| Baseline `develop` | 2 / 24 banner (~8 %) |
| Pre-`Draw` drain deleted outright | 24 / 24 banner (reverted) |
| Drain gated on `cfgInitialized` | banner gone |
| + `selectDockedTab` (tab id only) | 27 / 30, then 11 / 12 (all `linf=190`) |
| + `ImGui::FocusWindow` | ~87 % (n=30) |
| + startup check gated on `!Scenarios().Active()` | 26 / 30 |
| + latch-consume on `Active()` | 13 / 20 |
| + sticky `EverStarted()` at start site | 10 / 20 |
| + drain-time `suppressModal` (`EverStarted`) | 11 / 20 |
| **+ `ephemeralSession` gate (start + drain)** | **0 / 20** |
| **+ debug probe removed, rebuilt** | **0 / 20** |

## Files to modify

1. [`Source/Standalone/StandaloneAppBootstrap.cpp:653`](../../../Source/Standalone/StandaloneAppBootstrap.cpp) — gate the pre-`Draw` drain on `cfgInitialized`; latch `g_ui.ephemeralSession = forceMcp` in `Initialize`.
2. [`Source/Core/include/Ui/SmatchetUiSession.h:835`](../../../Source/Core/include/Ui/SmatchetUiSession.h) — new `bool ephemeralSession`.
3. [`Source/Core/src/Ui/SmatchetUI.cpp:119`](../../../Source/Core/src/Ui/SmatchetUI.cpp) — `DrainAppUpdateCheck` early-return before `OpenPopup`; `!d.ephemeralSession` on the startup-check branch; `selectDockedTab("User Info")` at the User Info focus site.
4. [`Source/Core/src/Ui/SmatchetUI_Layout.cpp`](../../../Source/Core/src/Ui/SmatchetUI_Layout.cpp) — new `SmatchetUI::selectDockedTab`.
5. [`Source/Core/include/Ui/SmatchetUI.h`](../../../Source/Core/include/Ui/SmatchetUI.h) — declare `selectDockedTab`.
6. [`Source/Core/src/Ui/SmatchetUI_Internal.h`](../../../Source/Core/src/Ui/SmatchetUI_Internal.h) — `imgui_internal.h` for `ImGuiWindow` / `ImGuiDockNode` / `ImGuiTabBar`.
7. [`Source/Core/src/Ui/SmatchetPreferencesUi.cpp`](../../../Source/Core/src/Ui/SmatchetPreferencesUi.cpp) — same `selectDockedTab` call for the Preferences tab.

## Existing utilities reused

- `SmatchetLocalization::WindowTitleFromSource` — maps the English `Begin()` source
  title to the real localized ImGui window name, so `FindWindowByName` resolves in
  every locale.
- `SmatchetUI::prepareTopLevelWindow` (`SmatchetUI_Layout.cpp`) — existing
  size/pos/focus preamble; `selectDockedTab` is called right after it, not instead.
- `UiDrawSession::cfgInitialized` — already the latch
  `SmatchetUI::drawInitConfigOnce` sets; reused as the drain gate.
- `IsEphemeralMode` / `forceMcp` (`CliCommandRunner.cpp:346`) — existing
  `--ephemeral` argv parse, already threaded into `Initialize`.

## Extraction sizing

N/A — this plan extracts nothing; it adds one helper and three gates.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact. `selectDockedTab` runs only on the frames a focus
  request is armed and does two pointer hops plus one `FindWindowByName`; the two new
  gates are `bool` tests on an already-hot branch.
- **Pillar 2 (UI never freezes)**: no impact — no new I/O, no new blocking call. The
  ephemeral gate strictly *removes* a network round-trip from spawned sessions.
- **Pillar 3 (never crash)**: `selectDockedTab` null-checks `window`, `DockNode`, and
  `TabBar` and returns early when the window is floating or has not yet been
  `Begin()`-ed. No new allocation, no raw `new`.
- **Pillar 4 (accessibility)**: neutral. Selecting the intended tab and focusing it is
  strictly better for keyboard nav than leaving nav focus on a stale modal.

## Perf-review-system gates

Diff touches `Source/Core/` — gates declared:

1. **PR-fast CI** — nearest scenario is `user-info-desktop-unified` (the bucket-C
   scenario this plan exists to stabilise); no grid/scroll path touched, so no
   `perf-pr-fast-set.json` entry is added.
2. **Pillar 2 static scanner** — N/A: no new sync I/O reachable from `ImGui::*`. The
   change removes a network call from ephemeral sessions.
3. **Dispatcher drain** — **fires**: `MainThreadDispatcher::Drain()` in the ephemeral
   `RunRenderLoop` is now conditional on `cfgInitialized`. Deferral is at most one
   frame and only before the first `Draw`; the in-`Draw` drain is untouched.
4. **Visible-cue bucket-E harness** — N/A: no new > 100 ms sync path.
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

## Risks / non-goals

- **Risk — `ephemeralSession` hides a real update prompt.** Accepted: an ephemeral
  spawn is a hidden-window process that serves one scripted command and exits; there
  is no user to prompt. The future is still drained, so no leak.
- **Risk — the drain gate defers a command by a frame.** Accepted: only for the frames
  before the first `Draw`, and the in-`Draw` drain still runs unconditionally.
- **Risk — `FocusWindow` on a docked window steals focus from a user's active tab.**
  Scoped: only fires on an explicit `requestUserInfoFocus` / `wantFocus`, which is
  already a "raise this window" intent.
- **Non-goal — the scenario double-tick.** `ScenarioRunner::Tick` is called twice per
  rendered frame in the ephemeral loop: once at the end of `SmatchetUI::Draw`
  (`SmatchetUI.cpp:642`) and again in `RunRenderLoop` after `SmatchetDrawFrameWithSeh`
  (`StandaloneAppBootstrap.cpp:685`). Consequences: `--warmupFrames=16` silently means
  8 rendered frames, and any scenario that **draws** in `OnFrame` submits its content
  twice into one ImGui frame. Visible today as duplicated code blocks in the
  `code-syntax-coloring` capture. Not fixed here — it changes frame semantics for
  every scenario and forces a full golden regeneration. Backlogged.
- **Non-goal — stale goldens.** All four `user-info-*` goldens are stale by a constant
  `linf=81` confined to `x=[273,296]`, `y=[8,19]`: the "Help" menu label, dim
  `(154,154,154)` in the golden vs bright `(232,232,232)` in every capture. PR #1937
  moved `drawMenuBarHelpMenu(ctx)` outside the `trackerLocked`
  `BeginDisabled()`/`EndDisabled()` block (`SmatchetUI_MainMenu.cpp:142-174`) and
  regenerated no goldens. Separately, `code-syntax-coloring`, `command-palette-fuzzy`
  and `dock-gap-sentinel` goldens date to 2026-05-26/31 and differ over the **whole**
  frame — window background `(15,15,15)` in the golden vs `(31,31,36)` in every
  capture, i.e. a theme-palette change since. Regenerating any golden is gated by
  `docs/agent-rules/golden-image-approval.md` and needs explicit user approval, so it
  is out of this plan.
- **Non-goal — eager pre-loop config init.** Loading config before the render loop
  would make the drain gate unnecessary, but it reorders bootstrap for every target.
  Backlogged.
- **Non-goal — unsynchronized `glReadBuffer(GL_FRONT)` readback.** Pre-existing
  capture-path hazard, unrelated to these three causes. Backlogged.

## Verification

- **Bucket A (ctest)**: N/A — all three fixes are ImGui-frame-ordering and
  process-identity behaviour, none reachable from a pure-logic unit.
- **Bucket E (ImGui Test Engine)**: `tests/ui/docked_tab_focus.test.cpp` — two variants
  over the real `SmatchetUI::selectDockedTab` (docked sibling → tab raised, with the
  `SetNextWindowFocus`-alone control; undocked / unknown window → no-op).
- **Bash-driver scenario / screenshot**: `bash scripts/dev/test-screenshot-diff.sh`;
  plus the scratchpad `repro-log.sh 20` loop over `user-info-desktop-unified`
  (`--spawn`, fresh `SMATCHET_USER_DATA` per run) as the determinism gate.
- **Build gate**: `bash scripts/dev/with-msvc-env.sh cmake --build build/ninja-iter-msvc`
  (dual-target `SmatchetStandalone` + `SmatchetCore_DX12`).
- **Doc validation**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: pending.
- **Manual residue**: golden regeneration is user-approval-gated by
  `docs/agent-rules/golden-image-approval.md` — deliberately manual, not residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep** — nothing previously documented as current is deferred by
this plan; the three non-goals above are new findings, not retracted scope.

- **Gate-escape postmortem for the stale goldens.** Bucket-C's golden diff is one of
  three sanctioned step-level CI masks (`AGENTS.md` § Merge gates), so PR #1937 shipped
  a golden-invalidating menu change with no CI signal. Owed a
  `gate-escape-postmortem` per `AGENTS.md` § Self-improvement loop.
- **Regenerating the seven stale goldens.** Needs user approval per
  `docs/agent-rules/golden-image-approval.md`.

## Implementation log

Shipped as PR #1962 on `claude/confident-allen-551b01`, in four commits:

1. `b089d2ad fix(screenshot)` — the three nondeterminism sources, exactly as
   § Files to modify lists them:
   - **Pre-`Draw` dispatcher-drain clobber.** `RunRenderLoop` drained
     `MainThreadDispatcher` before the first `SmatchetUI::Draw`, so an MCP command
     could apply config writes that `drawInitConfigOnce` then overwrote with the
     on-disk (first-run) values — including the scenario's
     `WhisperSetupCompleted = true`. Gated the pre-`Draw` drain on
     `d.cfgInitialized`; the in-`Draw` drain is untouched.
   - **Docked-tab focus.** `SetNextWindowFocus()` cannot raise a docked tab
     (upstream imgui#2304), so the User Info window sometimes rendered behind a
     sibling in the same node. New `SmatchetUI::selectDockedTab` writes
     `DockNode->TabBar->NextSelectedTabId` then `FocusWindow`s.
   - **Ephemeral update modal.** The startup update check fired in spawned
     sessions and could open the "Update Available" popup mid-capture. New
     `UiDrawSession::ephemeralSession`, latched from `forceMcp` in `Initialize`,
     gates both the check and the `OpenPopup`.
2. `e9a7afdc docs(self-improvement)` — backlog entries for the four non-goals
   (scenario double-tick, bucket-C golden mask, eager pre-loop config init,
   unsynchronized `glReadBuffer` readback).
3. `09693e74 test(golden)` — regenerated the four `user-info-*` goldens under
   explicit user approval (see § Deviations).
4. `0c13e450 test(bucket-e)` — `tests/ui/docked_tab_focus.test.cpp`, closing the
   deferral this plan's § Verification had originally taken.

## Deviations from plan

- **Golden regeneration moved in scope.** § Risks / non-goals declared all seven
  stale goldens out of scope pending approval. The user approved regenerating
  **exactly the four `user-info-*` PNGs** (`AskUserQuestion`, "Just the four
  user-info-*"), so those shipped here. `code-syntax-coloring`,
  `command-palette-fuzzy` and `dock-gap-sentinel` were restored with
  `git checkout --` and remain stale + masked — they need the `ScenarioRunner::Tick`
  double-call fixed first.
- **Bucket-E coverage un-deferred.** § Verification planned to defer the
  `selectDockedTab` test to a backlog entry. The test-delta merge gate correctly
  refused a `Source/Core/` diff with no test delta, so the test was written in this
  PR and the backlog entry deleted rather than filed.
- **`selectDockedTab` visibility.** Declared private per plan; made public so the
  bucket-E guard drives the real implementation instead of a replica that would
  keep passing after the imgui-internal workaround broke.

## Verification (actual)

- **Determinism gate (the user's acceptance criterion)**: 10 consecutive
  `bash scripts/dev/test-screenshot-diff.sh` runs, every one `user-info FAIL=0`.
  Before the fix the same loop reproduced the Whisper first-run banner on ~25% of
  spawns (diff band `y=[95,160]`, `linf=240`).
- **Per-cause loop**: scratchpad `repro-log.sh 20` over `user-info-desktop-unified`,
  fresh `SMATCHET_USER_DATA` per run — 0/20 deviations, measured twice (ledger in
  § Measurement ledger above).
- **Bucket E**: `ui_test.run --name=DockedTabFocus --spawn` → `passed=2 failed=0`.
- **Build gate**: `ninja-iter-msvc` (dual-target) and `ninja-ui-test-msvc` both green.
- **Delta lint**: `agents/scripts/project/test-lint-rules.sh --diff origin/develop`
  — all PASS; two advisory WARNs (`tu-line-ceiling` on the pre-existing
  `SmatchetUI.cpp`, comment-ratio on two headers).
- **Plan stress-test — `grill-with-docs`**: run pre-implementation; its findings are
  the § Risks / non-goals list.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
