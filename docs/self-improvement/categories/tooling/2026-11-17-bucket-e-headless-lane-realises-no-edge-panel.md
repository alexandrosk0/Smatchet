# Bucket-E under headless Mesa never realises an edge panel, so the keyboard panel-resize case cannot run there

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-17
- **Observed on**: PR #2440, `Bucket-E UI tests (Mesa headless GL)`, case `GestureKeys/PanelResize_KeysNudgeAVisiblePanel`. The case's own log line read `bottom node=0 parent=0 isVisible=0 windows=-1 showLog=1`, and no side bar was visible either.
- **Status**: open

## What happened

`view.panel.grow` / `view.panel.shrink` (piece 4.6) resize the bottom panel or a side bar through `SmatchetBottomPanelDrag::NudgePanelExtent`. The bucket-E case that presses the keys needs a realised, visible dock node. In the headless software-GL lane it has none: `DockBuilderGetNode(kBottomPanel)` is null at that point, and `SmatchetBottomPanelDrag::Expand` only sets show flags, so it cannot make one. `tests/ui/reset_layout_docking.test.cpp` already records the same property ("In Mesa-headless none are realized even when healthy") and skips on it.

The case now skips with a logged reason when the headless lane realises no node, and fails on a real-GL run. The keys, the command and the panel handlers are therefore not exercised end to end in CI. What does run: the bounds of the nudge (`NudgeTargetExtent` doctests, Linux and MSVC), command registration and the default bindings (bucket-E case A), and the `panel=focused`-with-no-panel result.

## Concrete next action

- Add a bucket-E primitive that gives a test a live bottom panel under the headless lane: either a seam that builds the default dock tree for the test (the `DockBuilder` calls the default layout makes, then a settle), or a headless-realisable fixture node that `EdgePanelNode` accepts (a parent and a host window).
- Then drop the skip in `PanelResize_KeysNudgeAVisiblePanel` and run the mutation check on `NudgePanelExtent`.

Last-reviewed: 2026-11-17
