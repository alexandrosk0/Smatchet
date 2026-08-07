- 2026-08-07 · claude-code · [test] · P2 — the two highest-severity fixes in [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966) ship with **zero** regression coverage; the 9 green bucket-E cases all predate them

  `tests/ui/window_expand.test.cpp` registers `ExpandCoversWorkAreaThenMinimizeRestores`,
  `SecondExpandMinimizesTheFirst`, `ClosedWhileExpandedSelfHeals`,
  `TitleBarToggleClickMinimizes`, `TabBarToggleClickExpandsThenMinimizes`,
  `StaleDockNodeRestoreFallsBackToFloating`, `MainGridPaneExpandsAndMinimizes`,
  `SplitterDragAfterMinimizeKeepsDocking`, `ScriptingWindowContributesTabBarToggle`.
  A 9/9 pass on that set is a **no-regression** signal only.

  Two uncovered fixes, both automatable:

  1. **Tab / toggle click overlap (MAJOR).** `ReserveTabBarSlot` submits a trailing tab
     item so `TabBarLayout` shrinks the central section
     (`ScrollingRectMaxX = BarRect.Max.x - sections[2].Width - sections[1].Spacing`) and a
     central tab's *hit box* — not just its pixels — stops short of the toggle. No existing
     case ever fills the bar, so nothing would notice if the reservation regressed.
     Shape: dock two windows into one node so the tab bar is full, `ItemClick` at the toggle
     rect, assert `ExpandedId` flipped **and** the selected tab did not change.
  2. **The two re-dock guards.** `SmatchetMcpServerUi` / `SmatchetAiAssistantUi` skip their
     `IsWindowDocked()`-driven redock latch while the window is expanded. Shape: expand the
     MCP window, step a frame, assert `pendingReDockWindows` stayed empty; minimize, assert
     the window returns to its node.

  Case 1 additionally pins the behaviour of `ImGui::TabItemSpacing`, which
  `ReserveTabBarSlot` now calls instead of a hand-rolled transparent `TabItemButton` — an
  upstream internal API, so a vendored-ImGui bump is exactly when a regression would land.
