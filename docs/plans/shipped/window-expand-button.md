# Plan — Window Expand Button

> **Slug**: `window-expand-button`
>
> **Status**: `shipped`

## Context

Smatchet's dockable windows (Log, Backend Audit, Preferences, the AI assistant, every
grid pane, …) can only be resized by dragging dock splitters. There is no one-click
"show me just this one, big" affordance, so reading a wide grid or a long log means
re-dragging the layout and dragging it back afterwards.

This feature adds an **Expand** control immediately LEFT of each window's close **X**.
Clicking it pins that window over the main viewport **work area** — the rect below the
main menu bar and above the status bar, so both stay reachable — covering every other
view. The same control then shows the **minimize** face and puts the window back exactly
where it came from.

**Intended outcome**: any dockable view can be taken fullscreen and returned to its dock
slot with one click, with the dock layout untouched.

Cross-link: N/A — new feature request.

## Approach

No dock-node surgery and no `.ini` rewriting. Expansion is a per-frame *override*:

1. **State** — `SmatchetWindowExpand::WindowExpandState` in `UiDrawSession`: the expanded
   window's `ImGuiID`, a map of pre-expand placements, a restore queue, the ids that
   submitted this frame, and the deferred docked-button queue. The struct is ImGui-free
   (`ImGuiID` is a plain `unsigned int`) so the transition core links into bucket-A.
2. **BeginWindow** — called immediately before each `ImGui::Begin`. For the expanded
   window it re-issues `SetNextWindowDockID(0)` + `SetNextWindowPos/Size(WorkPos/WorkSize)`
   every frame; for a window with a pending restore it replays the stored dock id (or
   float rect) for exactly one frame.
3. **DrawToggle** — called immediately after a `Begin` that returned true.
   - *Floating*: a manual button in the title bar, mirroring `RenderWindowTitleBarContents`'
     own right-to-left slot math, so it lands one slot left of the X.
   - *Docked*: a docked window has no title bar — its X lives on the dock node's tab bar.
     The button is queued and drawn at end-of-frame by re-`Begin`-ing the dock HOST window
     (the sanctioned pattern `DockNodeBeginAmendTabBar` uses) at
     `node->TabBar->BarRect.Max.x - FontSize`. `DockNodeCalcTabBarLayout` has already
     shrunk that edge past the node's close X, so this is glued to the right, exactly one
     slot left of the X.
4. **EndFrame** — drains the docked-button queue, then self-heals: an expanded window that
   did not submit is gone, so the expansion drops and its restore is armed.

**Trade-off — manual button, not `TabItemButton(ImGuiTabItemFlags_Trailing)`**: `Trailing`
is an *ordering* section, not right-alignment. `TabBarLayout` right-aligns it only once the
tabs overflow (`tab_offset = ImMin(BarRect.W - section.W, tab_offset)`), so with room to
spare it packs against the last tab — visibly left-aligned, which is what the first cut
shipped and the user rejected.

**Trade-off — override per frame, not a real undock**: no node is split, merged or
destroyed, so the layout the user built is still there to replay the dock id back into.
Two limits this does NOT buy, both recorded in `SmatchetWindowExpand.h` and in
§ Deviations: it is **not `.ini`-safe** (ImGui's settings writer snapshots `Pos`/`Size`/
`DockId` off the *live* window, so quitting while expanded persists floating+fullscreen and
relaunch re-docks to the DEFAULT slot), and minimize does not always land in the **original**
node (undocking the last tab out of a leaf destroys that node).

## Files to modify

1. `Source/Core/include/Ui/SmatchetWindowExpand.h` (new) — state structs + the
   `BeginWindow` / `DrawToggle` / `IsWindowExpanded` / `IsCurrentWindowExpanded` /
   `ToggleWindow` / `Reset` / `EndFrame` surface + the pure `detail::` core.
2. `Source/Core/src/Ui/SmatchetWindowExpand.cpp` (new) — the ImGui half: glyph, title-bar
   slot math, dock-host tab-bar placement, the per-frame geometry override.
3. `Source/Core/src/Ui/SmatchetWindowExpand_detail.cpp` (new) — the pure transition core
   (`ApplyToggle` / `ConsumeRestore` / `SelfHeal`), bucket-A linkable.
4. `Source/Core/include/Ui/SmatchetUiSession.h` — `WindowExpandState windowExpand;`.
5. `Source/Core/src/Ui/SmatchetUI.cpp` / `SmatchetUI_Layout.cpp` — `EndFrame` once per
   frame; `Reset` on layout reset; `repairTopLevelWindow` skips the expanded window.
6. The 16 window TUs (`SmatchetAuditUi.cpp`, `SmatchetPerfUi.cpp`,
   `SmatchetActiveProjectGridUi.cpp`, …) — the two-line `BeginWindow` / `DrawToggle` pair.
7. `Source/Core/src/SmatchetLocalization.cpp` — Expand / Minimize tooltips.
8. `tests/Core/WindowExpand.test.cpp`, `tests/ui/window_expand.test.cpp`,
   `scripts/dev/test-ui-window-expand.sh` (all new) + the two `CMakeLists.txt` and
   `ui_tests_registry.cpp` wiring.

## Existing utilities reused

- `prepareTopLevelWindow` / `repairTopLevelWindow` — existing window setup helpers.
- `SmatchetLocalizedImGui` / `SmatchetLocalization` — localized tooltips.
- `ImGui::PushOverrideID` + `ImHashStr` — per-instance ids for the manual buttons.
- Bucket-E `BootedAppOrSkip` / `YieldUntil` / `WindowIsLive` fixture shape.

## Extraction sizing

The ImGui half is a new ~280-line TU; the pure core is a separate ~70-line TU so bucket-A
can link it without an ImGui context. Both are well under the TU ceiling; no existing file
is split.

## UX Pillar callouts

- **Pillar 1 (perf, 6.94 ms)**: per submitted window, one map lookup and (only for the
  expanded one) two `SetNextWindow*` calls. No allocation in the steady state — the
  vectors are cleared, not freed.
- **Pillar 2 (never freezes)**: pure state mutation, no I/O, no locks.
- **Pillar 3 (never crash)**: every dock-node pointer is re-fetched through
  `DockBuilderGetNode` and null-checked (a stale node id from a previous session is the
  real failure mode); the end-of-frame self-heal drops an expansion whose window vanished.
- **Pillar 4 (accessibility)**: the control is a real `ImGui::Button` on the menu nav
  layer with a tooltip, so it is reachable by keyboard nav like the close X. Icon contrast
  follows the window's text colour.

## Perf-review-system gates

Diff touches `Source/Core/` → gate required. The added work is O(submitted windows) with
no allocation, no sync I/O, no new markers; the affected scenario set is the UI-idle /
grid-scroll pair. Verified against the standard PR-fast perf lane — no marker changes, so
no baseline re-record.

## Risks / non-goals

**Risks**:
- A dock id saved before a layout reset can be stale → `BeginWindow` falls back to
  replaying pos/size when `DockBuilderGetNode` returns null.
- The main grid pane runs with `ImGuiWindowFlags_NoTitleBar`; expanded it is floating,
  which would leave the minimize face nowhere to draw → the flag lifts for those frames
  (`IsWindowExpanded`, which is callable *before* `Begin`).
- A window that force-redocks itself every frame would fight the pin →
  `IsCurrentWindowExpanded` gates `repairTopLevelWindow` and `SmatchetPerfUi`'s self-redock.

**Non-goals**:
- No persistence of the expanded state across sessions.
- No animation.
- No menu-bar / status-bar hiding ("zen mode") — the work area is the deliberate bound.

## Verification

- **Bucket A** (`tests/Core/WindowExpand.test.cpp`): the pure transition core — toggle,
  hand-over between two windows, consume-once restore, self-heal.
- **Bucket E** (`tests/ui/window_expand.test.cpp`, 7 tests): expand really covers the work
  area undocked; minimize really returns to the node it left; a second window takes the
  slot and the first drops back; closing while expanded self-heals; both faces of the real
  control click; the docked face is **glued to the tab bar's right edge**
  (`RectFull.Max.x == BarRect.Max.x`); the main grid pane expands and minimizes too.
- **Driver**: `bash scripts/dev/test-ui-window-expand.sh` (zero-run floor, JSON envelope).
- **Build gate**: `SmatchetStandalone` + `SmatchetCore_DX12`.
- **Doc validation**: `bash scripts/dev/test-docs.sh`.

## Out of scope (flagged, not designed)

- Persisting the expanded window across sessions.
- A keyboard shortcut (e.g. F11) to toggle expand.
- Expand animation / smooth transition.
- Multi-monitor aware expansion (the main viewport work area is the bound).
- Mobile/tablet touch gesture alternative.

## Implementation log

1. **State** — `SmatchetWindowExpand::WindowExpandState windowExpand` added to
   `UiDrawSession`. Session-only, never persisted.
2. **Pure core** — `detail::ApplyToggle` / `ConsumeRestore` / `SelfHeal` in
   `SmatchetWindowExpand_detail.cpp`, ImGui-free and bucket-A covered.
3. **Geometry override** — `BeginWindow` pins the expanded window to
   `viewport->WorkPos/WorkSize` with `SetNextWindowDockID(0)` each frame and replays a
   pending restore for one frame; a dead dock id degrades to replaying pos/size.
4. **Control** — `DrawToggle` draws a manual button one slot left of the X: in the title
   bar when floating, over the dock node's tab bar (right-glued at `BarRect.Max.x`, drawn
   at end-of-frame into the dock HOST window under `PushOverrideID(node->ID)`) when docked.
   The glyph is drawn from `ImDrawList` primitives — no font dependency.
5. **Integration** — the `BeginWindow` / `DrawToggle` pair added to all 16 dockable window
   TUs plus every grid pane, including the main pane; `EndFrame` once per frame;
   `Reset` on layout reset.
6. **Main-pane title bar** — `IsWindowExpanded` (hash-based, callable before `Begin`) lifts
   `ImGuiWindowFlags_NoTitleBar` on the main grid pane while it is expanded.
7. **Tests** — bucket-A `tests/Core/WindowExpand.test.cpp`; bucket-E
   `tests/ui/window_expand.test.cpp` (7 cases) + `scripts/dev/test-ui-window-expand.sh`.

## Deviations from plan

The original plan was written against a file layout this repo does not have, and against a
different feature: it proposed hiding *sibling grid panes* behind a pane-strip toggle. The
user re-specified mid-flight — every dockable view, a control next to the X, fullscreen
over the other views — so the pane-hiding UI was dropped and the machinery reused.

| # | Plan said | Shipped | Why |
|---|---|---|---|
| a | `expandedPaneId` (a pane id) in `SmatchetUI.h` | `WindowExpandState` (an `ImGuiID`) in `SmatchetUiSession.h` | Scope became *every dockable window*, not just grid panes, so the key is the window id. `UiDrawSession` lives in `SmatchetUiSession.h`. |
| b | Hide sibling panes | Pin the expanded window over the viewport work area | Re-specified by the user: fullscreen over all other views, not a pane-local mode. |
| c | Toggle in the pane-control strip | Immediately left of the close X (title bar when floating, tab bar when docked) | Re-specified by the user. Needs the two draw paths because a docked window has no title bar. |
| d | `ImGuiTabItemFlags_Trailing` tab-bar button (first cut) | Manual button at `BarRect.Max.x - FontSize` | `Trailing` orders last but only right-*aligns* once tabs overflow, so it rendered left-packed against the last tab. Pinned by a geometry assertion so it cannot regress. |
| e | `ICON_FA_EXPAND` / `ICON_FA_COMPRESS` | `ImDrawList` primitives | Font Awesome is not loaded in every build; those code points render as tofu without it. |
| f | Escape collapses | Dropped | Escape is already overloaded in the grid (selection clear); the control is one click away and the user did not ask for a key. |
| g | Bucket A "N/A" | Bucket A covers the transition core | Splitting the pure core into its own TU made the state machine unit-testable without a UI loop. |
| h | Bucket-E scenario `window_expand_collapse` | `tests/ui/window_expand.test.cpp`, 7 cases | Geometry (work-area rect, restore-to-node, right-edge alignment) is what only a live frame loop can prove, so the cases are geometric rather than visibility-based. |
| i | Bash-driver screenshot script | `scripts/dev/test-ui-window-expand.sh` (headless, JSON envelope) | Deterministic and human-free; a screenshot script would be manual residue. |
| j | Main grid pane excluded ("all other views than the main") | Main pane gets the button too | User revised the scope after seeing the first tab without a control. |

Also required by the implementation, not anticipated by the plan: the main pane's
`ImGuiWindowFlags_NoTitleBar` had to become conditional (item 6) — without it, expanding
the main pane undocks it into a title-bar-less window with nowhere to draw the minimize
face, wedging it fullscreen.

## Verification (actual)

- `bash scripts/dev/test-ui-window-expand.sh` → **passed=7 failed=0**.
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` →
  both targets link (dual-target).
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` → clean.
- `bash scripts/dev/test-docs.sh` → clean.
- `bash scripts/dev/test-all.sh` → **2793 passed / 48 failed**, NOT green. Every one of the
  48 was reproduced at identical counts on a clean `develop@84799bf6` worktree (same host,
  same toolchain): bucket-C goldens 8/7 with the same L∞ magnitudes (239/240/81),
  `test-mutation-smoke-bats.sh` 1/10, `test-shell-lint-bats.sh` 24/1,
  `test-workflow-yaml.sh` 29/2, `test-p4-mirror-bootstrap-bats.sh` 7/2,
  `test-pre-push-format-delta-bats.sh` 1/1. Pre-existing on develop, not introduced here —
  `git status --short -- agents/scripts/ tests/bats/` on this branch is empty. No golden is
  re-bootstrapped here; the failures overlap the in-flight deterministic-capture work
  (#1952 / #1962) and a re-bootstrap would need user approval under
  `docs/agent-rules/golden-image-approval.md`.

`TabBarToggleClickExpandsThenMinimizes` failed 6/7 deterministically on one machine and
passed on another. Not a product defect: the spawned exe resolves `%LOCALAPPDATA%\Smatchet`
(`ConfigManager::GetPlatformSharedUserDataDirectory`) and loaded whatever `imgui.ini` the
developer's own interactive session last wrote, so windows the test never opens sat over the
dock tab bar and `ItemClick` could not hover the docked control — *"Failed to move window
'Views - Jira###SmatchetViewsDashboard'! While trying to make space to click at
(1245.50,102.50)"*, `Unable to Hover … Hovered id was 0x00000000`. The geometry assertion
passed throughout; only the click could not land.

Proof it was the environment and not z-order: same binary, same host — real `LOCALAPPDATA`
→ 6/1, throwaway dir → **7/0**. `scripts/dev/test-ui-window-expand.sh` now boots the exe
against a `mktemp -d` user-data dir (`LOCALAPPDATA` / `APPDATA` / `XDG_CONFIG_HOME` exported
for the child only, `trap`-cleaned), so the layout under test is the app's shipped default.
The first hypothesis (z-order; park overlapping floaters around the click) was written,
built and re-run byte-identically red, and is reverted. Every other bucket-E / bucket-C
driver has the same exposure: P1 in
`docs/self-improvement/categories/test/2026-08-05-bucket-e-inherits-developer-imgui-ini.md`.

A second bucket-E red (4/7) came from `ToggleWindow`'s liveness guard, which required
`window->WasActive`. `NewFrame` copies `Active` into `WasActive`, so a window opened on the
frame the caller toggles it is `Active=1, WasActive=0` — the by-name command-surface path
rejected exactly the window the caller had just opened, while the click paths (which run a
frame later) passed. The guard now accepts `Active || WasActive`, keeping the stale-window
rejection that motivated it. Instrumentation receipt: `ToggleWindow 'Log' id=3546767630
act=1 wasAct=0`. Two runs were burned on a **stale exe** first — the source edit landed
after the build — so `scripts/dev/test-ui-window-expand.sh` now honours
`SMATCHET_UI_TEST_HOME` to pin (and keep) the user-data dir; without it the `mktemp` dir and
the app log inside it are deleted on exit and a red run is undebuggable.

One bucket-E assertion was dropped as unobservable rather than fixed: `BeginDocked`
rewrites `ImGuiWindowFlags_NoTitleBar` from the *node's* shape (set when the node has no
tab bar, cleared when it has one), so a post-restore `window->Flags` check tests ImGui, not
this code. The floating-frame check plus `PlacementRestored` (exact node identity) cover it.

## Post-ship follow-ups (PR #1966, after user manual-verify)

The user verified the running exe and found one gap; PR review bots found three more. All
four are fixed on the same PR because each is this feature's own logic applied
inconsistently — not adjacent pre-existing debt.

| Finding | Source | Mechanism | Fix |
|---|---|---|---|
| First bottom-panel tab (Scripts) had no expand icon | user, manual verify | `DrawToggle` bails on `window->SkipItems`, which is what `Begin` returning false leaves behind — including an **unselected dock tab**. Each node therefore queues exactly one button per frame, from the selected tab only; a window whose `Begin` never returns true contributes none. | queue per **node**, not per window |
| MAJOR: a real tab can lay out under the manual toggle | CodeRabbit `3730764590` | `TabBarLayout` clips the central section only by the trailing section's width (`imgui_widgets.cpp` — central width = `BarRect.W - sections[0].W - sections[2].W - sections[1].Spacing`; `ScrollingRectMaxX = BarRect.Max.x - sections[2].Width`). Nothing reserved the slot the toggle paints over, so on a full bar a tab owned the clicks under it. | `ReserveTabBarSlot` — a fully transparent trailing `TabItemButton` submitted through `DockNodeBeginAmendTabBar`, purely to shrink the central section. The visible control stays the manual right-aligned draw, because a trailing item right-*aligns* only on overflow (deviation *d* above). Takes effect the next frame: amend re-enters an already-laid-out bar. |
| Medium: `RepairMcpWindowLayout` re-docks an expanded window | Bugbot `3730763798` | An expanded window is deliberately undocked and pinned every frame by `BeginWindow`; without an expand test the repair reads "undocked" as "broken", arms the latch and force-writes pos/size against the pin. | same `IsCurrentWindowExpanded` guard `RepairLuaWindowLayout` already carried |
| Medium: assistant `s_assistantNeedsReDock` armed while expanded | Bugbot `3730763815` | Same shape — arming makes `ApplyAssistantDocking` issue `SetNextWindowDockID` competing with the fullscreen pin. | same guard |

**HIGH `3730763805` refuted, not fixed.** The claim was that `BeginWindow` /
`IsWindowExpanded` hash the full `"display###StableId"` string while ImGui window IDs hash
only the `###` suffix, so expand/restore/`SelfHeal` never line up for Annotate, Views and
grid panes. `ImHashStr` (`imgui.cpp:2496-2545`) **skips the `###` characters themselves** in
both its sized and zero-terminated branches — its own doc comment states *"label###id"
outputs the same hash as "id"* — and an ImGui window ID is `ImHashStr(name)`
(`imgui.cpp:4675`) with the same default seed 0 this code uses. All three named call sites
pass the identical string to `BeginWindow` and to `ImGui::Begin`, and `FindWindowByName` is
itself hash-based. The same mechanism retires a previously-backlogged Low about
source-vs-localized titles: `BuildLabelFromSource` emits `translated + "###" + source`
precisely so the ID survives localization.

Verification for these four: `cmake --build build/ninja-ui-test-msvc --target
SmatchetStandalone` → exit 0; `bash scripts/dev/test-ui-window-expand.sh` (ephemeral home)
→ **passed=9 failed=0 tested=9** (two cases added since the 7 above).

**That is a no-regression result, not coverage of the four fixes.** All 9 registered cases
predate them, and none puts a tab under the toggle slot or drives an expanded MCP/Assistant
window through its repair path — so the MAJOR-severity tab/toggle overlap fix in particular
ships green but untested. Both gaps are automatable and are backlogged for `test-author` at
[`categories/test/2026-08-07-window-expand-overlap-and-redock-guards.md`](../../self-improvement/categories/test/2026-08-07-window-expand-overlap-and-redock-guards.md).

**Spun out, not fixed here.** The user asked for an audit of the class *"restore can't
re-dock and falls back to a floating window at the old rect — looks docked, isn't; the
splitter drag exposes it."* Five findings landed; they are pre-existing dock-liveness bugs
independent of this feature, so they ship on `fix/dock-slot-liveness` behind a shared
`SmatchetDockNodeIds::EnsureDockSlotAlive(ImGuiID)` guard rather than widening this PR.
