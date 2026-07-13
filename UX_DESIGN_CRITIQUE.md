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

---

## 8. Second pass — 2026-07-13 (post-implementation)

**Scope:** the tree at `develop` after PR #1819 merged the pass-1 fixes. Same method as pass 1 — static reading, every finding cites `file:line` and quotes the actual UI string — executed as four parallel area reviews (grid workspace; Views + Preferences; secondary windows; chrome + a cross-cutting consistency sweep), each finding then re-verified against the tree before inclusion. Pass-1 items (§2–§4) are not re-reported; several findings below are honest critique of the pass-1 additions themselves. Severity rubric unchanged (§ How to read severity).

### 8.1 High-severity findings

- **P2-H1 — Omnibar Enter silently and durably overwrites the saved view's query.** `SmatchetOmnibarUi.cpp:195-216` — a JQL-classified Enter runs `updated.Jql = query; ViewState.UpdateActive(updated)`, and `Views::UpdateActive` calls `Save()` immediately (`Views.cpp:75`): the view's query of record is rewritten **on disk** with no dirty flag, no Save/Discard strip, no undo. The only disclosure is a hover tooltip on the tiny mode glyph ("Filter query — Enter replaces the focused view's query."), while the pass-1 placeholder ("Search issues: key, filter query, or title text") actively invites throwaway searches. The app's flagship pattern gates column/sort edits behind the *Unsaved layout changes* strip — yet a search box commits a far more consequential edit instantly. **Fix:** route omnibar JQL through the same dirty/Save-Discard mechanism (set `viewsDirty` + snapshot instead of `UpdateActive`), or apply it session-only with a "query changed — Save to view / Revert" strip.
- **P2-H2 — Re-running "Run import" after a partial failure duplicates every already-created issue.** `SmatchetBulkTicketsUi.cpp:430` — the button wipes all terminal row states (`bulkImportStatus.assign(rows.size(), "queued")`, including rows already marked `ok KEY`) and resubmits everything. After a run with a few failures, the natural retry is the button just used — and every already-succeeded create is created **again** in the tracker. There is no "retry failed only". **Fix:** preserve terminal-success rows on re-run and requeue only failed/parse-error rows, or add an explicit *Retry failed rows* button.
- **P2-H3 — Tracker credential edits are silently discarded when Preferences closes.** `SmatchetPreferencesUi.cpp:144,177,193` — the Tracker tab edits stack buffers only; nothing reaches `cfg` until *Save & Sync*, and closing the window resets `preferencesBuffersLoaded` so reopening reloads from cfg. The Assistant tab earns a `*` dirty marker and an "Unsaved changes" hint; the Tracker tab — the one first-run force-opens, on which a new user types an API token — has neither, and 7 of 10 sibling tabs autosave, teaching exactly the wrong habit. **Fix:** track buffer-vs-cfg dirtiness (the `AssistantAiFieldsDiffer` pattern), show a `*` on the tab, and guard close with Save/Discard.
- **P2-H4 — An in-progress new-issue draft is destroyed with no confirmation.** `SmatchetGridUiSupport.cpp:507-522` — `CancelUnfinishedNewIssueForGridChange` wipes the draft (up to 64 KB of description + staged attachments) on any view/query change; the call site's comment admits "no confirmation in the grid path" (`SmatchetActiveProjectGridUi.cpp:412-413`). The Cancel button (`SmatchetNewIssueDraftUi.cpp:365-374`) is equally instant, and because of P2-H1 an omnibar Enter mid-draft also deletes it — silently, not even a toast. **Fix:** confirm when the draft has non-empty text/attachments, or auto-queue the abandoned draft offline so it is recoverable.
- **P2-H5 — Esc anywhere in the focused Annotate window destroys the pasted callstack and the completed analysis.** `AnnotateAnalysisUi_Window.cpp:1127-1130` → `CloseAnnotateModal` memsets the callstack buffer and clears all result rows (`AnnotateAnalysisUi_Modals.cpp:208-215`). Paste a crash callstack, run a long multi-frame `p4 annotate`, reflexively tap Esc to leave the text field — everything is gone, no confirm, no undo. **Fix:** Esc closes only when no work exists; hide without wiping otherwise, keeping the destructive reset on the explicit Close.
- **P2-H6 — Toast clicks and hovers fall through to the UI underneath.** `SmatchetToast.cpp:76-117` — toasts paint on the foreground draw list and hit-test with `IsMouseHoveringRect` + `IsMouseClicked`; nothing captures the input, so every dismissal click *also* lands on whatever sits under the bottom-right of the grid (row selection, buttons, scrollbars), and hover keeps tooltipping widgets through the toast. Pass 1 made error toasts sticky — the user now **must** click them, making the fall-through a routine hazard rather than a corner case. **Fix:** render each toast as a `NoTitleBar/NoDocking` window or lay an `InvisibleButton` over the rect so ImGui routes and consumes the input.
- **P2-H7 — A failed attachment-preview download leaves the card on "Loading..." forever.** `SmatchetAttachmentPreviewUi.cpp:505-512` — on failure the worker only logs and returns; no update reaches the entry, so the card renders "Loading..." (`:821`) indefinitely with no error and no retry, though the download path produces a real error string (`AttachmentAppUpdateService.cpp:436-438`). Expired token, offline spell, and slow load are indistinguishable. **Fix:** post the failure back through the existing `attachmentPreviewUpdateQueue` (the "preview failed" render path already exists) and clear `PreviewRequestIssued` so re-selection retries.
- **P2-H8 — "Open selected" downloads the attachment synchronously on the UI thread, then silently changes behavior on failure.** `SmatchetAttachmentPreviewUi.cpp:984` — the button runs `DownloadAttachmentToLocalFile` inline (the preview path documents a 120 s worst case for this class), freezing the app with zero progress cue; on failure it quietly opens a browser tab instead (`AttachmentAppUpdateService.cpp:395-398`) — the user asked for a local open and gets neither an explanation nor the thing they asked for. **Fix:** move the download to a worker with a "Downloading `<name>`..." cue; surface the fallback as a toast ("Download failed: `<err>` — opened in browser instead").
- **P2-H9 — Semantic status colors are hardcoded dark-theme literals that fail on "VS 2022 Light" — including pass 1's own additions.** `SmatchetStatusBarUi.cpp:160` ("Saving..." amber ≈ 1.4:1 contrast on a white bar — effectively invisible) and `:178` (the new "%d errors" badge ≈ 3.3:1, below AA); same family on user-facing text across the app: `SmatchetNewIssueDraftUi.cpp:619` "(required)" ~2:1, `SmatchetAiAssistantUi.cpp:843,1425`, `SmatchetBulkTicketsUi.cpp:404,463-465`, `SmatchetGridHeaderUi.cpp:498`. H5 fixed the toast *body* but left the semantic-color class behind — and error/warning text is precisely what must survive a theme switch. **Fix:** add per-theme semantic slots (ErrorText / WarningText / SuccessText / AccentText) in `SmatchetTheme::ApplyStyle` (the AI/syntax palettes already model this) and sweep the literals onto them.

### 8.2 Medium-severity findings

- **P2-M1 — Offline-queue Discard hard-deletes queued work in one click.** `SmatchetOfflineQueueUi.cpp:729,740,922` — "Discard selected" / "Clear archived dead rows" delete DB rows immediately; queued creates/edits are the *only* copy of offline-authored work, and Discard sits beside "Copy selected"/"Retry creates now". **Fix:** confirm for pending (non-dead) rows, or two-step mark-then-Undo-toast.
- **P2-M2 — Quick comment templates post to the tracker straight from a context-menu click — including a fill-in-the-blanks skeleton posted with the blanks.** `SmatchetGridUiSupport.cpp:235-270`; the built-in body is literally "Triage handoff for {key}:\n- Current owner: \n- Next action: ..." (`:100-101`) — published verbatim, team-visible, no preview/edit/undo. **Fix:** open the comments modal pre-filled instead of posting directly — at minimum for templates containing empty `:`-terminated blanks.
- **P2-M3 — Comments modal discards a typed comment on Escape/Close with no guard.** `SmatchetCommentsModalUi.cpp:264-267` — Escape fires even while the post box is active and `CloseCommentsModal()` zeroes `PostBuf`; minutes of writing lost to the most reflexive key in the app. **Fix:** first Escape defocuses; gate Close behind "Discard comment?" when the buffer is non-empty (or persist the draft per issue).
- **P2-M4 — Keybinding capture commits bare, modifier-less keys with no warning, and dispatch has no text-input gate.** `SmatchetHotkeyCapture.cpp:47-59`, `ImGuiHotkey.cpp:179-187`, `SmatchetUI.cpp:865-869` — bind plain "K" and every "k" typed into a comment fires the command; pressing a combo *during* capture also executes its old binding that same frame. Whisper's capture already requires a modifier ("Hotkey must include a modifier key (Ctrl, Alt, Shift, or Win)", `_Whisper.cpp:254-255`). **Fix:** warn/reject modifier-less combos, skip `dispatchKeybindings` while a capture is armed, and gate bare-key dispatch on `!io.WantTextInput`.
- **P2-M5 — "Reset all to defaults" nukes every custom keybinding instantly on an autosaving tab.** `SmatchetPreferencesUi_Keybindings.cpp:282-285` — one click, debounced save makes it permanent within ~100 ms; the app confirms view deletes and DB recreation but not this. **Fix:** confirm ("Replace all N shortcuts with defaults?") or a one-shot "Undo reset" snapshot.
- **P2-M6 — The new layout-reset guard has holes: "Don't ask again" persists even on Cancel, has no re-arm UI, and "Reset Layout" itself is still unguarded.** `SmatchetUI_MainMenu.cpp:518-520` (checkbox saves immediately, before the Cancel decision; no Preferences surface exposes `skip_layout_reset_confirm`) and `:675-677` (the *most* destructive menu item fires with no confirm, one row under Recently Used Views, while lesser panel moves now warn). **Fix:** commit the checkbox only on confirm; route Reset Layout through the same `requestLayoutResetAction` modal (a fourth action value); expose a "restore confirmation dialogs" toggle.
- **P2-M7 — The Command Palette still leads with raw command ids, violating the registry's own `Title` contract.** `CommandPaletteUi.cpp:337` (`rowLabel = c.Name + "  " + c.Summary`) — and "Open View..." funnels novices into it pre-filled with the literal `view.toggle.` (`SmatchetUI_MainMenu.cpp:617-619`). H2 stopped one raw-id leak; the advertised discovery surface still shows `view.toggle.log  Toggle the Log window`. Related polish: palette args render CLI-style (`--project *:`, `:189`) and the destructive hint names only the keyboard path ("hold Shift+Enter to confirm", `:288`) while an unmodified mouse click is silently inert (`:340-341`). **Fix:** render `Title` (title-cased-slug fallback) as the primary label with the id dimmed; pre-fill by category filter; drop the `--`; word the hint "hold Shift and click, or press Shift+Enter".
- **P2-M8 — A burst of sticky error toasts demands one click each; `DismissAllLive()` exists but nothing calls it, and reading the Notification Center doesn't clear the on-screen stack.** `SmatchetToast.h:41-43` (zero UI callers), `SmatchetNotificationCenterUi.cpp:51-55` (opening clears only the badge; "Clear all" clears history, not live toasts) — after reading the errors in the center, the same errors still occlude the grid. **Fix:** `DismissAllLive()` when the center opens (the trail is safely in view), and/or a "Dismiss all" affordance when 2+ toasts are live.
- **P2-M9 — The pass-1 grid loading state is gated on session-level *initial*-sync flags, so per-pane fetches fall through to the wrong state.** `SmatchetActiveProjectGridTable.cpp:488-501` — `syncLoading` reads only session-scoped flags; every pane runs its own sync (`EnsurePaneLiveSyncStarted`) and `app.IsPaneSyncLive(pane.id)` exists but is never consulted, so a pane mid-first-fetch renders "No issues match this view." (and a null-snapshot unfocused pane briefly *borrows the focused pane's rows* — `SmatchetActiveProjectGridUi.cpp:221-224`). The H1 confusion re-enters through the multi-pane door. **Fix:** include a per-pane in-flight signal in `syncLoading`; prefer an explicit per-pane loading state over the borrowed-snapshot fallback.
- **P2-M10 — The pass-1 settings search misdirects: keyword bags drifted from tab contents, and chips stop at the top-level tab.** `SmatchetPreferencesUi.cpp:759-783` — "Appearance" claims `theme ... color dark light` but themes live in the View menu; "Local data" claims `log level verbose` while the footer itself says those live in Runtime Log; "Grid" claims `tooltip` but the overflow-tooltip checkbox is on Appearance (`_Local.cpp:321`). Chips also can't jump to sub-tabs ("worklog template" lands on Fields Inputs, then the user hunts through four sub-tabs), show no hint of *which setting* matched, and the substring match lights up "Linear" for a query of "in". **Fix:** prune bags to labels that actually render; for out-of-Preferences settings show a "lives in View menu / Runtime Log" note; extend `PrefsTabFlags` to sub-tab bars; require whole-word or ≥3-char matches.
- **P2-M11 — Project picker swallows the fetch error and never retries; failure renders as a bare "  -".** `SmatchetProjectPicker.cpp:75,91,124` — the worker stores `fetchError` but nothing reads it; `fetchDone` latches, so a bad/expired PAT makes the picker permanently, inexplicably empty for the session. **Fix:** render the error with a Retry row that clears `fetchDone`; replace "  -" with "No projects found." / "Couldn't load projects".
- **P2-M12 — No "Test connection" for tracker credentials — the only validation is a full Save & Sync.** `SmatchetPreferencesUi.cpp:314-396` — both sibling credential surfaces have one (Assistant `_Assistant.cpp:417`, Whisper `_Whisper.cpp:363`); the first-run path commits unverified credentials and infers success from the grid. **Fix:** per-backend probe button reusing the existing worker + verdict-line pattern ("Connected as ...").
- **P2-M13 — Bug report: after the egress preview seeds, later description edits are silently excluded from the submission.** `SmatchetBugReportUi.cpp:64-66,176,188` — `BodyOverride` sends the (stale) preview while the UI still shows an editable description and the "This exact text is sent." promise sits inside a possibly-collapsed header — silent loss of typed content on the app's own feedback channel. **Fix:** merge post-seed description edits into the preview, or show "preview is out of date — description changes won't be sent" beside Submit.
- **P2-M14 — Bulk import can't be stopped, and the only way to stop it (closing the window) destroys the per-row results.** `SmatchetBulkTicketsUi.cpp:422-433,269-283` — a 1,000-row import against the wrong project can only be watched; closing cancels but erases the Status column, so the user can't tell what was created before the abort. **Fix:** a Stop button that keeps statuses + a "Copy results" action.
- **P2-M15 — Terminology drift: one window carries three names.** File menu "Import Issues..." (`SmatchetUI_MainMenu.cpp:314`), View toggle "Bulk Import" (`:733`), window title "Bulk import tickets" (`SmatchetBulkTicketsUi.cpp:560`); grid states say *issues*, mobile and Preferences say *tickets*, and the omnibar mixes both in one sentence ("Ticket key — Enter opens this issue.", `SmatchetOmnibarUi.cpp:53`). **Fix:** standardize on "issue" in UI strings; reserve "ticket" for code identifiers.
- **P2-M16 — Destructive-confirm button order flips between modals.** Action-first in the layout modal, delete-view, and conflict dialog (`SmatchetUI_MainMenu.cpp:522-527`, `SmatchetViewsDashboardUi.cpp:900-910`, `SmatchetOfflineQueueUi.cpp:1288-1295`) but Cancel-first in DB-recreate (`SmatchetPreferencesUi_Local.cpp:138-142`); only delete-view gets destructive-red styling. **Fix:** one order + one destructive style across confirms.
- **P2-M17 — Annotate failures render in the same neutral status line as successes, in dev-speak.** `AnnotateAnalysisUi_Window.cpp:807,810,940,107-110` — "assignee field not in catalog." reads like an informational note, indistinguishable from "CSV export copied", persisting with no dismiss. **Fix:** distinct error styling + cause-and-fix copy ("Couldn't match Perforce user 'X' to a Jira account — assign manually...").
- **P2-M18 — "Jira" hardcoded on backend-agnostic surfaces.** `SmatchetGridUiSupport.cpp:265` ("Failed to post Jira comment." on GitHub/Plane/Linear too), `:292,303,360` (header meta popup), and the pane picker builds "New jira pane" from the raw lowercase key (`SmatchetActiveProjectGridUi.cpp:104`) — the same internal-vocab class M6/H2 fixed, on surfaces that bypassed the new seam. **Fix:** reuse the display-name mapping; make header meta labels backend-neutral.

### 8.3 Low-severity / polish

- **P2-L1** — Backend Audit: no empty/zero-results state ("Page 1/1 (0 rows)") and the footer's "latest 1000+ events" overstates the exact-1000 cap (`SmatchetAuditUi.cpp:314,334-336,92`).
- **P2-L2** — Notification Center: actionable rows are indistinguishable from inert ones until hover, and "Clear all" wipes the error trail in one unguarded click (`SmatchetNotificationCenterUi.cpp:86-91,108`).
- **P2-L3** — Offline panel speaks queue-engineering ("Dead create", "replay", "rows ... from DB") where a user needs "failed — won't retry automatically" (`SmatchetOfflineQueueUi.cpp:682-684,765,929`).
- **P2-L4** — Toolbar fallback labels are the first 2 bytes of the tooltip/command id ("Ne", "vi"; can bisect UTF-8) when the icon font is missing — contrast the grid header's full-text fallback policy (`SmatchetToolbarUi.cpp:246-252` vs `SmatchetGridHeaderUi.cpp:544-545`).
- **P2-L5** — Grid-family affordance drift: "+ New Issue" vs "+ New issue"; the Sort-By delete is a bare un-tooltipped "X"; welcome links open a browser with no external-link cue or URL tooltip, and "Plane API docs" breaks the "Get a ... token" label pattern (`SmatchetGridHeaderUi.cpp:546,101`, `SmatchetNewIssueDraftUi.cpp:203,305`, `SmatchetActiveProjectGridTable.cpp:358-375`).
- **P2-L6** — Capitalization drift in the new grouped View menu: "Plan docs" among Title Case siblings; window titles split between "Bulk import tickets" and "Notifications"; "Read-only Mode" half-capitalizes (`SmatchetUI_MainMenu.cpp:726,321`, `SmatchetBulkTicketsUi.cpp:560`).
- **P2-L7** — New chrome strings dodge the localization seam: the layout modal assembles a sentence from fragments (`"%s resets the window layout."`), connectivity tooltips pass pre-built strings as `%s` args, and the badge hand-rolls English plurals while `notifCenter.count_one/many` shows the right pattern (`SmatchetUI_MainMenu.cpp:499-514`, `SmatchetStatusBarUi.cpp:62-79,133,177`).
- **P2-L8** — The clickable status-bar error badge gives no pointer affordance — plain text, hand cursor only documented in the tooltip (`SmatchetStatusBarUi.cpp:178-186`).
- **P2-L9** — Comment-template editor: 512-byte buffer silently truncates longer bodies (first keystroke persists the truncation), and the per-row "✖" deletes hand-authored templates instantly on an autosaving tab (`SmatchetPreferencesUi_Templates.cpp:257,311-321,344-345`).
- **P2-L10** — "Enter to add" is wired to the Button, not the InputText (`IsItemFocused()` after the button), so Enter in the suggestion fields silently does nothing; duplicates are dropped without a hint (`SmatchetPreferencesUi_Templates.cpp:136-137,231-232`).
- **P2-L11** — Nine masked credential fields, none with a reveal toggle or length hint — the codebase's own #979 comment records a trailing-space token bite this would have caught (`SmatchetPreferencesUi.cpp:319,340,354,377,571`, `_Assistant.cpp:561,584,608`, `_Whisper.cpp:341`).
- **P2-L12** — Plan docs empty state names directories the scanner doesn't scan ("under docs/plans/active or docs/adr." vs the actual `docs/design` + `docs/adr`), and an unreadable file renders as a silently blank body (`SmatchetPlanDocViewerUi.cpp:283,157-158,105-107`).

### 8.4 Cross-cutting themes (pass 2)

1. **Destructive actions without guards is the dominant class** — 9 of the findings above (P2-H2/H4/H5, P2-M1/M3/M5/M6, P2-L2/L9) destroy user work or state in one click/keypress. Pass 1 fixed the layout-reset instance; the pattern needs a repo-wide rule: any action that discards non-empty user input or deletes sole-copy data gets a confirm, an undo window, or recoverability.
2. **Failure feedback still trails success feedback** — the preview spinner that never resolves (P2-H7), the swallowed project-picker error (P2-M11), the neutral-styled Annotate failures (P2-M17), the silent browser fallback (P2-H8). The bulk-import failure copy (cause + fix + display-name mapping) is the house standard; these surfaces predate it.
3. **The pass-1 seams exist but aren't finished** — display names bypass the M6 mapping on three surfaces (P2-M18), the palette ignores the H2 `Title` contract (P2-M7), the H1 state machine misses the per-pane door (P2-M9), and the H5 theme fix left the semantic-color class behind (P2-H9). Each fix defined the right seam; the remaining work is sweeping the stragglers onto it.

### 8.5 Suggested prioritization (pass 2)

| Priority | Findings | Rationale |
|---|---|---|
| **Do first** | P2-H1 (omnibar overwrites views), P2-H2 (import duplicates), P2-H3 (credential loss), P2-H4 (draft loss) | Silent, durable data loss/corruption on common paths — the omnibar and first-run ones actively undermine pass-1 features. |
| **Do next** | P2-H5–H8 (Esc wipe, toast click-through, attachment failure modes), P2-M1–M6 | Work-destroying interactions + the guard-pattern debt. |
| **Then** | P2-H9 (semantic color slots), P2-M7–M14 | Theme correctness + finishing the pass-1 seams. |
| **Polish pass** | P2-M15–M18, P2-L1–L12 | Terminology, ordering, copy, affordance sweeps — cheap together. |

### 8.6 What the current tree does well (pass 2 observations)

- **Bulk import's per-row "Changes" diff** — live field-count with full old→new tooltip, visibly-disabled no-op rows, and plain-language skip semantics before commit (`SmatchetBulkTicketsUi.cpp:448-471,222-226`).
- **Bug-report privacy messaging** — "Attach screenshot (text redacted)" with the block-glyph explanation, plus a genuinely WYSIWYG editable egress preview (`SmatchetBugReportUi.cpp:328-334,205`) — undermined only by the stale-preview gap (P2-M13).
- **Whisper's diagnostic ladder** — Test connection → 3 s mic test with classified verdicts → end-to-end test with per-phase progress and fix-naming fast-fails (`_Whisper.cpp:483-487,586,662`).
- **The Assistant tab's staged-edit model** — dirty `*` in the tab label, explicit Save/Discard, sticky validation banner, real 1-token handshake test (`_Assistant.cpp:243-268,750-781`) — the template P2-H3 asks the Tracker tab to copy.
- **The pass-1 grid states landed well** — cause-aware zero-results strips with matching actions, ghost-state pruning, actionable first-run welcome with working token deep-links (`SmatchetActiveProjectGridTable.cpp:353-366,437-456,476-487`).

*Pass 2 was produced by static reading (four parallel area reviews, findings re-verified against the tree at `develop`/`87d1b6e`); the run-the-app visual verification recommended in pass 1 still has not been performed and remains the top validation gap before acting on the High findings.*
