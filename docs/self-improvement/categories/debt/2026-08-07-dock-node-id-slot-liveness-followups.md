- 2026-08-07 · claude-code · [debt] · P2 — two follow-ups left open by the `EnsureDockSlotAlive` audit ([PR #1984](https://github.com/alexandrosk0/Smatchet/pull/1984))

  **1. Five sites hand-roll the liveness guard instead of routing through the helper.**
  `DockNextWindowOnFirstUse` covers the call sites whose shape is a bare
  `SetNextWindowDockID(<constant>, FirstUseEver)`. Five sites instead have an
  `Always`/`FirstUseEver` if-else that does not fit the helper's signature, so each calls
  `EnsureDockSlotAlive` itself and skips the write when it returns 0:
  [`SmatchetAiAssistantUi.cpp:1484,1501`](../../../../Source/Core/src/Ui/SmatchetAiAssistantUi.cpp),
  [`SmatchetMcpServerUi.cpp:94-100`](../../../../Source/Core/src/Ui/SmatchetMcpServerUi.cpp),
  [`SmatchetPerfUi.cpp:228-229`](../../../../Source/Core/src/Ui/SmatchetPerfUi.cpp),
  [`SmatchetUI_Layout.cpp:118-119`](../../../../Source/Core/src/Ui/SmatchetUI_Layout.cpp),
  [`LuaConsolePlugin.cpp:66-67`](../../../../Source/Plugins/LuaConsole/LuaConsolePlugin.cpp).

  So they are **correct today, and safe by construction** — each carries a comment saying so
  (e.g. `SmatchetMcpServerUi.cpp:91-93`). The debt is shape, not liveness: the invariant now
  lives in six places instead of one, and the next `Always`/`FirstUseEver` site added by
  someone reading these as the pattern will hand-roll it again — and eventually forget the
  `!= 0` arm, which is the original bug. Either widen the helper to take the condition, or
  split it into `DockNextWindowOnFirstUse` / `DockNextWindowAlways` sharing one guard.

  **2. `kSecondarySideBar` (0x10) names a slot that does not exist.** No
  `DockBuilderSplitNode` / `DockBuilderAddNode` call anywhere in `Source/` creates it — the
  only DockBuilder use is `SmatchetMobileShellUi.cpp:388-395`, which cuts mobile-only ids —
  and the embedded default ini (`ConfigManager.cpp:167-175`) contains only
  `0x08BD597D / 0x09 / 0x01 / 0x02 / 0x08 / 0x04 / 0x0A`. The assistant "Right →" swap
  feature was built entirely on the orphan-minting behaviour this branch removes, so with the
  guard in place that id now correctly resolves to nothing. The swap does not silently
  no-op — [`SmatchetAiAssistantUi.cpp:1484-1498`](../../../../Source/Core/src/Ui/SmatchetAiAssistantUi.cpp)
  falls back to the side that *does* exist and rewrites `AssistantPanelOnSecondarySide` +
  `ScheduleConfigSaveDetached` so the button label, tooltip, and persisted config describe
  where the panel actually went. That is correct degradation, but it means the user asks for
  "right" and durably gets "left" with no explanation. Decide explicitly: either cut a real
  node for it in the default layout, or delete the constant and the feature that depends on
  it. Leaving a constant that names no slot is how the original bug got written.

  Worth recording in `Source/Core/src/Ui/AGENTS.md` (which today has no docking section at
  all) once the decision lands. One more fact belongs in that same section, learned while
  refuting a bot finding on [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966):
  `ImHashStr` **skips** the `###` marker and everything before it — both `###` branches reset
  `crc` to the seed and advance the cursor past the two `#` characters before continuing — so
  `ImHashStr("Foo###Bar") == ImHashStr("Bar")`. A window
  id is `ImHashStr(name)`, which means a source title and its localized rendering hash
  identically as long as both carry the same `###` suffix — the reason localization does not
  break window-id lookups, and a non-obvious invariant to state once rather than re-derive.

  **Proposed gate for (2), and for the class:** assert every `constexpr ImGuiID k*` constant
  declared in [`Source/Core/include/Ui/SmatchetDockNodeIds.h`](../../../../Source/Core/include/Ui/SmatchetDockNodeIds.h)
  lines 10-14 appears as a `DockNode ID=` in the embedded default ini. A pure text check over
  two files in the same repo — no ImGui context needed — and it would have failed the day
  `kSecondarySideBar` was added.

  Enumerate the **header constants**, not the `kEntries` table in the `.cpp`: `kEntries` maps
  layout keys to slots, lives in an anonymous namespace, and references only `kCentralNode` /
  `kViewsColumn` / `kBottomPanel`. `kSecondarySideBar` never appears in it, so a gate driven
  off that table would have stayed green through exactly the bug it is proposed for.

  Context on why an existence check alone is insufficient:
  [`../process/2026-08-07-id-existence-checks-need-containment.md`](../process/2026-08-07-id-existence-checks-need-containment.md).
