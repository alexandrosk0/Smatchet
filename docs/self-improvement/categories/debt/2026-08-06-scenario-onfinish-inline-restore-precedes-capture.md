# Screenshot scenarios still unwind state inline in `OnFinish`, before the captured frame draws

- **Category**: debt
- **Priority**: P2
- **Date**: 2026-08-06
- **Source**: PR #1952 (bucket-C determinism) — root-cause of the four `user-info-*` L_inf 240 failures

## What

`ScenarioRunner::Tick` runs from `SmatchetUI::drawPreWindowOverlays`, which is
near the TOP of `SmatchetUI::Draw` — before any window draws. So a scenario's
`OnFinish` (which stages `g_ui.requestScreenshot` for the post-swap handler) runs
*ahead of* the rendering of the very frame that gets captured. Any state a
scenario unwinds inline in `OnFinish` is therefore unwound **into the capture**.

This bit `UserInfoScreenshotScenario`: its inline `restoreState()` reset
`WhisperSetupCompleted` and cleared the identity pins, and the captured PNG showed
the first-run whisper-consent banner over an empty User Info body (L_inf 240 vs a
tolerance of 4). It was latent for as long as `StandaloneAppBootstrap.cpp` ran a
*second* `Scenarios().Tick` after `SmatchetDrawFrameWithSeh` — that post-render
tick absorbed `OnFinish`, so the unwind genuinely did land after the frame.
Removing the duplicate tick (this PR, and the fix the
`scenario-runner-ticks-twice-per-frame` entry asks for) exposed it.

Fixed for `UserInfoScreenshotScenario` via `QueuePostCaptureRestore()` in
`Commands/Scenarios/ScenarioCaptureQuiesce.h`, drained one frame later at the top
of `drawPreWindowOverlays`.

## Residue

The three ambient bucket-C scenarios still restore inline in `OnFinish`:

- `CommandPaletteFuzzyScenario.cpp` — `g_ui.cfg.BackendHasBeenReachable` +
  `RestoreCaptureQuiesce()`
- `DockGapSentinelScenario.cpp` — `RestoreCaptureQuiesce()`
- `CodeSyntaxColoringScenario.cpp` — `RestoreCaptureQuiesce()`

They pass today (their restored fields either only gate surfaces already drawn
earlier in the same frame, or feed an async check that cannot land within the
frame), so this is latent, not live — which is exactly why it should be closed
before someone reorders `SmatchetUI::Draw` and it becomes another 240-L_inf
bisect. Not folded into #1952 because touching a green, byte-matching capture
path costs a golden re-approval round for no observed defect.

## Fix

Move every `OnFinish` unwind in `Source/Core/src/Commands/Scenarios/*Scenario*.cpp`
behind `QueuePostCaptureRestore(...)`, keeping the `OnCancel` paths inline (no
capture is pending there). Then state the rule once in
`Source/Core/src/Commands/AGENTS.md`: **a screenshot scenario's `OnFinish` runs
before the captured frame is drawn — stage the capture there, restore afterwards.**

A gate is plausible but not obviously cheap: a grep rule flagging a
`restore`/`Restore` call in an `OnFinish` body of a TU that also writes
`g_ui.requestScreenshot` would cover the class. Worth scoping when the fix lands.
