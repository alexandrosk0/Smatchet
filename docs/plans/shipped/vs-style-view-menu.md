# VS Code shell: classic menu bar + View menu + Command Palette input + locked docking

<!-- plan-date: 2026-05-12 -->
<!-- index-summary: VS Code shell — classic menu bar, View menu around VS Code "Views" concept, embedded Command Palette input, locked docking. -->

## Context

Smatchet's menu bar groups items by workflow (Workspace / Selection / Issues / Automation / Inspect / Settings). Users want the VS Code shell instead — classic top-level menu bar, a Command Palette input field embedded in the menu-bar strip, a `View` menu built around VS Code's "Views" concept (per [code.visualstudio.com/docs/getstarted/userinterface#_views](https://code.visualstudio.com/docs/getstarted/userinterface#_views)), dockable side/bottom layout with locked undocking, and a VS-like theme. Floating tool windows go away.

### "Views" per VS Code docs

VS Code "Views" = specialized panels in the Primary Side Bar, movable to the Secondary Side Bar, opened via **View: Open View…**.

Smatchet mapping:
- **Side-bar View** = dockable tool panel in a side bar (Views Dashboard, Source Blame, Bulk Import/Export, Preferences).
- **Panel** = bottom dock (Log, Backend Audit, Performance, MCP Server, Scripts).
- **Editor area** = CentralNode hosting Active Project grid + future doc tabs.

To avoid collision with Smatchet's existing "view" domain term (saved JQL / Plane query), this design uses "side-bar View" in user-facing strings. `View > Open View…` opens the side-bar-View picker; saved queries stay under `File > Open Project View…`.

## Goals

1. Classic top-level menu bar `File / Edit / Selection / View / Go / Run / Tools / Help`.
2. Command Palette input field in the menu-bar strip, clickable + `Ctrl+Shift+P`.
3. `View` menu structured around side-bar Views, Panel, Appearance, Editor Layout.
4. No undocking — every tool window stays in its dock node.
5. Default dock layout: central editor, primary side bar (right by default), secondary side bar (left, reserved), bottom panel.
6. Theme variants via Appearance; default `SmatchetDark` is the current palette bit-identical.

## Out of scope

- New panels.
- Multi-viewport (`ImGuiConfigFlags_ViewportsEnable`) stays off.
- Renaming saved-query / project-view concepts.
- Watchers / Votes / Attachment Preview popouts — floating windows opened by a grid field button. Must carry `ImGuiWindowFlags_NoDocking` so the dockspace never captures them.
- Activity bar (icon column).
- VS Code editor-only entries (Minimap, Breadcrumbs, Sticky Scroll, Render Whitespace, Word Wrap, Editor Actions Position, Open Browser, Align Panel).

## Critical files

- [Source_Core/src/SmatchetUI.cpp](../../Source_Core/src/SmatchetUI.cpp) — `drawMainMenuBar()` (725), `resetWindowLayoutToDefault()` (330).
- [Source_Core/include/SmatchetUI.h](../../Source_Core/include/SmatchetUI.h)
- [Source_Core/src/SmatchetImGuiHost.cpp](../../Source_Core/src/SmatchetImGuiHost.cpp) — `DockSpaceOverViewport` at 598.
- [Source_Core/src/ConfigManager.cpp](../../Source_Core/src/ConfigManager.cpp) — `kDefaultImGuiDockLayoutIni` (383-489).
- [Source_Core/include/ConfigManager.h](../../Source_Core/include/ConfigManager.h)
- [Source_Core/src/SmatchetTheme.cpp](../../Source_Core/src/SmatchetTheme.cpp) + [SmatchetTheme.h](../../Source_Core/include/SmatchetTheme.h).
- [Source_Core/include/Commands/CommandPaletteUi.h](../../Source_Core/include/Commands/CommandPaletteUi.h) — gains `IsOpen`, `SetFilterText`, `OpenAnchoredBelow`, `AcceptTopResult`.
- [Source_Core/include/SmatchetLocalizedImGui.h](../../Source_Core/include/SmatchetLocalizedImGui.h) — extend existing `Begin` wrapper.

New headers:
- `Source_Core/include/SmatchetThemeIds.h` — `enum class ThemeId : std::uint8_t` only; no ImGui include.
- `Source_Core/include/SmatchetCommandPaletteConfig.h` — palette layout constants.
- `Source_Core/src/SmatchetStatusBarUi.{h,cpp}` — status bar.

### Panel `ImGui::Begin` inventory

| File:line | Window title | Destination |
|---|---|---|
| `SmatchetActiveProjectGridUi.cpp:129` | `Smatchet - Active Project` | CentralNode |
| `SmatchetViewsDashboardUi.cpp:135` | `SmatchetViewsDashboard` | Primary Side Bar (right) |
| `BlameAnalysisUi.cpp:1527` | `###BlameAnalysisModal` → `Source Blame` | Primary Side Bar (right) — drop modal flags |
| `SmatchetUtilityWindowsUi.cpp:117` | `Log` | Panel (bottom) |
| `SmatchetAuditUi.cpp:220` | `Backend Audit` | Panel |
| `SmatchetPerfUi.cpp:222` | `Performance` | Panel |
| `SmatchetMcpServerUi.cpp:270` | `MCP Server` | Panel `[if SMATCHET_WITH_MCP]` |
| `AppController_LuaBindings.cpp:1516` | `Scripting` | Panel `[if SMATCHET_WITH_LUA_AUTOMATION]` |
| `LuaConsole.h:95` | console | Panel (plugin) |
| `SmatchetBulkTicketsUi.cpp:134` | `Bulk import tickets` | Panel (hidden default) |
| `SmatchetBulkTicketsUi.cpp:480` | `Bulk export tickets` | Panel (hidden default) |
| `SmatchetPreferencesUi.cpp:102` | `Preferences` | Panel (hidden default) |
| `SmatchetAttachmentPreviewUi.cpp:612` | `Attachment Preview` | **Floating popout** — `NoDocking` |
| `TrackerGridFieldDisplay.cpp:820, 896` | `Watchers`, `Votes` | **Floating popouts** — `NoDocking` |

Excluded (intentionally non-dockable popups): `CommandPaletteUi.cpp:188`, `SmatchetAutocompleteUi.cpp:267`, `SmatchetPerfUi.cpp:319` FPS overlay.

## Design

### 0. Menu bar + Command Palette input

Replace `drawMainMenuBar` content with classic menus. Old groups removed; items redistributed.


```
File   Edit   Selection   View   Go   Run   Tools   Help        [  Command Palette  Ctrl+Shift+P  ]        [_][□][×]
```



Command Palette input UX = single-flow live filter: focus opens anchored popup, every keystroke filters, Enter selects top result, Esc closes both. Push/pop `ImGuiStyleVar_FramePadding` so the input matches menu-item height. Collapse to a magnifier icon button when the bar is narrower than the minimum input width. Reserve `kRightReservedPx` for the standalone window controls / Unreal `Close` button.

`Ctrl+Shift+P` opens the palette centered (existing behaviour, untouched).

#### Item redistribution

| Old location | New location |
|---|---|
| `Workspace > Grid` | `Go > Active Project Grid` |
| `Workspace > Views & Queries…` | `File > Open Project View…` |
| `Workspace > Reset Workspace Layout` | `View > Reset Layout` |
| `Selection > Select All / Clear / Copy` | `Selection > Select All / Clear Selection / Copy Selection` |
| `Issues > Import / Export` | `File > Import Issues… / Export Issues…` |
| `Automation > Scripts & Actions…` | `Run > Scripts & Actions…` + `View > Scripts & Actions` toggle |
| `Automation > Agent Bridge (MCP)…` | `Tools > MCP Server…` + `View > MCP Server` toggle |
| `Inspect > Source Blame…` | `View > Source Blame` toggle + `Go > Source Blame…` |
| `Inspect > Sync Audit…` | `View > Backend Audit` toggle |
| `Inspect > Runtime Log` | `View > Log` toggle |
| `Inspect > Performance Monitor…` | `View > Performance` toggle |
| `Settings > Preferences…` | `Tools > Preferences…` + `View > Preferences` toggle |
| `Settings > Check for Updates…` | `Help > Check for Updates…` |
| `Settings > Read-only Mode` | `File > Read-only Mode` |

#### Menu contents


```
File
 ├ New Issue…                       Ctrl+N
 ├ Open Project View…               Ctrl+O
 ├ ──
 ├ Import Issues…
 ├ Export Issues…
 ├ ──
 ├ ✓ Read-only Mode
 ├ ──
 └ Exit                             Alt+F4          (standalone only)

Edit
 ├ Cut                              Ctrl+X
 ├ Copy                             Ctrl+C
 ├ Paste                            Ctrl+V
 ├ ──
 └ Find in Grid…                    Ctrl+F

Selection
 ├ Select All                       Ctrl+A
 ├ Clear Selection                  Ctrl+Shift+A    (Esc rejected — ImGui consumes Esc for popup dismissal)
 └ Copy Selection                   Ctrl+Shift+C

View    (see § 1)

Go
 ├ Active Project Grid              Ctrl+Alt+1
 ├ Views Dashboard                  Ctrl+Alt+2
 ├ Log                              Ctrl+Alt+3
 ├ Backend Audit                    Ctrl+Alt+4
 ├ Performance                      Ctrl+Alt+5
 ├ ──
 ├ Go to Issue…                     Ctrl+P          (routed through inline palette, `issue:` prefix)
 └ Source Blame…

Run
 ├ Run Scenario…
 ├ Run Lua Script…                                  [if SMATCHET_WITH_LUA_AUTOMATION]
 └ Scripts & Actions…                               [if SMATCHET_WITH_LUA_AUTOMATION]

Tools
 ├ Preferences…                     Ctrl+,
 ├ MCP Server…                                      [if SMATCHET_WITH_MCP]
 └ Sync Now

Help
 ├ Documentation…
 ├ ──
 ├ Check for Updates…
 ├ ──
 └ About Smatchet…
```



Hide entries whose backing feature is not yet implemented — never ship greyed placeholders.

### 1. View menu


```
View
 ├ Command Palette…                 Ctrl+Shift+P
 ├ Open View…                       Ctrl+Shift+V
 ├ ──
 ├ Appearance ►
 ├ Editor Layout ►                                       (only when SMATCHET_ENABLE_EDITOR_LAYOUT)
 ├ ──                                                    (side-bar Views — primary side bar)
 ├ Views Dashboard                  Ctrl+Shift+E
 ├ Source Blame                     Ctrl+Shift+B
 ├ ──                                                    (Panel — bottom dock)
 ├ Log                              Ctrl+Shift+U
 ├ Backend Audit                    Ctrl+Shift+M
 ├ Performance
 ├ Bulk Import
 ├ Bulk Export
 ├ Preferences
 ├ MCP Server                                            [if SMATCHET_WITH_MCP]
 ├ Scripts & Actions                                     [if SMATCHET_WITH_LUA_AUTOMATION]
 ├ ──
 ├ Recently Used Views ►                                  (LRU of last 5 toggled side-bar Views / Panels)
 └ Reset Layout
```



`Open View…` opens the palette pre-filtered with `view.toggle.` — one command per toggle registered in the command registry under `view.toggle.<id>`. Single source of truth: menu, palette, and shortcuts all dispatch the same command.

Source Blame is a read-alongside view → right side bar with Views Dashboard. Bulk Import / Bulk Export / Preferences are task-shaped → bottom panel, hidden by default. Attachment Preview / Watchers / Votes are not in the View menu — they are floating popouts triggered by grid field buttons.

#### Appearance submenu


```
Appearance
 ├ Full Screen                      F11              (standalone only — entry hidden on Unreal embed)
 ├ Zen Mode                         Ctrl+M, Z        (chord; hide menu/sides/status/palette-input; Esc Esc exits)
 ├ Centered Layout
 ├ ──
 ├ ✓ Menu Bar
 ├ ✓ Primary Side Bar               Ctrl+B
 ├ ✓ Secondary Side Bar             Ctrl+Alt+B
 ├   Status Bar
 ├ ✓ Panel                          Ctrl+J
 ├ ──
 ├ Move Primary Side Bar {Right|Left}                (label flips based on cfg.PrimarySideBarOnRight)
 ├ Panel Position ►                                      (Bottom / Right / Left)
 ├ Tab Bar ►                                             (Visible / Hidden / Collapsed)
 ├ ──
 ├ Theme ►
 │   ├ ● Smatchet Dark (default — existing palette, bit-identical)
 │   ├   Modern Dark
 │   ├   VS 2022 Dark
 │   ├   VS 2022 Light
 │   └   High Contrast
 ├ Font ►                                                (mirror Preferences font combo)
 ├ Density ►                                             (Compact / Normal / Comfortable)
 ├ ──
 ├ Zoom In                          Ctrl+=
 ├ Zoom Out                         Ctrl+-
 └ Reset Zoom                       Ctrl+NumPad0
```



`Ctrl+M, Z` is a two-step chord (press Ctrl+M, release, press Z within ~1s). ImGui has no native chord API — custom helper with a frame-time accumulator. Clear chord prefix when a text input grabs focus mid-sequence.

#### Editor Layout submenu


```
Editor Layout
 ├ Split Up / Down / Left / Right
 ├ ──
 └ Single / Two Columns / Three Columns / Two Rows / Grid (2x2)
```



Gated behind `SMATCHET_ENABLE_EDITOR_LAYOUT` (off by default until doc-tab work lands — entry absent, not greyed).

#### VS Code → Smatchet mapping

| VS Code | Smatchet |
|---|---|
| Menu Bar (File / Edit / Selection / View / Go / Run / Terminal / Help) | Same, with Tools instead of Terminal |
| Quick Input | Inline `InputTextWithHint` in menu bar; opens `CommandPaletteUi` popup anchored below |
| Activity Bar | None |
| Primary Side Bar | Right dock node — Views Dashboard, Source Blame |
| Secondary Side Bar | Left dock node, reserved + hidden by default |
| Panel | Bottom dock node — Log, Audit, Perf, MCP, Scripts |
| Editor area | CentralNode — Active Project grid + future doc tabs |
| Status Bar | `SmatchetStatusBarUi` (new) |
| Zen Mode | Hide menu / sides / status / palette input; Esc Esc restores |
| View: Open View… | Palette pre-filtered to `view.toggle.*` |

### 2. Lock undocking

`ImGuiDockNodeFlags_NoUndocking` on the dockspace at `SmatchetImGuiHost.cpp:598`. Single line forbids any window in the dockspace from being dragged out.

Belt-and-braces: extend the existing `SmatchetLocalizedImGui::Begin` wrapper to OR-in `ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove`. All callers pick up the flags automatically.

**Exception — floating popouts** (Watchers / Votes / Attachment Preview): use raw `::ImGui::Begin` with `ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse`. They must remain movable and never be captured by the dockspace.

### 3. VS shell layout — rewrite default dock ini


```
Root (Split=X)
 ├ Left   (~200)         — Secondary Side Bar, hidden by default
 ├ Center (Split=Y)
 │   ├ Top (CentralNode) — `Smatchet - Active Project` (+ future doc tabs)
 │   └ Bottom            — Log, Backend Audit, Performance, MCP, Scripts, Bulk*, Preferences
 └ Right  (~320)         — `SmatchetViewsDashboard`, `Source Blame`

Floating popouts (not in dock tree):
 • Watchers, Votes, Attachment Preview — open at field-button position
```


Bottom-dock crowding: Bulk Import / Bulk Export / Preferences default `d.show* = false`. Always-on bottom tabs are Log / Backend Audit / Performance (+ MCP / Scripts when their build flags are set). Prefix the hidden-by-default sections in `kDefaultImGuiDockLayoutIni` with an INI comment so the leaked-looking entries are self-documenting.

`BlameAnalysisUi` drops modal flags, docks into right side bar.

`resetWindowLayoutToDefault` keeps current signature.

### 4. Theme + Appearance

`SmatchetTheme::ApplyStyle()` becomes `ApplyStyle(ThemeId)`. The current palette body moves to `ApplySmatchetDark` verbatim — current users see no visual change. New variants: `ModernDark`, `Vs2022Dark`, `Vs2022Light`, `HighContrast`. `ApplyStyle` is a `switch` with exhaustive `default:` fallback to `ApplySmatchetDark`.

`ThemeId` lives in `SmatchetThemeIds.h` (no ImGui include) so `ConfigManager.h` can reference it without dragging ImGui into every TU.

Density adjusts `FramePadding` / `ItemSpacing` / `WindowPadding` after the theme apply, so it is theme-independent.

`AppConfig` fields (enum class, `std::uint8_t` storage where applicable):
- `ThemeId Theme = SmatchetDark`
- `DensityId Density = Normal`
- `std::uint8_t FontSizePt = 16` (clamp [8, 32] at load)
- `bool ShowMenuBar / ShowStatusBar / ShowPrimarySideBar / ShowPanel = true`
- `bool ShowSecondarySideBar = false`
- `bool PrimarySideBarOnRight = true`
- `PanelPosition PanelDockSide = Bottom`
- `TabBarMode TabBarVisibility = Visible`
- `bool CenteredLayout = false`
- `std::uint32_t LayoutSchemaVersion = 0`
- Transient (not persisted): `bool ZenMode = false`, `bool FullScreen = false`

INI (de)serializers for enums at the on-disk boundary; runtime carries enums only. `Preferences` window keeps its own Theme / Font controls — same config fields.

### 4a. `SmatchetLocalizedImGui::Begin` localization pin

`WindowTitleFromSource` must return a process-lifetime-stable `const char*`. ImGui hashes window titles into IDs — a drifting pointer corrupts collapse state and dock-node membership. Back the lookup with a `static std::unordered_map<std::string, std::string>` (process lifetime, insert-only). Pointer stability comes from map-node stability.

### 5. Status bar

`ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down, GetFrameHeight(), …)` at the bottom of the viewport. Shows backend (Jira/Plane), online/offline, queued-ops count, in-flight indicator, FPS. Right-side chips for `Theme` and `LayoutSchemaVersion`; right-click → `Reset Layout` / `Revert Theme` so the migration is discoverable.

### 6. Recently Used Views

Fixed-capacity LRU (5 entries) on `SmatchetUI`, not in `AppConfig` (transient). `Touch(const std::string&)` on every `view.toggle.<id>` dispatch from any source. No `string_view` (banned by C++14-hard rule).

### 7. Side-bar visibility state machine

| Action | Effect |
|---|---|
| `d.showX = true` while `ShowPrimarySideBar == false` | Set flag, also set `ShowPrimarySideBar = true` |
| `ShowPrimarySideBar = false` while tabs open | Hide node, keep individual flags; restoring node restores tabs |
| Closing last visible tab via X | Hide tab only; do not hide node |
| `Reset Layout` | All `d.show*` and `cfg.Show*` revert to defaults |

Same rules for `ShowSecondarySideBar` / `ShowPanel`. Encode as one helper `SetViewVisible(cfg, flag, slot, visible)`.

### 8. Migration

`kCurrentSchemaVersion = 1U` (`constexpr std::uint32_t`). Commit-last order:
1. Read `cfg.LayoutSchemaVersion`. If `>= kCurrentSchemaVersion`, skip.
2. Call `resetWindowLayoutToDefault()`.
3. On success, set version and write `cfg.json` atomically (write-tmp + fsync + rename).
4. If step 2 fails, leave version untouched — retry next launch.

`ZenMode` and `FullScreen` are not persisted.

## Risk / invariants

- **C++14 hard**: no `string_view`, no `optional`, no `if constexpr`. Lambda-to-fn-pointer via `+[]` is C++14-legal.
- **Dual-target**: no GLFW/GL in `Source_Core/` headers. `glfwSetWindowMonitor` for Full Screen lives behind `#ifndef SMATCHET_EMBEDDED_IN_UNREAL` in `SmatchetImGuiHost.cpp`. Verify both targets compile: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12`.
- **`NoUndocking` flag**: confirmed in vendored docking-branch ImGui (`UnrealPlugins/.../imgui.h`). Sanity-check the symbol on the standalone include before patch.
- **Floating-popout `NoDocking`** is load-bearing — without it the dockspace `NoUndocking` glues popouts permanently on first dock.
- **`WindowTitleFromSource` pointer stability**: regression silently corrupts ImGui IDs. Add a pointer-equality unit test.
- **Chord `Ctrl+M, Z`**: custom helper, 1s frame-based timeout. Clear prefix when a text input grabs focus mid-sequence to avoid swallowed keystrokes.
- **New headers `SmatchetCommandPaletteConfig.h` and `SmatchetThemeIds.h`**: add to `Source_Core/CMakeLists.txt` if it lists headers explicitly; glob is automatic.
- **Inline palette `FramePadding`**: push/pop `ImGuiStyleVar_FramePadding` around the input so it matches menu-item height. Visual-verify both targets.
- **Captureless lambda**: keep the lambda body inside `SmatchetUI::drawMainMenuBar` so `+[]` → fn-pointer conversion stays valid.
- **`Clear Selection` shortcut**: `Ctrl+Shift+A`, not `Esc`. ImGui consumes `Esc` for popup dismissal.
- **Default ini comment**: prefix Bulk Import / Bulk Export / Preferences in `kDefaultImGuiDockLayoutIni` with `; hidden by default` so future readers don't think entries leaked.
- **Lint**: clang-format / cppcheck / clang-tidy auto-run via PostToolUse hook.

## Implementation order

1. `ImGuiDockNodeFlags_NoUndocking` on the dockspace (1 line).
2. `ImGuiWindowFlags_NoDocking` on Watchers / Votes / Attachment Preview `ImGui::Begin` sites.
3. Rewrite `kDefaultImGuiDockLayoutIni` + `LayoutSchemaVersion` migration (commit-last per §8).
4. Restructure menu bar to classic shell. Move every old item per the redistribution table.
5. Inline Command Palette input + `CommandPaletteUi::IsOpen / SetFilterText / OpenAnchoredBelow / AcceptTopResult`. Constants in `SmatchetCommandPaletteConfig.h`. Push/pop `FramePadding`.
6. Register `view.toggle.<id>` commands. Wire `View > Open View…` (palette pre-filter `view.toggle.`) and `Go > Go to Issue…` (`issue:`).
7. Convert `BlameAnalysisUi` modal → dockable side-bar View.
8. Extend `SmatchetLocalizedImGui::Begin` to OR-in `kSmatchetPanelFlags`.
9. `SmatchetThemeIds.h` + `enum class DensityId / PanelPosition / TabBarMode` + INI (de)serializers + `ApplyStyle(ThemeId)`. Theme submenu.
10. Density + Font submenus.
11. Zoom In / Out / Reset; persist `cfg.FontSizePt` clamped at load.
12. Side-bar / panel / menu-bar / status-bar / centered-layout toggles via `SetViewVisible(...)`.
13. Panel Position + Tab Bar submenus.
14. `Move Primary Side Bar {Right|Left}` swap.
15. Full Screen (standalone only).
16. Zen Mode chord.
17. Editor Layout submenu (gated).
18. `SmatchetStatusBarUi` + Theme / SchemaVersion chips.
19. Recently Used Views LRU.
20. Build both targets, lint, manual smoke.

Steps 1–6 = MVP (lock + popouts + new menu bar + palette input + command registry). 7–11 = appearance polish. 12–19 = VS feature parity.

## Verification

- `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — both compile.
- Fresh `imgui.ini` launch:
  - VS shell layout: secondary left hidden, central grid, right side bar (Views Dashboard + Source Blame tab), bottom panel (Log / Audit / Performance).
  - Menu bar: `File   Edit   Selection   View   Go   Run   Tools   Help`.
  - Palette input between menus and right zone; collapses to magnifier on narrow window.
  - Focus → anchored popup; typing filters live; Enter accepts top; Esc closes both.
  - `Ctrl+Shift+P` opens centered palette without touching inline buffer.
  - `View` menu order: Command Palette → Open View → Appearance → (Editor Layout) → side-bar Views → Panel → Recently Used Views → Reset Layout.
  - `View > Open View…` (`Ctrl+Shift+V`) lists side-bar Views + Panel; selecting shows + focuses.
  - Tab drag inside dockspace cannot undock.
  - Floating popouts: clicking Watchers / Votes / Attachment field button opens floating window; dragging over a dock target shows blocked cursor.
  - Default theme = `Smatchet Dark`, byte-identical to pre-upgrade output.
  - `Theme > VS 2022 Light` flips palette without restart.
  - Cycling themes returns to exact starting palette.
  - `Primary Side Bar` (Ctrl+B) toggles right column; closing last tab does not hide the node.
  - `Panel` (Ctrl+J) toggles bottom; opening `View > Bulk Import` while hidden auto-restores the node + selects the tab.
  - `Zen Mode` chord `Ctrl+M, Z` hides chrome; Esc Esc restores. Chord prefix clears if a text input grabs focus mid-sequence.
  - `Full Screen` (F11): standalone goes borderless; Unreal embed hides the entry.
  - `Zoom In/Out/Reset` (`Ctrl+=` / `Ctrl+-` / `Ctrl+NumPad0`) resize fonts; persisted clamped [8, 32].
  - `Move Primary Side Bar Right/Left` swaps without losing tabs; label flips.
  - `Recently Used Views` shows up to 5; LRU updates on any toggle source.
  - `Reset Layout` returns to new VS shell.
  - `Go > Active Project Grid` = `Ctrl+Alt+1` (`Ctrl+0` unbound).
  - `Go > Go to Issue…` (`Ctrl+P`) pre-fills inline palette with `issue:`.
- Existing install: `LayoutSchemaVersion < kCurrentSchemaVersion` triggers commit-last migration; kill between reset and bump retries next launch, not half-migrated.
- `ZenMode` / `FullScreen` reset to `false` on every launch.
- Lint clean on every touched `.cpp` / `.h`.
- Lua build (`-DSMATCHET_WITH_LUA_AUTOMATION=ON`) and MCP build show their menu entries; opposite builds hide them entirely.
- `WindowTitleFromSource("Log")` returns the same pointer on two consecutive calls.
