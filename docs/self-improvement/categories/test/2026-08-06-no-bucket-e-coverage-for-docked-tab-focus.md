- 2026-08-06 · claude-code · [test] · P2 — `SmatchetUI::selectDockedTab` (the docked-tab focus fix from PR #1962) has no bucket-E coverage; its only gate is a bucket-C golden diff that is currently masked

  [`SmatchetUI_Layout.cpp`](../../../../Source/Core/src/Ui/SmatchetUI_Layout.cpp)
  gained `selectDockedTab(const char*)`, which raises a **docked** tab by writing
  `window->DockNode->TabBar->NextSelectedTabId = window->TabId` and then calling
  `ImGui::FocusWindow(window)`. It exists because `ImGui::SetNextWindowFocus()`
  cannot raise a docked tab — upstream leaves `FocusWindow`'s tab-selection lines
  commented out (ocornut/imgui#2304). Two callers today: the User Info focus path
  in `drawSecondaryWindows`, and `beginPreferencesWindow`.

  The behaviour is exactly the kind ImGui Test Engine tests well and pixels test
  badly: assert that after requesting focus on a window sharing a dock node with a
  sibling, `DockNode->TabBar->NextSelectedTabId` (and, one frame later,
  `VisibleTabId`) names the requested window. Today the only thing that would
  notice a regression is a `user-info-*` screenshot capturing the Preferences tab
  instead — and that verdict is discarded by the sanctioned bucket-C mask
  ([`tooling/2026-08-06-bucket-c-golden-mask-hides-stale-goldens.md`](../tooling/2026-08-06-bucket-c-golden-mask-hides-stale-goldens.md)).
  Two upstream-fragile assumptions make this worth pinning: the ImGui #2304
  workaround is undone the moment upstream restores those lines, and the
  `window->DockNode->TabBar` chain is `imgui_internal.h` API with no stability
  guarantee.

  Deferred from PR #1962 rather than skipped: the fix was one of three landing
  together under a determinism investigation, and wiring a new bucket-E case was
  out of that slice's scope. Concrete action — add a case to the existing bucket-E
  suite (`cmake --build --preset ninja-ui-test-msvc`) that docks two windows into
  one node, calls the focus path, pumps a frame, and asserts the visible tab id.
