- 2026-08-07 · claude-code · [process] · P2 — add a `code-review` checklist line: an existence check on an ImGui / dock / handle id must also assert the **containment** relationship, because ids are recycled *and persisted*

  Caught as a High in the [PR #1984](https://github.com/alexandrosk0/Smatchet/pull/1984)
  review. The first cut of `EnsureDockSlotAlive` was:

  ```cpp
  return ImGui::DockBuilderGetNode(slot) != nullptr ? slot : 0;
  ```

  which looks obviously correct and is obviously wrong. `DockBuilderGetNode` is a flat
  `DockContextFindNodeByID` map lookup (`imgui.cpp:20701-20705`). The orphan root nodes the
  guard exists to reject are written to `imgui.ini` under `[Docking][Data]` and **reloaded
  next launch** — so for every user who already ran the buggy build, the lookup succeeds for
  exactly the ids that must fail. The guard would have been a no-op on the whole installed
  base while passing every fresh-profile test.

  The fix was one clause: also require `node->ParentNode != nullptr`, since every constant in
  `SmatchetDockNodeIds.h` names a node the default layout cuts as a *child* of the dockspace
  root, so a null parent means the id resolved to a detached node.

  Generalised checklist line, for the `code-review` subsystem-invariants section under `Ui/`:

  > An existence check on an ImGui id, dock node, or opaque handle is not a validity check.
  > Ids are hashes — recycled across sessions and, for dock nodes, **persisted to `imgui.ini`**.
  > A lookup that succeeds proves an object exists, not that it is the object you meant.
  > Require the structural relationship too (parent / root / owning container), and name in a
  > comment which relationship the constant is supposed to satisfy.

  Broader than docking: the same shape applies to `ImGuiID` window lookups (`FindWindowByName`
  finds a stale window from a previous layout) and to any `id -> object` map that outlives a
  session.
