# Plan — Rebindable keyboard shortcuts (unified, all shortcuts listed + changeable)

> **Slug**: `keyboard-shortcuts-rebindable` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Today every keyboard shortcut in Smatchet is **hardcoded** in scattered `ImGui::IsKeyPressed` / `MatchHotkey` blocks across `SmatchetUI.cpp`, `CommandPaletteUi.cpp`, and the Whisper prefs tab. None of them are listed in one place, and only **one** (the Whisper push-to-talk hotkey) is user-rebindable. The user asked for "all keyboard shortcuts working and able to be changed — like in the toolbar, add the ability to add new shortcuts to commands."

The toolbar already solves the structurally-identical problem for *buttons*: `ToolbarConfig` (a persisted list of `{CommandId, args, …}`) + `SmatchetToolbarUi` (a bind-command → edit-modal → persist editor) lets the user attach any registered command to a button. This plan mirrors that architecture for **keystrokes**: one persisted `KeybindingsConfig` (a list of `{Hotkey, CommandId, args, enabled}`), one dispatcher that drives it, and one editor (Preferences tab + right-click quick-bind) to change it.

**Intended outcome — after this lands**: every keyboard shortcut is an entry in one rebindable registry; the user can see the full list, change any binding, add new bindings to any registered command, and is warned on conflicts. (Decided scope via `AskUserQuestion`: migrate *everything*; editor = Preferences tab **and** right-click quick-bind; v1 = warn-on-conflict, single combo only, multi-key chords deferred.)

## Approach

Ship in **two PRs along a behavior-neutral seam**:

- **PR1 — Foundation + migration (behavior-neutral).** Add `KeybindingsConfig` (modeled byte-for-byte on `ToolbarConfig.h`, `json_fwd` only). Extend `ImGuiHotkey` with `Stringify` (the inverse of `ParseImGuiHotkey`), an optional super/win modifier, and a pure testable `FindShortcutConflict` helper. Fill the **command gaps** (sidebars / bottom-panel / dock-debug / fullscreen / bug-report) with new registered commands, and add an `action ∈ {toggle, show, hide}` param to the existing `view.toggle.*` group so a migrated *reveal* shortcut keeps its never-close + focus-latch semantics. Replace every scattered hardcoded `IsKeyPressed`/`MatchHotkey` block with one `DispatchKeybindings(app, d, io)` that walks `cfg.Keybindings`, matches, and dispatches through `CommandRegistry`. `KeybindingsConfig::Defaults()` reproduces the *exact* current set, so the only observable change is "nothing changed" — verifiable by parity. The Zen `Ctrl+M→Z` chord, `Esc-Esc` exit-Zen, and the palette's internal nav keys stay **special-cased and non-rebindable** (the documented "system" set; chords are out of v1 scope).

- **PR2 — Editor + surfacing (the visible UX).** A Preferences "Keyboard Shortcuts" tab (searchable table; add / clear / enable / rebind; live conflict-warn; a read-only "System (non-rebindable)" section). A shared **ImGuiKey** capture widget (the Whisper "Click to rebind" UX generalized — *not* its VK-based code; see nuance below). Right-click "Set shortcut…" on toolbar buttons and command-palette rows. Surfacing the bound combo on palette rows / menu items / toolbar tooltips. One-time back-compat fold of the legacy `BugReportHotkey` config field into `Keybindings`.

**Non-obvious trade-off that shaped the design**: shortcuts are bound to a command **id externally**, never as a field on the `Command` struct. Adding a `shortcut` field to `Command` would couple the registry to a UI concern and break "register once, surface everywhere" (CLI/MCP/Lua have no keystrokes). The toolbar already proves the external-binding pattern; keybindings reuse it.

## Files to modify

### PR1 — Foundation + migration

1. **`Source/Core/include/Config/KeybindingsConfig.h`** *(new)* — mirror [`ToolbarConfig.h`](Source/Core/include/Config/ToolbarConfig.h:15) (`<nlohmann/json_fwd.hpp>` only — header-cost discipline). `struct Keybinding { std::string CommandId; std::string Hotkey; std::string ArgsJson = "{}"; bool Enabled = true; friend to_json/from_json; }` + `struct KeybindingsConfig { std::vector<Keybinding> Bindings; static KeybindingsConfig Defaults(); }`.
2. **`Source/Core/src/Config/KeybindingsConfig.cpp`** *(new)* — `to_json`/`from_json` (mirror `ToolbarConfig.cpp`) + `Defaults()` reproducing every current binding byte-for-byte (table in § Existing utilities reused).
3. **[`Source/Core/include/Config/ConfigManager.h:220`](Source/Core/include/Config/ConfigManager.h:220)** — add `KeybindingsConfig Keybindings = KeybindingsConfig::Defaults();` next to `ToolbarConfig Toolbar`. Wire into the config `to_json`/`from_json` (same site that serializes `Toolbar`).
4. **[`Source/Core/include/Ui/ImGuiHotkey.h:17`](Source/Core/include/Ui/ImGuiHotkey.h:17)** + **`Source/Core/src/Ui/ImGuiHotkey.cpp`** — add `bool super = false;` to `ImGuiBugHotkey`; teach `ParseImGuiHotkey`/`MatchHotkey` the super modifier (harmless no-op on current bindings); add `std::string StringifyImGuiHotkey(const ImGuiBugHotkey&)` (inverse of parse); add pure `int FindShortcutConflict(const std::vector<ImGuiBugHotkey>&, const ImGuiBugHotkey& candidate)` (returns index or -1; no ImGui-IO dependency, unit-testable). Keep exact-modifier equality in `MatchHotkey` (already disambiguates Ctrl+B vs Ctrl+Alt+B — verified at [`ImGuiHotkey.cpp` MatchHotkey]).
5. **[`Source/Core/src/Commands/ViewToggleCommands.cpp:28`](Source/Core/src/Commands/ViewToggleCommands.cpp:28)** — add `action` param (`toggle` default / `show` / `hide`) to the handler built by `RegisterToggle`. **`show` must raise the focus latch** (`request*Focus = true`) to match the current reveal-shortcut behavior — so `RegisterToggle` gains a `bool UiDrawSession::* focusLatch` parameter and `show` sets `*flag = true; *focusLatch = true` (plus existing `onOpen`). `toggle` keeps today's behavior (default → zero change for existing callers).
6. **[`Source/Core/src/Commands/ViewCommands.cpp`] / a new `Source/Core/src/Commands/AppViewCommands.cpp`** — register the gap commands (all reachable from every front-end per the registry contract):
   - `view.sidebar.primary` / `view.sidebar.secondary` / `view.panel.bottom` — each takes `action` and flips **persisted** view-slot state (`SetViewVisible(cfg, ViewSlot::*) + ConfigManager::Save`), NOT a session bool (see Nuance 1).
   - `view.assistant` (AI-gated `SMATCHET_WITH_AI`) — reveals the assistant side panel (`d.assistantPanelOpen`/`d.requestAssistantFocus`), absent from the `view.toggle.*` set.
   - `app.fullscreen.toggle` → `d.requestFullScreenToggle = true`.
   - `app.dock_debug.toggle` → `d.showDockDebug = !d.showDockDebug`.
   - `app.bug_report.open` → raise the bug-report modal (replaces the inline `BugReportHotkey` poll).
7. **[`Source/Core/src/Ui/SmatchetUI.cpp:515`](Source/Core/src/Ui/SmatchetUI.cpp:515)** (`drawPreWindowOverlays`) — add `dispatchKeybindings(app, d)`: parse-cache the `cfg.Keybindings` hotkeys (re-parse only when the config revision changes — Pillar 1), `MatchHotkey`, dispatch via `app.Commands().Dispatch(id, args, ctx)` (or a small `ui.*` pseudo handler for palette-open). Then **delete** the hardcoded blocks:
   - the `BugReportHotkey` poll at [`:537`](Source/Core/src/Ui/SmatchetUI.cpp:537),
   - dock-debug `Ctrl+Alt+D` at [`:693`](Source/Core/src/Ui/SmatchetUI.cpp:693), fullscreen `F11` at [`:704`](Source/Core/src/Ui/SmatchetUI.cpp:704) (in `drawChromeAndModeToggles`),
   - `handlePanelVisibilityShortcuts` at [`:765`](Source/Core/src/Ui/SmatchetUI.cpp:765) (Ctrl+B / Ctrl+Alt+B / Ctrl+J),
   - `handleViewRevealShortcuts` at [`:787`](Source/Core/src/Ui/SmatchetUI.cpp:787) (Ctrl+Shift+{A,F,D,I,X,K,L} / Ctrl+,).
   - **Keep special-cased (NOT migrated)**: the Zen `Ctrl+M→Z` chord + `Esc-Esc` exit-Zen in `drawChromeAndModeToggles` (multi-key chord, out of v1 scope).
8. **[`Source/Core/src/Commands/CommandPaletteUi.cpp:204`](Source/Core/src/Commands/CommandPaletteUi.cpp:204)** — route the `Ctrl+Shift+P` open through `dispatchKeybindings` (a `ui.command_palette` pseudo); **keep** the palette's internal nav keys (arrows / enter / esc) hardcoded (modal-local, non-rebindable system set).
9. **`tests/Core/ImGuiHotkey.test.cpp`** *(extend or new)* + **`tests/Core/KeybindingsConfig.test.cpp`** *(new)* — see § Verification Bucket A.

### PR2 — Editor + surfacing

10. **`Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp`** *(new)* — the "Keyboard Shortcuts" tab; searchable table; rebind via the shared capture widget; live conflict-warn via `FindShortcutConflict`; read-only "System (non-rebindable)" list (Zen chord etc.); persist via `EnqueueTrackerConfig`. Registered into the prefs tab bar beside the Whisper tab.
11. **`Source/Core/src/Ui/SmatchetHotkeyCapture.{h,cpp}`** *(new)* — shared **ImGuiKey** capture widget ("Click to rebind", Esc cancels, rejects modifier-only). Generalizes the Whisper UX at [`SmatchetPreferencesUi_Whisper.cpp:274`](Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp:274) — but captures ImGuiKey, not VK (Nuance 3).
12. **[`Source/Core/src/Ui/SmatchetToolbarUi.cpp`]** — add "Set shortcut…" to the per-button right-click menu (reuses the capture widget; writes a `Keybinding` for that button's `CommandId`).
13. **[`Source/Core/src/Commands/CommandPaletteUi.cpp`]** — per-row "Set shortcut…" context action; render the bound combo (Stringify) on each row.
14. **`Source/Core/src/Ui/…` menu/toolbar tooltip sites** — surface the bound combo on menu items + toolbar tooltips.
15. **[`Source/Core/include/Config/ConfigManager.h:108`](Source/Core/include/Config/ConfigManager.h:108)** + load path — one-time migration: if `BugReportHotkey`/`BugReportHotkeyEnabled` differ from defaults, fold into `Keybindings` (`app.bug_report.open`), then those fields read through.
16. **`Locales/*.json`** — tab title, column headers, conflict-warning, capture-widget strings.
17. **`docs/guides/` user doc** — the rebindable-shortcuts feature page.

## Existing utilities reused

- `ParseImGuiHotkey` / `MatchHotkey` — [`Source/Core/include/Ui/ImGuiHotkey.h:28`](Source/Core/include/Ui/ImGuiHotkey.h:28) — parse a spec string + exact-modifier match; the dispatcher's core.
- `ToolbarConfig` / `ToolbarButton` + `to_json`/`from_json` — [`Source/Core/include/Config/ToolbarConfig.h:25`](Source/Core/include/Config/ToolbarConfig.h:25) — the structural template for `KeybindingsConfig` (json_fwd-only header, default factory).
- `RegisterToggle` / `ToggleFlag` — [`Source/Core/src/Commands/ViewToggleCommands.cpp:28`](Source/Core/src/Commands/ViewToggleCommands.cpp:28) — extended in-place with the `action` + focus-latch params; the `view.toggle.*` group already covers performance / plan-doc / bulk-import / bulk-export / preferences / mcp / scripts.
- `SetViewVisible` + `ConfigManager::Save` — [`Source/Core/src/Ui/SmatchetUI.cpp:770`](Source/Core/src/Ui/SmatchetUI.cpp:770) — the persisted view-slot toggle the new sidebar/panel commands call.
- Whisper capture UX (`CaptureHotkeyVkThisFrame` / `CommitCapturedHotkey` / `DrawWhisperHotkey`) — [`Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp:185`](Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp:185) — the **UX precedent** (rejects modifier-only, "Click to rebind", Esc cancels) generalized in the new capture widget. Its VK-capture code is *not* lifted (Nuance 3).
- `EnqueueTrackerConfig` (`smatchet::config_save`) — the off-thread persist path the toolbar editor uses; the keybinding editor reuses it (Pillar 2).
- `app.Commands().Dispatch` — [`Source/Core/include/Commands/CommandRegistry.h`] — the single dispatch surface the migrated shortcuts route through.

**`Defaults()` binding table (PR1 parity target — every current shortcut):**

| Hotkey | Command (post-migration) | Source today |
|---|---|---|
| `Ctrl+B` | `view.sidebar.primary` (`action:toggle`) | `handlePanelVisibilityShortcuts` |
| `Ctrl+Alt+B` | `view.sidebar.secondary` (`toggle`) | `handlePanelVisibilityShortcuts` |
| `Ctrl+J` | `view.panel.bottom` (`toggle`) | `handlePanelVisibilityShortcuts` |
| `Ctrl+Shift+A` | `view.assistant` (`show`) *(AI-gated)* | `handleViewRevealShortcuts` |
| `Ctrl+Shift+F` | `view.toggle.performance` (`action:show`) | `handleViewRevealShortcuts` |
| `Ctrl+Shift+D` | `view.toggle.plan_doc_viewer` (`show`) | `handleViewRevealShortcuts` |
| `Ctrl+Shift+I` | `view.toggle.bulk_import` (`show`) | `handleViewRevealShortcuts` |
| `Ctrl+Shift+X` | `view.toggle.bulk_export` (`show`) | `handleViewRevealShortcuts` |
| `Ctrl+Shift+K` | `view.toggle.mcp_server` (`show`) *(MCP-gated)* | `handleViewRevealShortcuts` |
| `Ctrl+Shift+L` | `view.toggle.scripts` (`show`) *(Lua-gated)* | `handleViewRevealShortcuts` |
| `Ctrl+,` | `view.toggle.preferences` (`show`) | `handleViewRevealShortcuts` |
| `Ctrl+Alt+D` | `app.dock_debug.toggle` | `drawChromeAndModeToggles:693` |
| `F11` | `app.fullscreen.toggle` | `drawChromeAndModeToggles:704` |
| `Ctrl+Shift+P` | `ui.command_palette` (pseudo) | `CommandPaletteUi.cpp:204` |
| `Ctrl+Shift+B` | `app.bug_report.open` | `BugReportHotkey` config (migrated PR2) |

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: the dispatcher walks a small (~15-entry) vector once per frame, matching pre-parsed `ImGuiBugHotkey` structs cached against a config revision counter — no per-frame string parsing. Strictly cheaper than today's N separate `IsKeyPressed` calls. No new perf scope needed (covered by the existing `drawPreWindowOverlays` scope).
- **Pillar 2 (UI-thread never blocks > 100 ms)**: dispatch is an in-memory map lookup; the editor persists via `EnqueueTrackerConfig` (already off-thread). No new sync I/O on the render path.
- **Pillar 3 (never crash)**: a malformed user-entered hotkey string fails `ParseImGuiHotkey` → that binding is skipped (logged `LOG_WARN`), never throws; an unknown `CommandId` returns the error envelope from `Dispatch`. RAII throughout; config structs are value types.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: net **positive** — making every shortcut discoverable + rebindable is itself an accessibility win (users can move bindings off hard-to-reach combos). The editor table follows existing prefs styling (font-scale-aware). No new contrast surfaces.

## Perf-review-system gates (diff touches `Source/Core/` — MANDATORY)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`.

1. **PR-fast CI** — scenario most directly exercising the changed path: the **startup / first-frame** + any **command-dispatch** scenario (the dispatcher runs every frame in `drawPreWindowOverlays`). Confirm the curated diff→scenario map in `agents/core/perf-gatekeeper.md` maps `Source/Core/src/Ui/SmatchetUI.cpp` to a steady-state UI scenario; if the touched path isn't in `scripts/dev/perf-pr-fast-set.json`, add it.
2. **Pillar 2 static scanner** — **N/A**: no new sync I/O (`cpr`/`SQLite`/`p4`/file) reachable from `ImGui::*`; the editor's only write is the existing off-thread `EnqueueTrackerConfig`.
3. **Dispatcher drain** — **N/A**: does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — **N/A**: adds no new sync-stall path > 100 ms.
5. **Marker inventory** — **N/A**: no new `SMATCHET_UI_PERF_SCOPE` markers (reuses `drawPreWindowOverlays`).

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the steady-state UI scenario before opening PR1.

**Override**: none expected; `perf-out-of-band` only if a baseline-bump PR is queued.

## Risks / non-goals

- **Risk — parity regression during migration.** A migrated binding that silently changes semantics (e.g. a *reveal* becoming a *toggle*, or losing the focus latch) is the top risk. **Mitigated** by: (a) the `action:show` + focus-latch design (Nuance 2); (b) a `Defaults()`-parity unit test asserting every legacy spec is present and parses; (c) PR1 being behavior-neutral by construction — reviewable as "does every old shortcut still fire identically?"
- **Risk — sidebar/panel commands wrong target.** Panel-visibility shortcuts flip *persisted* `cfg` view-slot state, not session bools (Nuance 1). **Mitigated**: new commands call `SetViewVisible(cfg, …) + ConfigManager::Save`, matching the deleted block exactly.
- **Risk — Whisper VK vs in-app ImGuiKey confusion.** The Whisper hotkey is an OS-global `RegisterHotKey` (needs Win32 VK); in-app bindings are ImGui-frame keys. **Mitigated**: the shared capture widget captures ImGuiKey; Whisper's VK path is untouched (Nuance 3). The two stay separate systems.
- **Non-goal — multi-key chords.** The Zen `Ctrl+M→Z` chord stays special-cased and non-rebindable (v1 decision). A future plan adds chord support to `KeybindingsConfig`.
- **Non-goal — palette internal nav + Esc-Esc exit-Zen.** Modal-local keys stay hardcoded; not user-rebindable.
- **Non-goal — per-tracker keybindings.** Unlike the toolbar's per-tracker append, v1 keybindings are global only.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps.

- **Bucket A (pure-logic ctest, `test-rig`)**:
  - `StringifyImGuiHotkey` ↔ `ParseImGuiHotkey` roundtrip over the full default set + edge cases (modifier-only, unknown key, super modifier).
  - `FindShortcutConflict` — returns the colliding index for an exact match, -1 otherwise; verifies exact-modifier discrimination (Ctrl+B vs Ctrl+Alt+B do **not** collide).
  - `KeybindingsConfig::Defaults()` parity — every legacy hotkey spec from the § Defaults table is present, parses, and names a registered (or gated) command id.
  - `KeybindingsConfig` json roundtrip (`to_json`/`from_json` stable).
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: PR2 — open the Keyboard Shortcuts tab, rebind a command, assert the new combo fires the command and the conflict warning shows on a colliding bind. PR1 — a smoke test that a representative migrated shortcut (e.g. `Ctrl+,` → preferences) still opens its window.
- **Bash-driver scenario / screenshot / sanitizer**: sanitizer build clean (Pillar 3); a CLI scenario dispatching `view.sidebar.primary {action:"show"}` then `{action:"hide"}` asserts the persisted flag flips (proves the command path independent of the keystroke).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — `KeybindingsConfig.h` must compile under DX12, hence json_fwd-only).
- **Doc validation (blocks plan-doc PRs)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script).
- **Plan stress-test — `grill-with-docs`**: run before finalising; stress-test against the command-system + config-persistence domain model; record outcome below.
- **Manual residue**: visual confirmation that a rebound shortcut focuses the right docked tab is the one inherently-visual step → covered by the Bucket-E focus assertion where the Test Engine can read window-focus state; if any residue remains, add a `docs/self-improvement/categories/tooling.md` entry naming the deferred automation. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here (multi-key chords, per-tracker keybindings) and revise or delete them.

- **Multi-key chord support** — follow-up plan; v1 is single-combo only.
- **Per-tracker / per-view keybindings** — follow-up; v1 is global only.
- **Mouse-button / gamepad bindings** — no-action; out of the keyboard-shortcut scope the user asked for.
- **Importing/exporting a keybinding profile** — no-action for v1; the config already round-trips, so a future export is cheap.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*In the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
