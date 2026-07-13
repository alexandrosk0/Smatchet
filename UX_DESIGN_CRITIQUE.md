# Smatchet UX & Design Critique

**Date:** 2026-07-05
**Branch:** `claude/app-design-ux-critique-0zh838`
**Scope:** First-party desktop UI (`Source/Core/src/Ui/**`) of the standalone Dear ImGui client — application shell, issue grid + saved views, issue editing/creation, preferences/onboarding, feedback/notification systems, and power-user tools (AI assistant, command palette, Perforce annotate). Mobile shell and Unreal-embedded specifics are noted only where they contrast with the desktop app.
**Method:** Static reading of the UI source (the app was not run). Every finding cites `file:line` and quotes the actual UI string where one exists. This is a **design/usability critique**, complementary to `CPP_CODE_AUDIT.md` (correctness) and `SECURITY_AUDIT.md` — it does not re-report defects covered there.

> **How to read severity.** High = a real user is likely to be blocked, lose work, or be confused on a common path. Med = friction or inconsistency that erodes trust/efficiency but has a workaround. Low = polish; worth doing but not urgent.

---

## 1. What the app does well

Before the critique, the parts that are genuinely strong — these are the bar the rest of the app should be levelled up to:

- **One command registry drives four frontends.** A single `RegisterCommand({...})` surfaces in the CLI, the Command Palette (`Ctrl+Shift+P`), MCP `tools/call`, and Lua. Menu items dispatch through the same registry (`SmatchetUI_MainMenu.cpp:101` `DispatchMenuCommand`), so a keybinding and its menu item share one code path and can't drift.
- **Live shortcut hints track user rebinds.** Menu items show the *current* bound key, not a hardcoded literal — `MenuShortcut` reads the keybinding registry per frame (`SmatchetUI_MainMenu.cpp:73`). Rebind a key and every menu updates. This is better than most native apps manage.
- **First-launch is gated behind a working connection.** Until the backend probe succeeds, everything except Preferences is disabled and Preferences is force-opened (`SmatchetUI_MainMenu.cpp:127-130`, `trackerLocked`). The user can't get lost in an app that can't load data yet.
- **Notifications are never silently lost.** Every toast is also written to a bounded session history (`SmatchetToast.cpp:26-33`), so a message that faded can still be found in the Notification Center.
- **Layout edits are gated behind an explicit commit.** Column reorder/resize/sort raises an *Unsaved layout changes* strip with `Save` / `Save as new…` / `Discard` rather than silently autosaving — a deliberate, correct choice for shared saved views.
- **Actionable error copy in the AI panel.** `"No active AI provider — open Preferences and pick a configured provider, then press Send again."` (`SmatchetAiAssistantUi.cpp:1017`) names the cause *and* the fix. This is the model the rest of the app should copy.

---

## 2. High-severity findings

### H1 — The desktop grid has no empty, loading, or zero-results state
**Severity: High** · Evidence: `SmatchetActiveProjectGridUi.cpp` (no "no issues"/"loading" string anywhere in the desktop grid; grep for empty/loading/syncing returns **zero** matches in this file and in `SmatchetGridPaneWindows.cpp`). Contrast the *mobile* shell, which does it right: `mobile.grid.no_tickets` → "No tickets loaded.", `mobile.grid.select_ticket` → "Select a ticket to see its details." (`SmatchetMobileShellUi.cpp:410-415`).

The primary workspace — the issue grid — renders an empty table when there are no rows, whether that's because a sync is in flight, the query matched nothing, or the backend errored. The three states are indistinguishable to the user: *"Is it loading? Is my filter wrong? Is it broken?"* This is the single most-visited surface in the app and it has the weakest feedback.

**Fix:** Add three explicit grid body states drawn in place of the (empty) table: (1) **loading** — a spinner + "Loading issues…" driven by the existing `d.initialTicketSyncLoading` / `d.fieldCatalogLoading` / `d.connectivityRecoveryTicketFetchLoading` flags that already exist (`SmatchetUI.cpp:626-631`); (2) **zero results** — "No issues match this view." with a *Clear filter* / *Edit view* button; (3) **error** — the connectivity reason from the status bar plus a *Retry* button. The mobile strings already exist to reuse.

### H2 — "Recently Used Views" menu shows raw internal command IDs to the user
**Severity: High** · Evidence: `SmatchetUI_MainMenu.cpp:531` — `if (ImGui::MenuItem(cmdId.c_str()))` renders the command id verbatim, so the menu literally reads:

```
Recently Used Views
  view.toggle.views-dashboard
  view.toggle.backend-audit
  view.toggle.performance
```

These are developer identifiers, not labels. A user who opens the menu to re-open something they just used is shown dotted, kebab-cased internal strings instead of "Views Dashboard", "Backend Audit", "Performance". It reads like debug output leaked into the shipping UI.

**Fix:** Resolve each `cmdId` to its registered command's display label before drawing (the registry already stores a human `Summary` — `SmatchetUI.cpp:894` reads `c->Summary`). Fall back to a title-cased slug only if no command is found. Same map should back the `recentViews_.Touch(...)` ids, which are themselves inconsistent (some use dots, some dashes — see M3).

### H3 — Changing panel position or swapping the sidebar destroys the user's custom layout
**Severity: High** · Evidence: `SmatchetUI_MainMenu.cpp:392-404` (Panel Position → Bottom/Right) and `:431-435` (Move Primary Side Bar Left/Right) both call `resetWindowLayoutToDefault(d)`. The code comments admit it: *"Dock rebuild is complex and fragile; reset layout instead."*

A user who has spent time arranging panes, resizing the grid, and floating the assistant will silently lose **all of it** the moment they move the panel from Bottom to Right — an action they'd reasonably expect to move *one* panel. There is no warning and no undo. This punishes exactly the power users the docking system is built for.

**Fix:** Two options, in order of preference: (a) implement the targeted `DockBuilder` re-dock so only the affected node moves (the real fix); (b) if that stays deferred, gate the action behind a confirm — "Changing panel position will reset your window layout. Continue?" — with a *Don't ask again* option, and offer *Reset Layout* as the honest label. Silent destruction of user-arranged state is the worst of the three.

### H4 — No first-run orientation: the user lands in a fully-greyed-out app
**Severity: High** · Evidence: `SmatchetUI_MainMenu.cpp:127-158` — on first launch `trackerLocked` is true, so File/Edit/Selection/View/Help are all wrapped in `BeginDisabled()` and Preferences is force-shown. There is no welcome panel, no "Connect your tracker to get started" copy, and no setup wizard (grep for `onboard`/`welcome`/`first-run`/`setup wizard` finds only the Whisper dictation banner at `SmatchetUI.cpp:722`, nothing for the core tracker flow).

A brand-new user sees a mostly-disabled menu bar, an empty grid (see H1), and the Preferences window open on the **Tracker** tab (`SmatchetPreferencesUi.cpp:511`) with a backend dropdown and credential fields — but no guidance on *which* backend, *where* to get an API token, or *what happens next*. The gating logic is good; the orientation on top of it is missing.

**Fix:** When `!BackendHasBeenReachable`, render a lightweight welcome/empty-state in the main content area: one line of what Smatchet is, a "Choose a backend" step (Jira / Plane / GitHub with a one-line each), and a deep link to each provider's token page. Keep the existing gate — just give the locked state a face. This is the highest-leverage onboarding win.

### H5 — Notification toasts are hardcoded dark and ignore the theme
**Severity: High (visual correctness)** · Evidence: `SmatchetToast.cpp:97-98,117` — the toast background is a literal `ImVec4(0.12f, 0.12f, 0.14f, 0.95f)` and the title text is a literal white `ImVec4(1,1,1,alpha)`. The app ships **light** themes ("VS 2022 Light", and "High Contrast") — `SmatchetUI_MainMenu.cpp:336-338`.

On a light or high-contrast theme, every toast is a dark charcoal box with white text floating over a light UI — off-brand at best, and on High Contrast an accessibility regression (the theme exists precisely to guarantee contrast, and the toast opts out of it). Only the accent bar reads from the theme (`StatusDone`/`StatusBlocked` etc., `:102-105`); the body does not.

**Fix:** Source the toast background/border/text from the active theme's `ImGuiCol_PopupBg` / `ImGuiCol_Border` / `ImGuiCol_Text` (or dedicated `SmatchetTheme::Colors` entries) instead of literals, so a light theme yields a light toast. Keep the semantic accent bar as-is — that part is right.

---

## 3. Medium-severity findings

### M1 — Toasts can't be dismissed; clicking one navigates away instead
**Severity: Med** · Evidence: `SmatchetToast.cpp:88-91` — clicking anywhere on a toast sets `m_openCenterRequested` (open the Notification Center); there is no close affordance. Auto-dismiss is purely timer-driven (`:48-50`, `Expiry`).

Two problems: (1) there's no `×` to dismiss a toast you've read, so a stack of them (they stack upward, `:126`) just sits there occluding the bottom-right of the grid until each timer fires; (2) the *entire* toast is a click target that yanks you to another window — a user reaching to swat a notification away instead gets teleported into the Notification Center. Click-to-dismiss is the near-universal convention; click-to-navigate-elsewhere is surprising.

**Fix:** Add a hover-revealed `×` that removes just that toast. Make the body click dismiss (the convention); move "open history" to a small explicit link/affordance or keep it only for error toasts. Also consider that **error** toasts probably shouldn't auto-expire at all — see M2.

### M2 — Error toasts auto-expire, so a failure can vanish before it's read
**Severity: Med** · Evidence: `SmatchetToast.cpp:15-24` — `durationMs` is a caller-supplied timer applied uniformly; `ToastType::Error` (`:103`) gets no special retention. The only escape hatch is that history is retained (`:26-33`), but the *discoverability* of "your error is in the Notification Center" is nil.

A backend save that fails throws an Error toast that fades on the same schedule as a "Copied to clipboard" success. If the user glanced away, the error is gone and they have no signal that anything went wrong — the grid just didn't update.

**Fix:** Make `ToastType::Error` sticky (no auto-expire; require manual dismiss), and/or badge the Notification Center entry in the menu bar / status bar with an unread-error count so there's a persistent trail. Success/info can keep their short timers.

### M3 — The View menu is a flat list of 13 window toggles; keyboard chords are near-unmemorable
**Severity: Med** · Evidence: `SmatchetUI_MainMenu.cpp:554-647` — Views Dashboard, Annotate, Log, Notifications, Backend Audit, Performance, Plan docs, Bulk Import, Bulk Export, Preferences, MCP Server, Assistant, Scripts & Actions are one undifferentiated list. Their defaults pile onto one modifier: `Ctrl+Shift+` **E, N, U, M, F, D, I, X, K, A, L** plus `P`/`V` on the palette. That's ~13 chords sharing a prefix, and the mnemonics fight the user: Annotate = `Ctrl+Shift+N`, Log = `Ctrl+Shift+U`, Backend Audit = `Ctrl+Shift+M`, Performance = `Ctrl+Shift+F`. Nobody will retain "U = Log".

**Fix:** (1) Group the View menu with separators/submenus by intent — *Workspace* (Views Dashboard, Annotate), *Diagnostics* (Log, Backend Audit, Performance, Plan docs), *Data* (Bulk Import/Export), *System* (Notifications, MCP, Assistant, Preferences). Some grouping exists via `Separator()` but it's arbitrary. (2) Lean on the command palette as the real discovery surface and stop trying to assign a default chord to every window — reserve chords for the 3–4 genuinely high-frequency windows (Palette, Assistant, Preferences, Views Dashboard) and let the rest be palette-only. Fewer, better-mnemonic defaults beat 13 arbitrary ones.

### M4 — Preferences has ~13 tabs and no search-within-settings
**Severity: Med** · Evidence: tab items across the Preferences TU family: **Tracker, User Info, Integrations, Local data, Appearance, Whisper, Keybindings, Assistant, Grid, Time Estimates, Work Log Templates, Fields Inputs (with its own sub-tab bar), Annotate** (`SmatchetPreferencesUi.cpp:512,533,543`; `_Local.cpp:255,596`; `_Whisper.cpp:865`; `_Keybindings.cpp:181`; `_Assistant.cpp:746`; `_Templates.cpp:32,55,156,447,449,466`). That's a two-row tab bar with nested sub-tabs.

Finding a setting means knowing which of 13 tabs owns it — is "compact rows" under *Appearance*, *Grid*, or *Fields Inputs*? Is the API token under *Tracker* or *Integrations*? There's no search box and the grouping isn't self-evident (User Info and Time Estimates and Work Log Templates are all separate top-level tabs).

**Fix:** Add a filter box at the top of the Preferences window that live-matches setting labels across all tabs (ImGui makes this cheap — the labels are static strings). Consolidate the long tail: fold *Time Estimates* + *Work Log Templates* + *Fields Inputs* under a single *Issue Defaults* tab, and *Whisper* + *Assistant* + *Integrations* under *AI & Integrations*. Target ~6 top-level tabs.

### M5 — The status bar surfaces developer telemetry (FPS) to end users by default
**Severity: Med** · Evidence: `SmatchetStatusBarUi.cpp:110-127` — the right side always reads e.g. `12pt  Modern Dark  60 fps`. FPS is a debugging metric; an issue-tracking user has no use for it and it makes the product feel like a dev tool. It also crowds out space that could show something they *do* care about (e.g., last-synced time, active project).

**Fix:** Drop FPS from the default status bar (keep it behind the existing Performance window / a debug toggle). Replace the reclaimed space with a *last synced* timestamp or the active project name. Font-size and theme name are also low-value in a persistent bar — consider moving them into an on-hover tooltip and using the space for connectivity detail.

### M6 — Backend identity in the status bar is the raw lowercase type string
**Severity: Med** · Evidence: `SmatchetStatusBarUi.cpp:72-75` — `backend` is `d.cfg.TrackerType` shown verbatim (truncated to 64 chars), so the chip reads `jira` / `plane` / `github` in lowercase, not "Jira" / "Plane" / "GitHub". Minor, but it's the most persistent label in the app and it looks unpolished. The `"?"` fallback when empty (`:74`) is also cryptic.

**Fix:** Map the type to a display name (title-cased, with the same friendly names used elsewhere), and replace `"?"` with "No backend" or "Not connected". Consider a small backend icon for instant recognition.

### M7 — Two menu-bar palettes for one feature; the inline box's behavior is subtle
**Severity: Med** · Evidence: there's an inline "Search commands (Ctrl+Shift+P)" InputText embedded in the menu bar (`SmatchetUI_MainMenu.cpp:709-740`) **and** a full Command Palette modal, **and** an omnibar for grid search (`SmatchetOmnibarUi.cpp`). The inline box opens/pre-fills the modal on any edit (`:730-734`) and self-clears when it loses focus (`:736-738`). Three overlapping search entry points with non-obvious relationships is a discoverability tax — a user won't know whether to type in the menu-bar box, hit `Ctrl+Shift+P`, or use the omnibar, or how they differ (commands vs. issues).

**Fix:** Clarify the division of labor in the UI: the menu-bar box and palette should feel like one thing (they nearly are) — consider dropping the always-present inline box in favor of a single palette entry, or clearly label the omnibar as issue-search vs. command-search. At minimum, distinct placeholder copy that tells the user what each searches.

### M8 — "Notifications" menu item ships with no default shortcut while everything around it has one
**Severity: Med (consistency)** · Evidence: `SmatchetUI_MainMenu.cpp:580` — `MenuShortcut(ctx, "notifications", "")` passes an **empty** fallback, so the item shows no hint while its neighbors (Log `Ctrl+Shift+U`, Backend Audit `Ctrl+Shift+M`, Performance `Ctrl+Shift+F`) all do. The command id `"notifications"` also breaks the `view.toggle.*` naming pattern its siblings follow.

**Fix:** Either assign a default (if it's worth a chord) or, per M3, deliberately leave it palette-only — but then do the same for its low-frequency siblings so the presence/absence of a chord communicates frequency, rather than looking like an oversight. Rename the command id to `view.toggle.notifications` for consistency.

---

## 4. Low-severity / polish

- **L1 — Inconsistent recent-view ids (dots vs. dashes).** `recentViews_.Touch(...)` is called with `"view.toggle.views-dashboard"` (dash, `:567`) but also `"view.toggle.plan_doc_viewer"` (underscore, `:602`) and `"notifications"` (bare, `:584`). Because these strings are shown to users (H2), the inconsistency is user-visible. Normalize the id scheme and resolve to labels.
- **L2 — Zen Mode uses a two-key chord (`Ctrl+M, Z`) while its neighbors use single chords** (`SmatchetUI_MainMenu.cpp:421`). Chord vs. non-chord within one menu is inconsistent; pick one convention for toggles.
- **L3 — "ImGui Default Dark (bright)" leaks the framework name into a user-facing theme list** (`SmatchetUI_MainMenu.cpp:333`). Users don't know what "ImGui" is. Rename to something descriptive ("Classic Bright" or similar).
- **L4 — Six hardcoded font names, no "system default" or custom-path option** (`SmatchetUI_MainMenu.cpp:368-370`: Segoe UI, Consolas, Calibri, Arial, Cascadia Code, JetBrains Mono). On a machine missing these, the choice silently does nothing useful. Offer a system-default entry and surface whether the selected font actually loaded.
- **L5 — `Ctrl+O` ("Open Project View…") shadows the near-universal "Open File" mental model** (`SmatchetUI_MainMenu.cpp:269-271`). Not wrong, but a first-time user pressing `Ctrl+O` expecting a file dialog gets the Views Dashboard. Consider whether the muscle-memory collision is worth it.
- **L6 — "queued" / "auth error" / "unavailable" status strings are terse to the point of ambiguity** (`SmatchetStatusBarUi.cpp:46-54,95`). "3 queued" of what? Add a tooltip on the status-bar segments explaining queued offline ops and what each connectivity state means / how to recover.
- **L7 — The in-flight edit indicator is a bare `*`** (`SmatchetStatusBarUi.cpp:106`). A lone asterisk is not self-explanatory as "saving…". Use a small spinner or the word "Saving…" with the existing amber color.

---

## 5. Cross-cutting themes

Three patterns recur across the findings and are worth fixing at the root rather than case-by-case:

1. **Internal vocabulary leaks into the UI.** Command ids in menus (H2, L1), the raw tracker type (M6), "ImGui" in a theme name (L3), FPS in the status bar (M5). There should be one seam that maps every internal identifier to a display label, and nothing raw should reach a widget. The command registry's `Summary` field is that seam for commands — use it everywhere.

2. **Empty/loading/error states are an afterthought on desktop but done well on mobile.** The mobile shell has "No tickets loaded." / "Select a ticket…" (`SmatchetMobileShellUi.cpp:410-415`); the desktop grid — the flagship surface — has nothing (H1, H4). Porting the mobile state discipline to desktop is the biggest perceived-quality win available.

3. **Destructive/irreversible UI actions lack guards.** Panel-position and sidebar swaps nuke the layout with no confirm (H3). The app is otherwise careful about data (offline queue, unsaved-layout strip, retained toast history) — the layout-reset behavior is the outlier and should be brought up to the same standard.

---

## 6. Suggested prioritization

| Priority | Findings | Rationale |
|---|---|---|
| **Do first** | H1 (grid states), H2 (command-id leak), H4 (first-run orientation) | Highest ratio of user-visible quality to effort; all three are on the most-trafficked first-session path. |
| **Do next** | H3 (layout-destroy guard), H5 (theme-aware toasts), M1/M2 (toast dismiss + sticky errors) | Trust and data-integrity; H3 and M2 both risk the user losing something (layout, an error they needed to see). |
| **Then** | M3/M4 (menu + settings IA), M5/M6 (status-bar polish), M7 (search entry points) | Structural IA cleanup — higher effort, compounding payoff. |
| **Polish pass** | M8, L1–L7 | Consistency sweep; cheap individually, do them together. |

---

*This critique was produced by static reading of the UI source. Findings marked with a specific `file:line` were verified against the tree at the branch head; the "run the app and watch it" verification (e.g. confirming the empty grid truly renders blank, or that a light-theme toast is unreadable) was not performed and is the recommended next step before acting on the High findings.*

---

## 7. Implementation status (2026-07-13)

| Finding | Status | Where |
|---|---|---|
| H1 grid states | **Done** — welcome/loading/error states replace the empty table; zero-results/filter-no-match render a strip above it (headers + inline new-issue row stay usable) | `SmatchetActiveProjectGridTable.cpp` |
| H2 command-id leak | **Done** — Recently Used Views resolves labels via the new `Command::Title` registry seam (title-cased-slug fallback) | `SmatchetUI_MainMenu.cpp`, `Command.h` |
| H3 layout-destroy guard | **Done (option b)** — confirm modal with persisted *Don't ask again* (`skip_layout_reset_confirm`); the targeted DockBuilder re-dock remains deferred | `SmatchetUI_MainMenu.cpp` |
| H4 first-run orientation | **Done** — welcome panel in the grid body while `!BackendHasBeenReachable`: product one-liner, per-backend credential hints + token deep-links, Open Preferences | `SmatchetActiveProjectGridTable.cpp` |
| H5 theme-aware toasts | **Done** — toast bg/border/text from `ImGuiCol_PopupBg/Border/Text`; semantic accent bar kept | `SmatchetToast.cpp` |
| M1 toast dismiss | **Done** — click dismisses (error click also opens the center); hover-revealed close cross | `SmatchetToast.cpp` |
| M2 sticky errors | **Done** — error toasts never auto-expire; unread-error badge in the status bar opens Notifications and clears on view | `SmatchetToast.*`, `SmatchetStatusBarUi.cpp`, `SmatchetNotificationCenterUi.cpp` |
| M3 View-menu IA | **Done** — toggles grouped Workspace / Diagnostics / Data / System via `SeparatorText` | `SmatchetUI_MainMenu.cpp` |
| M4 settings search | **Done** — search box + per-tab keyword index; match chips jump to the owning tab (`PrefsTabFlags`) | `SmatchetPreferencesUi*.cpp` |
| M5 FPS in status bar | **Done** — FPS dropped (lives in the Performance window); right side keeps font/theme with an explanatory tooltip | `SmatchetStatusBarUi.cpp` |
| M6 raw backend string | **Done** — friendly display names (Jira/Plane/GitHub/Linear), "Not connected" fallback, tooltip | `SmatchetStatusBarUi.cpp` |
| M7 search entry points | **Done** — omnibar placeholder says issue-search; menu-bar box says commands-not-issues | `SmatchetOmnibarUi.cpp`, `SmatchetUI_MainMenu.cpp` |
| M8 notifications chord/id | **Done** — renamed to `view.toggle.notifications` (bare `notifications` kept as alias), default `Ctrl+Shift+Y` seeded via `migrated_menu_shortcuts_v2` | `ViewToggleCommands.cpp`, `KeybindingsConfig.cpp`, `ConfigManager_Load.cpp` |
| L1 recent-view ids | **Done** — Touch ids normalized to registered command ids; sidebar/panel/assistant gained `view.toggle.*` aliases so Recents can re-dispatch them | `SmatchetUI_MainMenu.cpp`, `AppViewCommands.cpp` |
| L2 Zen chord | Deferred — single-item consistency change; revisit with a keybinding-defaults pass | — |
| L3 "ImGui" in theme list | **Done** — display name "Classic Bright" (persisted config string unchanged) | `SmatchetUI_MainMenu.cpp`, `SmatchetStatusBarUi.cpp` |
| L4 font list | **Done (partial)** — "Built-in Default" entry + "(not installed)" annotation via `SmatchetIsFontAvailable`; custom-path picker deferred | `SmatchetUI_MainMenu.cpp`, `SmatchetImGuiFonts.*` |
| L5 Ctrl+O collision | Deferred — muscle-memory tradeoff judged acceptable; rebindable via Keyboard Shortcuts | — |
| L6 terse status strings | **Done** — tooltips on backend / connectivity / queued segments explain state + recovery | `SmatchetStatusBarUi.cpp` |
| L7 bare `*` indicator | **Done** — amber "Saving..." with tooltip | `SmatchetStatusBarUi.cpp` |
