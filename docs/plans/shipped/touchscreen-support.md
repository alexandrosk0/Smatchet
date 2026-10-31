# Plan — Touchscreen support (standalone + Unreal)
<!-- plan-date: 2026-10-31 -->

> **Slug**: `touchscreen-support` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why).

## Context

Smatchet is mouse-first. On Windows touch laptops / tablets (standalone GLFW build) and in the Unreal DX12 overlay a finger today:

- range-selects in the grid instead of scrolling — there is no pan and no inertia anywhere;
- cannot open any of the ~21 context menus (all right-click only);
- cannot reach the ~250 hover-only tooltips / help markers;
- hits tiny targets (14 px scrollbars, `TouchExtraPadding` 0);
- in Unreal, behaves exactly like a mouse: Slate delivers touch as `LeftMouseButton` pointer events (with `IsTouchEvent()` set), and the host setters write `io.MousePos` / `io.MouseDown` directly, so ImGui never learns the input source. *(Corrected during review: the original draft said touch was dropped via `EKeys::Touch1`; Slate's touch `FPointerEvent` constructors set `EffectingButton = LeftMouseButton`, UE 5.7 `Events.h`.)*

The standalone build already reports `io.MouseSource == ImGuiMouseSource_TouchScreen` (the GLFW backend subclasses the WndProc and calls `AddMouseSourceEvent` from `GetMessageExtraInfo()`), but nothing consumes it. Existing touch code (long-press-to-edit in `TouchCellEditGesture.h`, Mobile touch density, the IME bridge) is Android-only.

User request (2026-10-15 session): "make the app work with touchscreens better." Decisions: target standalone **and** Unreal; behaviours = long-press → right-click, finger pan-scroll with inertia, tap shows tooltips, bigger hit targets; activation Auto + Preferences override (Auto / On / Off); no on-screen-keyboard work.

After this lands: a finger can scroll, open context menus, read tooltips and hit controls comfortably in both hosts, while mouse behaviour is unchanged.

## Approach

One **gesture router per ImGui context** (`TouchInputRouter`). Mouse input bypasses it. Touch raw events are fed to it by each host; it **holds back** the press until it can classify it as tap / long-press / pan / widget-drag, then emits real ImGui events. It is driven from an ImGui `NewFramePre` context hook, which runs before `UpdateInputEvents` while last frame's `HoveredId` / `HoveredWindow` / `MouseCursor` still describe what is under the finger. No edits to the ~21 right-click sites or ~250 tooltip sites.

Interception happens at the event source, not by rewriting `g.InputEventsQueue`: `ImGuiIO::AddMouseButtonEvent` drops an event equal to the latest known state, so erasing a held-back down makes the backend's later up vanish.

Gesture classification lives in a pure, ImGui-free state machine (`TouchGesturePure.h`) driven by ImGui frame time, so it is deterministic under the test engine. Hit-target sizing is a separate pure detector + style overlay (`TouchSizingPure.h`) applied through the existing single style-rebuild point.

### Gesture table

| Gesture | Result |
|---|---|
| Tap (moves < 10 px) | Left click; pointer stays parked at the tap point so tooltips appear ~0.3 s later |
| Hold 0.5 s (configurable 0.3–1.5 s) within slop | Right click (down + up) → context menu; left never sent. Progress ring from 0.15 s |
| Second tap within 0.30 s / 24 px | Snapped to first tap and forwarded immediately → real ImGui double-click (cell editor) |
| Drag past slop over scrollable content | Scrolls the nearest scrollable window per axis (axis lock ratio 2.0); pointer hidden during pan |
| Release while moving (≥ 120 px/s) | Fling: `disp = v·τ·(1−e^(−dt/τ))`, τ = 0.325 s, max 6000 px/s, stop < 15 px/s or at scroll limit or on any mouse / key / wheel; a tap during a fling only stops it |
| Drag on scrollbar, splitter / resize cursor, text, title bar, menu bar, dock tab bar, marked drag surface, or a non-scrollable window | Forwarded as a normal left-drag (late start) |
| Touch outside an open popup / modal | Blocked (no pan); a tap still closes the popup normally |

### Bigger hit targets

`TouchSizingDetector` switches on at the first touch button event; off only after ≥ 2 s without touch **and** a mouse click / wheel or ≥ 48 px of mouse motion within 1 s. Applied only while the router is idle (no layout shift under the finger). `ComputeTouchTargetStyle` (max-semantics, idempotent): `TouchExtraPadding = clamp(floor(ItemSpacing × 0.5), 0, 6 s)` (no hitbox overlap), `ScrollbarSize ≥ 20 s`, `GrabMinSize ≥ 20 s`, `FramePadding.y ≥ round(cur × 1.5)`, `WindowBorderHoverPadding ≥ 8 s`, `TabCloseButtonMinWidthUnselected = -1`. Font size untouched.

### Long-press vs Android long-press-to-edit

Desktop: long-press = right-click (Windows convention); edit = double-tap (already handled by `ShouldOpenCellEditorOnGesture` via `IsMouseDoubleClicked(0)`). Android is unchanged — its host never installs the router. Only the 0.5 s constant is shared.

## Files to modify

**PR 1 — Unreal input (event queue + touch source)**

1. `Source/Core/src/Ui/SmatchetImGuiHost.cpp:786-824` — `SetMousePosition` / `SetMouseButton` / `AddMouseWheel` move to `AddMousePosEvent` / `AddMouseButtonEvent` / `AddMouseWheelEvent`; new `SetMouseSource(int)` wrapping `AddMouseSourceEvent`; C wrapper `SmatchetHost_SetMouseSource` beside `:1271-1290`.
2. `Source/Core/include/Ui/SmatchetImGuiHost.h:69-71` + `Source/Core/include/Ui/SmatchetImGuiHostC.h:97-99` — declare `SetMouseSource` / `SmatchetHost_SetMouseSource` (additive ABI).
3. `Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet/include/SmatchetImGuiHost.h` + `SmatchetImGuiHostC.h` — keep the shipped plugin header copies identical.
4. `Source/UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/Private/SmatchetImGuiInputProcessor.cpp:294-436` — report source (`IsTouchEvent()`) before position/button calls; map `EKeys::Touch1` → 0; ignore pointer index ≠ 0; one-shot `UE_LOG` on first touch.
5. `Source/UnrealPlugins/SmatchetImGuiPlugin/Source/SmatchetImGuiPlugin/Private/SmatchetImGuiRenderBackend_WinDx12.cpp:146` — no software cursor while the source is touch.

**PR 2 — Gestures + sizing**

6. `Source/Core/include/TouchGesturePure.h` (new) — state machine, thresholds, velocity tracker, fling, action pacing (ImGui-free, C++14).
7. `Source/Core/include/Ui/TouchSizingPure.h` (new) — sizing detector + style-overlay math (ImGui-free).
8. `Source/Core/include/Ui/TouchInputRouter.h`, `Source/Core/src/Ui/TouchInputRouter.cpp` (new) — context hooks, press classification, pan application by window ID, emission, pointer parking, progress ring; per-context registry freed by a `Shutdown` hook.
9. `Source/Core/include/TouchCellEditGesture.h:41` — share the long-press constant.
10. `Source/Standalone/StandaloneTouchBridge.h` + `.cpp` (new) — GLFW cursor / button / enter callbacks chained to `ImGui_ImplGlfw_*Callback`; key bridge install moves here; Windows `MicrosoftTabletPenServiceProperty` press-and-hold toggle.
11. `Source/Standalone/main.cpp`, `Source/Standalone/StandaloneAppBootstrap.cpp` — install router after `CreateContext`; replace the four duplicated `glfwSetKeyCallback(window, KeypadEnterBridgeCallback)` calls with `InstallStandaloneInputBridges(window)`.
12. `Source/Core/src/Ui/SmatchetImGuiHost.cpp` — install router; route touch-source events to it.
13. `Source/Core/include/SmatchetUiModeIds.h` — `enum class TouchInputMode { Auto, On, Off }` + string conversion.
14. `Source/Core/include/Config/ConfigManager.h`, `Source/Core/src/Config/ConfigManager.cpp` (`kFloatFields`), `ConfigManager_Save.cpp`, `ConfigManager_Load.cpp` — `TouchInput` (`touchInput`) + `TouchLongPressSeconds` (`touch_long_press_seconds`).
15. `Source/Core/src/Ui/PreferencesSchema.cpp`, `Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp`, `Source/Core/src/SmatchetLocalization.cpp` — prefs rows, Touch section, en + fr strings.
16. `Source/Core/src/Ui/SmatchetTheme.cpp` + `Source/Core/include/Ui/SmatchetTheme.h` — `ApplyTouchTargetOverlay(hostScale)`.
17. `Source/Core/src/Ui/SmatchetUI.cpp:500-553` + `Source/Core/include/Ui/SmatchetUI.h` — feed router settings each frame; `touchTargetsChanged` style-rebuild trigger; `lastAppliedTouchTargets_`.
18. Drag-surface marks (one line each): grid `TableHeader` (`Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp`), views drag handle (`Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp`), toolbar-editor row (`Source/Core/src/Ui/SmatchetToolbarUi.cpp`).
19. `Source/Core/src/Commands/Scenarios/UiTestScenario.cpp` — move router hooks after the test engine's hook.
20. Tests: `tests/Core/TouchGesturePure.test.cpp`, `tests/Core/TouchSizingPure.test.cpp`, `tests/ui/touch_gestures.test.cpp`; `tests/CMakeLists.txt` (both lists), `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`; config / schema test updates.

## Existing utilities reused

- `GetMouseSourceFromMessageExtraInfo` + WndProc hook (`imgui_impl_glfw.cpp`) — standalone source detection, read back through `GImGui->InputEventsNextMouseSource`.
- `KeypadEnterBridgeCallback` (`Source/Standalone/StandaloneBoot_detail.cpp:85`) — the callback-chaining pattern the touch bridge follows.
- `smatchet::ui::RouteWheelToScrollableTooltipBeforeNewFrame` (`Source/Core/src/Ui/SmatchetTooltipWheelRouter.cpp`) — precedent for calling `SetScrollY` on a window outside the frame.
- `SmatchetTheme::HostDensityScale` / `ApplyTouchScale` / `ReapplyHostDensityScale` (`Source/Core/src/Ui/SmatchetTheme.cpp`) — scale inputs for thresholds and overlay.
- `SmatchetUI::drawApplyAppearanceSettings` (`Source/Core/src/Ui/SmatchetUI.cpp:500`) — the single style-rebuild point.
- `ShouldOpenCellEditorOnGesture` (`Source/Core/include/TouchCellEditGesture.h:50`) — double-click path used by double-tap.
- `ImGuiTestRunFlags_EnableRawInputs` + the geometry-capture pattern of `tests/ui/drag_checkbox_paint.test.cpp` — bucket-E driving.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no code is extracted; new TUs are additive.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: mouse users pay one `NewFramePre` hook doing an O(queued events) scan; touch pan calls `SetScrollX/Y` once per frame. Measured by the PR-fast scenario below.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no I/O; all work is per-frame arithmetic.
- **Pillar 3 (never crash)**: pan targets re-resolved by `ImGuiID` every tick; router access null-checked (Android / CLI have none); router lifetime owned by the context `Shutdown` hook.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: improves reach for touch users; keyboard nav and font scaling untouched (the overlay never changes font size).

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires: `priority-grid-scroll` (grid scrolling is the path the hook and the pan sit on).
2. **Pillar 2 static scanner** — N/A: no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new sync-stall path.
5. **Marker inventory** — N/A: no new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `priority-grid-scroll` before opening PR 2.

## Risks / non-goals

- **Unmarked drag sources inside scrollable windows pan instead of drag** — mitigated by drag-surface marks on the three known sources and the cursor / scrollbar / title-bar rules; a comment beside `MarkLastItemAsTouchDragSurface` tells future authors.
- **Drag-to-paint checkboxes are not marked** — accepted: marking them makes the Fields list un-pannable; a tap still toggles.
- **Windows press-and-hold right-click double-fires** — mitigated by setting `MicrosoftTabletPenServiceProperty` while gestures are on; restored on Off.
- **Unreal: Slate may not hand touch to `IInputProcessor` with `IsTouchEvent()`** — if it arrives as promoted mouse, behaviour degrades to today's mouse path. Manual residue (on-device).
- **Unreal mouse path moves to the event queue** — fast down/up pairs within one frame are no longer lost (improvement); isolated in PR 1 so it bisects cleanly.
- **Hook ordering vs the test engine** — router hooks moved after the engine's; asserted by the bucket-E suite.
- **Tooltip may flash before a long-press menu** (0.3 s vs 0.5 s) — accepted.
- Non-goals: on-screen keyboard; pinch-zoom / multi-touch; Android behaviour; overscroll bounce; touch range-select in the grid; panning inside scrollable tooltips.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `TouchGesturePure.test.cpp` — tap pacing; pan emits no buttons; drag-through emits `LeftDown@P` first; blocked emits nothing; long-press fires at threshold, not after movement, release swallowed; double-tap snap; fling min / max / decay / stop / absorbed tap; velocity 0 after pause; duplicate downs ignored; `Cancel` emits `LeftUp`; axis lock. `TouchSizingPure.test.cpp` — detector hysteresis, overlay idempotence and no-overlap. `ConfigManager.test.cpp` enum + float round-trip and unknown fallback; `UiModeIds.test.cpp` strings; `PreferencesSchema.test.cpp` + `tests/ui/prefs_schema_coverage.test.cpp`.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: `tests/ui/touch_gestures.test.cpp` — tap selects once; long-press opens popup without left activation; drag scrolls ≈ finger delta with zero activations; fling continues then stops; splitter drag-through moves the splitter; tap-park shows tooltip; double-tap → `IsMouseDoubleClicked`; **mouse left-drag does not pan** (regression guard); `TouchExtraPadding > 0` after touch and reset after mouse use past 2 s.
- **Bash-driver scenario / screenshot / sanitizer**: `priority-grid-scroll` PR-fast perf scenario.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run before finalising; outcome recorded in § Verification (actual).
- **Manual residue**: (1) real Windows touchscreen on the standalone build — source latch, press-and-hold property, gesture feel; (2) Unreal on a touchscreen — Slate touch delivery to `IInputProcessor`. Deferred-automation entry goes into `docs/self-improvement/categories/tooling.md` (synthetic-pointer-injection driver for the standalone bridge).

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- Windows on-screen keyboard (TabTip / InputPane) — excluded by the user; follow-up plan if requested.
- Pinch-zoom and multi-finger gestures — follow-up plan.
- Touch range-select in the grid — no action (mouse feature).
- Panning inside scrollable tooltips — no action.

## Implementation log

Branch `claude/touchscreen-support-e7d3ad` (shipped as one PR; SHAs are pre-squash):

- `af18311db` · wip(plan): this plan.
- `6fe5778c9` · Unreal host mouse setters move to the ImGui event queue; additive C ABI `SmatchetHost_SetMouseSource`; input processor reports `IsTouchEvent()`; no software cursor while touching.
- `25877115b` · Review fixes: dead `Touch1` branch removed; host measures the real wall delta between `BeginFrame` calls (the plugin passes a fixed 1/60 s); no position event in front of each wheel notch.
- `68595683d` · `TouchGesturePure.h` (tap / hold / double-tap / pan / fling / drag-through machine) and `Ui/TouchSizingPure.h` (sizing detector + style overlay), with unit tests.
- `466efe1ae` · `TouchInput` (Auto / On / Off) and `TouchLongPressSeconds` config, Preferences rows, en/fr strings, config tests.
- `90c331a7d` · `TouchInputRouter` + hooks, standalone `StandaloneTouchBridge`, Unreal host routing, `SmatchetTheme::ApplyTouchTargetOverlay` through the `drawApplyAppearanceSettings` rebuild gate, drag-surface marks, `UiTestScenario` hook reorder.
- `459b24e4b` · Bucket-E suite `tests/ui/touch_gestures.test.cpp`; double-tap now requires the same item; router events flagged `AddedByTestEngine`.
- `5bce111d3` · This plan's post-ship sections + archive; tooling entry for the on-device residue.
- `3b6aa3127` · Branch-review fixes: Windows press-and-hold property no longer set at startup, removed at shutdown; lost-release recovery (repeated press or app focus loss cancels the stuck gesture); every event after a press waits a tick (fast-tap hover fix); touch settings and sizing gated on the router (Android untouched); stronger bucket-E guards; `Active` instead of `WasActive`; localization entry formatted.

## Deviations from plan

- **One PR instead of two.** Review showed PR 1's premise was wrong (Unreal touch already arrived as a left click), so on its own it only tagged the source; its value is enabling the router, and its test coverage lives with PR 2. The plan-lock is keyed to one branch.
- **No `Touch1` mapping in Unreal.** Slate builds touch `FPointerEvent`s with `EffectingButton = LeftMouseButton`; the branch could never run.
- **Unreal frame clock fixed (added).** Both Unreal backends passed a constant 1/60 s, so ImGui time (double-click, long-press) followed the presentation rate. `SmatchetImGuiHost::BeginFrame` now uses the measured wall delta (1 ms..250 ms clamp).
- **No action pacing in the pure layer.** ImGui's trickle queue (on by default) already spreads a down+up over two frames and delays a touch press until its position is seen; the router relies on it instead.
- **Router flags its queued events `AddedByTestEngine`.** Under the test engine, trickled leftovers (a tap's release) were erased between frames and left the button held; ImGui core ignores the field.
- **Double-tap needs the same item (added).** The bucket-E suite showed a quick tap on the *neighbouring* row (within 0.3 s / 24 px) snapping onto the first row as a double-click. The machine's glue gained `SameTarget`, decided from ImGui's hovered item one frame after the second press.
- **Foreign touch position events are dropped.** While gestures are on, touchscreen-sourced position events the router did not queue (GLFW backend cursor polling) are removed so they cannot drag the held-back pointer.
- **`FramePadding.y` touch minimum is an absolute 6 px × scale** (not ×1.5 of the current value) so the overlay stays idempotent.
- **Press-and-hold property follows the device (review).** `MicrosoftTabletPenServiceProperty` also governs the pen, which the router does not handle. It is set by touchscreen input, cleared by pen input (a pen hovers before contact, so a pen hold gets Windows' right click again), and removed before the window is destroyed (`UninstallStandaloneInputBridges`). The very first finger hold of a session may still get the OS behaviour, because the property lands mid-contact; that is part of the on-device residue.
- **Lost-release recovery (review).** A second press while the router still thinks a finger is down, or an app focus loss, cancels the stuck gesture (releasing anything it pressed).
- **Touch settings are desktop-only.** The Preferences block and the sizing overlay require an installed router, so Android (no router) keeps its own Mobile-mode handling.
- **Not done from review (low):** DPI-scaling the gesture thresholds on high-DPI desktops (`HostDensityScale` is 1.0 there), and feeding `io.MouseDoubleClickTime` into the double-tap window. Both are tuning, left for on-device feedback.
- **Prefs rows are `ConditionalDraw`**: they draw only where a router is installed (desktop); `prefs_schema_coverage` still sees both ids on desktop.
- **Not done from review (low, rare):** a focus loss and a new finger press landing in the same frame drop that one touch; the boot-failure teardown paths do not call `UninstallStandaloneInputBridges` (only matters if a touch arrived during a failed boot).

## Verification (actual)

- **Bucket A**: `SmatchetTests.exe` full run 4001/4001 passed (53321 assertions) on the final head, including 29 touch gesture / sizing cases and the config + `touchInputMode` tests.
- **Bucket E**: `ui_test.run --name=TouchGestures` 9/9 on two consecutive runs (isolated `APPDATA`). Full `--all`: 167/171 locally; the 4 failures (`TrackerFirstRun/VerifiedPinClearsOnWindowClose`, `NotificationCenter/OpenViaToastClickRequest_RendersRowsAndClears`, `FuncSizeWindowRender/AuditWindow_RendersAndShowsRefreshButton`, `FuncSizeMainUi/BulkImportWindow_RendersAndShowsLoadFile`) also fail individually on an unmodified develop build (`3964fd78c`) on the same machine, and run before any touch test; develop CI is 162/162, so they are local-environment failures, not regressions.
- **Code review**: two adversarial `code-review` passes (Unreal commit; whole branch). Blocking findings fixed (pen press-and-hold regression, a clang-format slip); medium findings fixed (lost release, fast-tap hover, vacuous test guards).
- **Build gate**: `SmatchetStandalone` (ninja-iter-msvc), `SmatchetImGuiHost_DX12` + `SmatchetCore_DX12` (Unreal host libs), `SmatchetTests` (ninja-test-msvc), `SmatchetStandalone` (ninja-ui-test-msvc) — all built clean under strict warnings. The UE plugin module itself was not compiled here (needs the engine build).
- **clang-format**: `--dry-run --Werror` clean on every touched C++ file.
- **Perf gate (`priority-grid-scroll`)**: not run locally; left to the PR-fast CI lane.
- **Plan stress-test (`grill-with-docs`)**: not run — the skill is not available in this harness session.
- **Manual residue**: real Windows touchscreen (standalone) and Unreal touchscreen. Tracked in `docs/self-improvement/categories/tooling/2026-10-31-touch-gestures-need-on-device-pointer-injection.md` (pointer-injection driver + UE Automation spec).
