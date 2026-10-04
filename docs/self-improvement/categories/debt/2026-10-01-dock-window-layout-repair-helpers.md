- 2026-10-01 · deviation renewal (markers expiring 2026-11-30 / 12-01) · [debt] · P3 — Three window-layout repair helpers duplicate one dock-repair shape

Details:
`SmatchetUI::repairTopLevelWindow` (`Source/Core/src/Ui/SmatchetUI_Layout.cpp`),
`RepairMcpWindowLayout` (`Source/Core/src/Ui/SmatchetMcpServerUi.cpp`) and `RepairLuaWindowLayout`
(`Source/Plugins/LuaConsole/LuaConsolePlugin.cpp`) all do the same thing after `Begin`:

- return early when the window is docked or expanded;
- arm the re-dock latch (`pendingReDockWindows`) unless the mouse is down or just released;
- snap a too-small or off-screen floating window back to a default rect.

They differ only in data: the layout key, the minimum size, the size they grow to, and the fallback
position. `PrepareMcpWindowLayout` and `PrepareLuaWindowLayout` repeat the same pairing before
`Begin`. Two duplication markers exempt the clone today:
`LuaConsolePlugin.cpp` above `RepairLuaWindowLayout` (revisit 2027-03-31) and
`SmatchetMcpServerUi.cpp` above `ClampMcpWindowPos` and in `PrepareMcpWindowLayout` (revisit
2027-03-31).

Merging them is not a mechanical refactor. The MCP repair also fires during a layout reset
(`layoutForceDefaultsFrames`) while the Lua one does not, and each window grows to a different
size. A shared helper changes window geometry unless those differences become parameters. The Lua
console is a plugin TU, but it already includes Core's `SmatchetWindowExpand.h`,
`SmatchetDockNodeIds.h` and `SmatchetUiSession.h`, so a shared helper adds no new layer edge.

Concrete next action:
Add one helper next to `repairTopLevelWindow` that takes a small spec (layout key, minimum size,
grow-to size, fallback rect, whether layout reset forces the rect) and use it from the three call
sites, keeping each window's current numbers. Cover it with the bucket-E window-layout tests
(`tests/ui/window_expand.test.cpp`, the dock-slot tests) and check the MCP and Lua windows in the
bucket-C captures. Then remove both duplication markers.

Status: open
Last-reviewed: 2026-10-03
