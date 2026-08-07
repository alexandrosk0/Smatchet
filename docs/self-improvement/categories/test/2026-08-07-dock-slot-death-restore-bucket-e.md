- 2026-08-07 · claude-code · [test] · P2 — no bucket-E case covers "drag the last bottom-panel tab out, then restore", which is exactly the state [PR #1984](https://github.com/alexandrosk0/Smatchet/pull/1984) changed the behaviour of

  `EnsureDockSlotAlive` makes a forced dock into a **dead** slot a no-op instead of
  minting an orphan root node. The observable consequence: a bottom-panel window
  whose slot (`kBottomPanel`, `0x0A`) no longer exists now snaps to
  `DefaultLayoutRectFor` as a visible floating window, where before it appeared to
  re-collect into a re-minted `0x0A` and only popped out on the next splitter drag.

  The `Dock` bucket-E filter (6 tests) exercises the *live*-slot paths. Nothing
  drives the slot to death first, so the new branch — the whole point of the change
  — is uncovered by automation and was verified only by reasoning about
  `DockBuilderGetNode` semantics plus the review's read of `imgui.cpp`.

  Suggested shape, for `test-author`:
  1. Boot with the default layout, assert `DockBuilderGetNode(0x0A) != nullptr`.
  2. Undock every tab in the bottom panel (ImGui Test Engine `ItemDragAndDrop` on
     each tab out to a free viewport point, or `DockBuilderRemoveNode(0x0A)`
     directly if the drag proves flaky).
  3. Assert `EnsureDockSlotAlive(kBottomPanel) == 0`.
  4. Trigger a redock request (`pendingReDockWindows` via the layout-reset command)
     and assert the window is **floating at `DefaultLayoutRectFor`**, not parented
     to a fresh root node — i.e. assert `window->DockNode == nullptr` rather than
     merely `DockIsActive == false`.
  5. Re-create the node, re-trigger, assert the latch was **retained** and the
     window docks — the "consume only against a live slot" policy this PR made
     uniform across all six pending-redock sites.

  Step 4's assertion is the one that matters: the pre-fix code would have passed a
  naive `IsDocked()` check, since an orphan root node *is* a dock node.
