# Plan: Enforce dock-slot-only windows (no free-floating)

## Context

Smatchet uses ImGui docking with four visible dock slots (Central, Views column, Primary sidebar, Bottom panel). The dockspace already has `ImGuiDockNodeFlags_NoUndocking` which prevents users from dragging windows out. However, windows can still end up floating if: (a) they have no DockId in the ini (Blame Analysis, Plan Docs), (b) the ini is corrupted, or (c) a window is opened for the first time with no persisted state. The goal is to guarantee every content window lands in a dock slot, and if it ever floats, it snaps back automatically.

## Dock slot layout

```text
┌──────────────────┬──────────┬────────────┐
│                  │  Views   │  Primary   │
│    Central       │  column  │  Sidebar   │
│    (0x02)        │  (0x08)  │  (0x04)    │
│                  │          │            │
├──────────────────┴──────────┴────────────┤
│              Bottom Panel (0x0A)          │
│  (Prefs, Log, Perf, Scripting, MCP, ...) │
└──────────────────────────────────────────┘
```

Secondary sidebar (0x10) is created on demand for AI Assistant side-swap.

## Window → slot mapping

| Layout key | Window title | Default slot |
|---|---|---|
| `active` | Smatchet - Active Project | Central (0x02) |
| `views` | Views - Jira (###SmatchetViewsDashboard) | Views column (0x08) |
| — | SmatchetViewsDashboard (sidebar widget) | Primary sidebar (0x04) |
| — | Smatchet Assistant | Primary/Secondary sidebar |
| `preferences` | Preferences | Bottom (0x0A) |
| `log` | Log | Bottom (0x0A) |
| `performance` | Performance | Bottom (0x0A) |
| `scripting` | Scripting | Bottom (0x0A) |
| `mcp` | MCP Server | Bottom (0x0A) |
| `bulk_import` | Bulk import tickets | Bottom (0x0A) |
| `bulk_export` | Bulk export tickets | Bottom (0x0A) |
| `audit` | Backend Audit | Bottom (0x0A) |
| `blame` | Annotate###BlameAnalysisModal | Bottom (0x0A) |
| `plandocs` | Plan docs | Bottom (0x0A) |

**Excluded (overlays/popups):** Command Palette, Attachment Preview, FPS Overlay, Dock Debug, Whisper Overlay, Whisper Setup Banner, Watchers, Votes.

## Approach

1. **Register every content window with a default dock slot** via a lookup function `SmatchetDockNodeIds::DefaultDockSlotForLayoutKey()`.
2. **Inject `SetNextWindowDockID` before `Begin()`** for every content window — `ImGuiCond_FirstUseEver` for normal use, `ImGuiCond_Always` for re-dock recovery.
3. **Detect floating after `Begin()` and schedule re-dock** on the next frame via `UiDrawSession::pendingReDockWindows` (since `SetNextWindowDockID` only works pre-Begin).
4. **Add missing ini entries** for Blame Analysis and Plan Docs.

## Changes

### 1. Expand `SmatchetDockNodeIds.h`

Add `kCentralNode` (0x02), `kViewsColumn` (0x08), and a free function:
```cpp
ImGuiID DefaultDockSlotForLayoutKey(const char* layoutKey);
```
Returns the default dock slot for a layout key, or 0 for unregistered overlay/popup windows.

### 2. New file: `SmatchetDockNodeIds.cpp`

Implements `DefaultDockSlotForLayoutKey` — a simple linear scan of ~14 `{key, slot}` pairs.

### 3. Add `pendingReDockWindows` to `UiDrawSession`

In `SmatchetUiSession.h`:
```cpp
std::unordered_set<std::string> pendingReDockWindows;
```
Tracks windows detected floating after `Begin()` so the next frame's `prepareTopLevelWindow` can force-dock them with `ImGuiCond_Always`.

### 4. Modify `prepareTopLevelWindow` (SmatchetUI_Layout.cpp)

Before the existing `SetNextWindowSize`, add:
```cpp
const ImGuiID slot = SmatchetDockNodeIds::DefaultDockSlotForLayoutKey(layoutKey);
if (slot != 0) {
    const bool needsForce = d.pendingReDockWindows.erase(layoutKey) > 0;
    ImGui::SetNextWindowDockID(slot, needsForce ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
}
```
**Signature change**: `d` becomes `UiDrawSession&` (was `const UiDrawSession&`). All call sites already pass non-const; no source changes needed at call sites.

### 5. Modify `repairTopLevelWindow` (SmatchetUI_Layout.cpp)

After the `IsWindowDocked()` early return, add:
```cpp
const ImGuiID slot = SmatchetDockNodeIds::DefaultDockSlotForLayoutKey(layoutKey);
if (slot != 0) {
    d.pendingReDockWindows.insert(layoutKey);
    return;  // Will be force-docked next frame by prepareTopLevelWindow
}
```
**Signature change**: same `const` removal.

### 6. Add `SetNextWindowDockID` to bypass windows

Windows that don't use `prepareTopLevelWindow`:

- **BlameAnalysisUi_Window.cpp** (~line 31): Add `SetNextWindowDockID(kBottomPanel, ImGuiCond_FirstUseEver)` before Begin.
- **SmatchetPlanDocViewerUi.cpp** (~line 210): Same.
- **LuaConsolePlugin.cpp** (~line 370): Same.
- **SmatchetMcpServerUi.cpp** (line 103): Change `SetNextWindowDockID(0, ...)` to `SetNextWindowDockID(kBottomPanel, ...)`. Also add `ImGuiCond_FirstUseEver` dock call in the non-reset path.
- **SmatchetAiAssistantUi.cpp**: Already has its own `SetNextWindowDockID` logic — no change.

Note: bypass windows lack `UiDrawSession` access so they can't participate in the frame-delayed re-dock pattern. This is acceptable: `NoUndocking` on the dockspace prevents runtime undocking, and `FirstUseEver` covers the cold-start case. Only ini corruption could leave these floating, and a layout reset fixes that.

### 7. Add missing ini entries (ConfigManager.cpp)

Add to `kDefaultImGuiDockLayoutIni`:
```ini
[Window][Annotate###BlameAnalysisModal]
DockId=0x0000000A,8

[Window][Plan docs]
DockId=0x0000000A,9
```

## Files modified

| File | Change |
|---|---|
| `Source_Core/include/SmatchetDockNodeIds.h` | Add `kCentralNode`, `kViewsColumn`, `DefaultDockSlotForLayoutKey()` |
| `Source_Core/src/SmatchetDockNodeIds.cpp` | **New** — lookup implementation |
| `Source_Core/include/SmatchetUiSession.h` | Add `pendingReDockWindows` set |
| `Source_Core/include/SmatchetUI.h` | `const UiDrawSession&` → `UiDrawSession&` on prepare/repair |
| `Source_Core/src/SmatchetUI_Layout.cpp` | Dock enforcement in prepare/repair |
| `Source_Core/src/ConfigManager.cpp` | Add Blame + Plan Docs ini entries |
| `Source_Core/src/BlameAnalysisUi_Window.cpp` | `SetNextWindowDockID` before Begin |
| `Source_Core/src/SmatchetPlanDocViewerUi.cpp` | `SetNextWindowDockID` before Begin |
| `Plugins/LuaConsole/LuaConsolePlugin.cpp` | `SetNextWindowDockID` before Begin |
| `Source_Core/src/SmatchetMcpServerUi.cpp` | Fix dock ID from `0` to `kBottomPanel` |

## Verification

1. `cmake --build --preset ninja-iter-clang --target SmatchetStandalone SmatchetCore_DX12` — both targets clean
2. Launch Smatchet, open every content window via menu — verify all dock into slots, none float
3. Delete `imgui.ini`, restart — verify all windows still dock correctly on fresh start
4. Try to drag a window tab to another dock slot — verify it tabs in cleanly
5. Drop a window tab in empty space — verify it snaps back to its default slot
6. Restart after repositioning — verify user-chosen slot persists (saved in ini)
7. Reset layout — verify all windows return to default slots

## Amendment: allow drag-and-snap between slots

### Problem

`ImGuiDockNodeFlags_NoUndocking` on the DockSpace prevents all undocking — including dragging windows between dock slots. ImGui requires a temporary floating state during tab drag, which `NoUndocking` blocks entirely.

### Solution

1. **Remove `ImGuiDockNodeFlags_NoUndocking`** from all three DockSpace calls (`StandaloneAppBootstrap.cpp`, `main.cpp`, `SmatchetImGuiHost.cpp`). This allows users to drag window tabs between any of the four slots.

2. **Guard re-dock scheduling against active drags** — in `repairTopLevelWindow` and all bypass windows, only schedule re-dock when `!ImGui::IsMouseDown(0)`. This prevents the enforcement from fighting an in-progress drag:
   - During drag (mouse held): window is temporarily floating, no re-dock scheduled
   - Drop on another dock node: window docks into target, `IsWindowDocked()` true → no re-dock
   - Drop in empty space (mouse released, still floating): re-dock scheduled → snaps to default slot next frame

3. **User's intentional slot changes persist** — ImGui saves DockId per window in `imgui.ini`. When a user drags a window from Bottom to Primary Sidebar, the new slot is persisted. `SetNextWindowDockID(..., FirstUseEver)` only fires on first-ever use, so it won't override the saved position on subsequent launches.

### Files modified

| File | Change |
|---|---|
| `Target_Standalone/StandaloneAppBootstrap.cpp` | Remove `NoUndocking` flag |
| `Target_Standalone/main.cpp` | Remove `NoUndocking` flag |
| `Source_Core/src/SmatchetImGuiHost.cpp` | Remove `NoUndocking` flag |
| `Source_Core/src/SmatchetUI_Layout.cpp` | Guard `pendingReDockWindows.insert` with `!IsMouseDown(0)` |
| `Source_Core/src/SmatchetPerfUi.cpp` | Guard `s_needsReDock` with `!IsMouseDown(0)` |
| `Source_Core/src/SmatchetAiAssistantUi.cpp` | Guard `s_assistantNeedsReDock` with `!IsMouseDown(0)` |
| `Plugins/LuaConsole/LuaConsolePlugin.cpp` | Guard `pendingReDockWindows.insert` with `!IsMouseDown(0)` |
| `Source_Core/src/SmatchetMcpServerUi.cpp` | Guard `pendingReDockWindows.insert` with `!IsMouseDown(0)` |

### Verification

1. Build clean
2. Drag a window tab from Bottom to Primary Sidebar → tabs into sidebar
3. Drag a window tab to empty space → snaps back to its default slot
4. Restart → window stays in user-chosen slot (persisted in ini)
5. Reset layout → all windows return to default slots

## Implementation log

- `f081d80` (2026-05-27) — initial enforcement: `SmatchetDockNodeIds` registry, `pendingReDockWindows`, `prepareTopLevelWindow` / `repairTopLevelWindow` enforcement, missing ini entries for Blame + Plan Docs.
- `1a2ecf1` — follow-up enforcement for bypass windows: Performance (`s_needsReDock`), Lua (`pendingReDockWindows`), MCP (`pendingReDockWindows`), AI Assistant (`s_assistantNeedsReDock`). All four lacked float-detection re-dock before this.
- `5e352e5` — drag-and-snap amendment: removed `ImGuiDockNodeFlags_NoUndocking` from all three DockSpace call sites, added `!IsMouseDown(0)` guard to all re-dock scheduling sites so enforcement doesn't fight in-progress drags.
- `0907200` — bumped `kCurrentLayoutSchemaVersion` to 7 to trigger `WriteDefaultImGuiSettingsFile()` on next launch, clearing stale `imgui.ini` state that blocked drag after `NoUndocking` removal.

## Deviations from plan

None for the core plan. The drag-and-snap amendment required three extra changes not in the original scope:

1. `SmatchetPerfUi.cpp` and `SmatchetAiAssistantUi.cpp` needed `!IsMouseDown(0)` guards — these bypass windows were missed in the original bypass-window list.
2. Schema version bump to 7 was not planned but required to clear cached `NoUndocking` state from existing `imgui.ini` files.
