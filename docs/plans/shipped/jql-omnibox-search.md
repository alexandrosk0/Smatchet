# Plan — Global JQL/ticket omnibox + icon-only Refresh buttons

> **Slug**: `jql-omnibox-search` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — both streams merged: Stream A icon-Refresh sweep (#1257, 2026-06-15) + Stream B global omnibox (#1261, 2026-06-16). Plan archived retroactively (#1261 shipped the code but did not update/move this doc). See § Implementation log.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Two user-requested UX upgrades to the ticket-grid surface:

1. **A "Chrome-like" omnibox** — a single, prominent, full-width search bar pinned at the top of the window that drives the **currently-focused grid pane**. It accepts **either** a JQL/Plane query (re-fetch the pane) **or** a ticket key / title (jump straight to that ticket). This extends — does not replace — the existing per-pane JQL machinery and the per-pane client-side quick-filter (`##GridFilter`), which stays separate (server-query vs client-row-filter are different concepts; user confirmed "keep both").
2. **Icon-only Refresh buttons** — replace the text "Refresh"-style buttons across the UI with a FontAwesome glyph (`ICON_FA_ARROWS_ROTATE`) + hover tooltip, matching a modern toolbar idiom. User confirmed "all refresh buttons", not just the grid header.

**Decisions locked with the user (AskUserQuestion):** scope = JQL **+** jump-to-ticket-by-key/title (NOT the command palette); placement = **global top bar** driving the active/focused grid (not per-pane); Refresh = **all** refresh-style buttons; quick-filter = **kept separate**.

Intended outcome — *after this lands*: a user can type a query or a ticket key in one top bar and the focused grid responds, and every Refresh control is a compact icon with a tooltip (graceful text fallback when the icon font is unavailable).

Originating request: in-session user ask (2026-06-14). No prior Issue/PR.

## Approach

**Two independent features → two PR streams** (per `AGENTS.md` § Autonomous ship-loop § PR-batching: one PR per logical feature):

- **Stream A — icon-Refresh sweep (small, ships first).** Add one reusable helper `SmatchetIconButton(icon, fallbackLabel, tooltip)` that renders an icon-only `ImGui::Button` + tooltip **when `SmatchetAreFaIconsLoaded()` is true**, else falls back to the original text button (preserves behaviour when `fa-solid-900.ttf` is absent). Replace the genuine refresh/reload sites with it. The two commit-style "& Sync" buttons (`Save & Sync`, `Apply & Sync`) are **not** pure refreshes — they get an icon-*leading* button that keeps its text label (dropping the label on a write-and-sync action hurts clarity); called out under § Risks.

- **Stream B — global omnibox.** Three slices: **(2a)** refactor the existing embedded JQL editor so its buffer + autocomplete state are a reusable `JqlEditorState` instance (the omnibox needs its *own* instance to avoid sharing request-ids with the dashboard editor — recon flagged a real in-flight-future collision); **(2b)** draw a top toolbar via `ImGui::BeginViewportSideBar(ImGuiDir_Up)` from **inside `SmatchetUI::Draw`, immediately after the main menu bar** — riding the same one-frame-deferred ImGui work-area reservation the menu bar already uses (verified below), so the dockspace accommodates it with **no frame-loop or host edit** — and fill it with the JQL editor wired to the focused pane via `SyncWithCurrentView`; **(2c)** add ticket-key/title detection that, on Enter, routes to `ITrackerIssueReader::FetchIssuesForKeys` (async, worker-thread) → select-in-grid or open-browse.

**Grill finding — the bar placement is lower-risk than it first looks (verified in code).** The standalone frame loop creates the dockspace (`StandaloneAppBootstrap.cpp:563`, `main.cpp:779`) *before* it calls `SmatchetUI::Draw` (`SmatchetDrawFrameWithSeh` at `:574`) — yet the main menu bar, drawn from within `Draw`'s call tree (transitively, via `drawChromeAndModeToggles`→`drawMainMenuBar`, which lives in the separate TU `SmatchetUI_MainMenu.cpp:67`), still reserves its strip. That works because ImGui's menu/side bars reserve viewport work-area space that the *next* frame's `DockSpaceOverViewport` honours (one-frame-deferred). The omnibar rides the identical mechanism: drawn after the menu bar inside `Draw`, it reserves the strip below the menu and the dockspace shrinks to fit on the following frame. So **mechanism A (no host/frame-loop edit) is the primary**; the original "reserve before the dockspace at the host seam" is the **fallback** (mechanism B) only if `BeginViewportSideBar`'s deferral proves insufficient at the live ImGui version — validate A first.

**Non-obvious trade-off that shaped 2a:** the JQL editor's autocomplete state (`jqlAcp*` cluster) is baked into `UiDrawSession` and keyed by a single shared request-id. Two live editors (dashboard + omnibox) sharing it would stale each other's async user-search results. Extracting a `JqlEditorState` struct (two instances) is the DRY-correct fix vs. duplicating the field cluster with an `omni`-prefix (Pillar 5 clone). We accept the wider 2a diff to avoid the clone.

**Why the apply path is crash-safe by construction:** the omnibox JQL apply mutates `d.cfg.JqlQuery` in place + calls `SyncWithCurrentView` — exactly what the per-pane Refresh button already does (`SmatchetGridHeaderUi.cpp:213`). It performs **no** `Views::Create`/`DeleteActive`, so it never resizes the `ViewsStore` vector and the store-resize dangling-pointer class (PR #962) does not arise on this path.

## Files to modify

**Stream A — icon-Refresh (PR 1)**

1. New `Source/Core/include/Ui/SmatchetIconButtons.h` + `Source/Core/src/Ui/SmatchetIconButtons.cpp` — `SmatchetIconButton(const char* icon, const char* fallbackLabel, const char* tooltip, ImVec2 size = {})` returning `bool`. **Grep first** (`rg -l 'IconButton|SmatchetIconButton' Source/Core/`) — recon found no existing icon-only button helper, but confirm before adding (the `CalcTextSize` button-width sizing pattern at `SmatchetAiAssistantUi.cpp:365` — a close-button width calc — is the reference if a square button is wanted).
2. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:209` — `"Refresh View"` → `SmatchetIconButton(ICON_FA_ARROWS_ROTATE, "Refresh View", "Re-run this view's query and refresh the grid.")`; keep the existing focused/deferred branch body unchanged.
3. `Source/Core/src/Ui/SmatchetAuditUi.cpp:271` — `"Refresh"` → icon (tooltip "Reload the audit trail.").
4. `Source/Core/src/Ui/SmatchetUserInfoUi_Sections.cpp:143` — `"Reload"/"Load"` dual-state → icon with state-aware tooltip ("Load activity" / "Reload activity"); `ICON_FA_ARROWS_ROTATE` covers both.
5. `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp:833` — `"Re-run setup banner"` → icon (tooltip "Re-run Whisper setup.").
6. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:719` (`Save & Sync` — the existing call is `ImGui::Button("Save & Sync", ImVec2(140.0f, 0.0f))`; the fixed 140px width must be preserved/reconciled by the icon-leading replacement) and `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:314` (`Apply & Sync`) — **icon-leading, label retained** (`ICON_FA_ARROWS_ROTATE " Save & Sync"`), not icon-only; see § Risks.
7. `tests/Core/` — new `SmatchetIconButtons.test.cpp` for the pure fallback-selection logic (see § Verification Bucket A). **Also edit `tests/CMakeLists.txt`** — register `Core/SmatchetIconButtons.test.cpp` in the hand-maintained `add_executable(SmatchetTests …)` source list (starts `:98`) **and** list `${CMAKE_SOURCE_DIR}/Source/Core/src/Ui/SmatchetIconButtons.cpp` (TU is non-header-only — mirror the `UiThreadAffinity` pattern at `:106-107`). The configure-time glob-vs-list guard at `tests/CMakeLists.txt:910-928` `FATAL_ERROR`s the **whole** CMake configure if a `Core/*.test.cpp` is unlisted — this edit is mandatory.

**Stream B — global omnibox (PR 2; may split 2a into its own PR if the diff is large)**

*Slice 2a — reusable JQL editor state*
8. `Source/Core/include/Ui/SmatchetUiSession.h:422-466` — extract the `viewJqlBuf[512]` (`:423`; `viewNameBuf` at `:422`) + `jqlAcp*` cluster (`:425-466`, **including the `JqlUserSearchResult` struct + `std::future<JqlUserSearchResult>` member at `:465` and `jqlAcpUserSearchInFlightId` at `:466` — these move too**) into `struct JqlEditorState { … };`; give `UiDrawSession` two instances: `viewJqlEditor` (existing dashboard) + `omniJqlEditor` (new). Keep field semantics identical. *(Not `:398-441` — `:398-400` are unrelated MCP fields and the old end-line truncated the future/in-flight-id.)*
9. `Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp:310` — `DrawJqlQueryEditorEmbedded(app, d)` → `(app, d, JqlEditorState& st, TrackerQuerySuggestKind kind, <layout opts>)`; replace all `d.viewJqlBuf`/`d.jqlAcp*` reads with `st.*`. Update the project-pill (`:244`) + clear-button (`:343`) likewise.
10. `Source/Core/include/Ui/SmatchetAutocompleteUi.h:16-22` + `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp` — `TrackerQueryAcpCallbackUserData` already carries `session`/`app`/`kind`; thread the `JqlEditorState*` through so `InputTextCallback` / `TickDebouncedUserSearch` / `DrawPopup` / `FlushPendingReplace` read per-instance state (not `d.jqlAcp*`). This is the load-bearing refactor — the request-id that invalidates in-flight futures (increment at `:341`; armed/in-flight ids at `:343`/`:402`) must live in `JqlEditorState`, one per bar.
11. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:374` — update the single existing caller to pass `d.viewJqlEditor`, the derived `kind`, and the dashboard layout.

*Slice 2b — top bar region + omnibox content*
12. **Mechanism A (primary) — no host edit.** Draw the bar via `ImGui::BeginViewportSideBar("##SmatchetOmnibar", viewport, ImGuiDir_Up, barHeight, flags)` from **inside `SmatchetUI::Draw`, immediately after the main menu bar** (entry #14) — it rides ImGui's one-frame-deferred work-area reservation, the same path the menu bar already uses despite the dockspace being created before `Draw` (verified: `StandaloneAppBootstrap.cpp:563` / `main.cpp:779` create the dockspace, `:574` calls `Draw`; the menu bar — drawn transitively from `Draw`'s call tree via `drawChromeAndModeToggles`→`drawMainMenuBar` in the separate TU `SmatchetUI_MainMenu.cpp:67` — still reserves). No `SmatchetImGuiHost.cpp` / frame-loop change. **Fallback B (only if A's deferral proves insufficient at the live ImGui version):** reserve the strip at the host seam *before* `DockSpaceOverViewport` — `SmatchetImGuiHost.cpp:753` + the two standalone dockspace sites (`StandaloneAppBootstrap.cpp:563`, `main.cpp:779`) — and route that variant to the `ui-host` agent. Validate A before touching B.
13. New `Source/Core/src/Ui/SmatchetOmnibarUi.cpp` (+ header) — `DrawOmnibar(AppController&, UiDrawSession&)`: renders `DrawJqlQueryEditorEmbedded(app, d, d.omniJqlEditor, kindForFocusedPane, omnibarLayout)`, a leading mode glyph (`ICON_FA_MAGNIFYING_GLASS` for query / a ticket glyph when input parses as a key), and a trailing icon-Refresh. Section-helper shape from the start (`docs/guides/imgui-draw-pattern.md`) — it will approach the 200-line draw cap.
14. `Source/Core/src/Ui/SmatchetUI.cpp` (root `Draw` at `:289`) — wire the omnibar draw **between the `drawChromeAndModeToggles` call (`:328`) and the `drawSecondaryWindows` call (`:330`)** — i.e. after the menu bar (drawn transitively inside `drawChromeAndModeToggles` (`:662`)→`drawMainMenuBar` (`:665`), which lives in the separate TU `SmatchetUI_MainMenu.cpp:67`) and before the pane windows (`drawGridPaneWindows` at `:895`, inside `drawSecondaryWindows`). Respect the mobile-mode fork at `:316` (the `EffectiveUiMode::Mobile` check; the early `return` is `:320`) — desktop-only v1, see § Out of scope. *(Not `~:664/:910` — those are perf-scope lines inside two separate helper methods, not the seam.)*
15. Apply path: on Enter in query mode → set `d.cfg.JqlQuery` from `d.omniJqlEditor.buf`, then `SyncWithCurrentView(app, d, ViewState.GetStore(), true)` targeting `d.focusedPane()` (stable, last-frame-resolved — see § Risks focus note). Mirror the focused/deferred branch from `SmatchetGridHeaderUi.cpp:210-220`.

*Slice 2c — ticket quick-find*
16. New pure helper `Source/Core/include/Ui/OmnibarInputClassifier.h` (+ small `.cpp` or header-only) — `ClassifyOmnibarInput(const std::string&, TrackerType) → {Jql | TicketKey | TitleSearch}` reusing `ExtractIssueKeyPrefix` (`Tracker/ProjectResolver.h:17`) for Jira keys and `ParseGitHubIssueKey` (`Tracker/GitHubClientHelpers.h:33`) for GitHub; Plane has no key format → degrade to TitleSearch.
17. `Source/Core/src/Ui/SmatchetOmnibarUi.cpp` — ticket-mode Enter handler: dispatch `ITrackerIssueReader::FetchIssuesForKeys(cfg, {key}, views)` (`ITrackerIssueReader.h:56`) **async on a worker** (mirror the debounced-user-search `std::async` + `wait_for(0s)` poll pattern at `SmatchetAutocompleteUi.cpp:350-351` (poll) + `:397-408` (dispatch; `std::async` at `:404`)); on success → `SpreadsheetState::SetActiveIssue` (`SpreadsheetState.h:85`) on the focused pane if the row is loaded, else open browse URL via `BuildBrowseUrl` (`ITrackerIssueReader.h:63`) + `app.OpenUrl`. Title mode v1 = substring match over already-loaded rows only.
18. `tests/Core/OmnibarInputClassifier.test.cpp` — pure-logic table tests for key vs JQL vs title classification across the three backends. **Also edit `tests/CMakeLists.txt`** — register `Core/OmnibarInputClassifier.test.cpp` in the `add_executable(SmatchetTests …)` source list; if the classifier (#16) is header-only no extra production `.cpp` is needed, else list it. Same `:910-928` `FATAL_ERROR` guard applies.

## Existing utilities reused

- `SyncWithCurrentView(AppController&, UiDrawSession&, const ViewsStore&, bool)` — `SmatchetGridUiSupport.h:48` / impl `SmatchetGridUiSupport.cpp:529` — the single canonical "apply this query to the focused pane and re-fetch" route; both the per-pane Refresh and the omnibox use it.
- `UiDrawSession::focusedPane()` / `focusedPaneId` — `SmatchetUiSession.h:619`/`:586` — resolves the active pane the omnibox targets (`FindGridPaneById`, falls back to first pane, never null).
- `DrawJqlQueryEditorEmbedded` + `TrackerQueryAcp_*` family — `SmatchetViewsDashboardUi_widgets.cpp:307`, `SmatchetAutocompleteUi.cpp` — the JQL input + autocomplete popup + project pill, lifted into the omnibox via the 2a refactor.
- `SmatchetAreFaIconsLoaded()` — `SmatchetImGuiFonts.cpp:550` — runtime guard for the icon/text fallback in `SmatchetIconButton`.
- `ICON_FA_ARROWS_ROTATE` (U+f021) / `ICON_FA_MAGNIFYING_GLASS` — `IconsFontAwesome6.h` — refresh + search glyphs; the FA solid font is already merged to the atlas (`SmatchetImGuiFonts.cpp:260-282`).
- `ITrackerIssueReader::FetchIssuesForKeys` / `BuildBrowseUrl` — `ITrackerIssueReader.h:56`/`:63` — per-backend single-ticket fetch + browse-URL for ticket-jump.
- `ExtractIssueKeyPrefix` (`ProjectResolver.h:17`), `ParseGitHubIssueKey` (`GitHubClientHelpers.h:33`) — key-format detection for the input classifier.
- `SpreadsheetState::SetActiveIssue` — `SpreadsheetState.h:85` — select/focus the jumped-to ticket in the focused grid.
- Per-pane deferred-action latch `paneDeferredActionPaneId` / `PaneDeferredActionKind::RefreshView` — `SmatchetGridHeaderUi.cpp:215-219` — reuse for the not-yet-focused-pane apply edge.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: the always-drawn omnibox adds one `InputText` + the existing per-frame *sync* suggest-build (already cheap); the expensive cross-backend user-search stays **debounced at 220 ms** (`SmatchetAutocompleteUi.cpp:344`), unchanged. Wrap the omnibar in one `SMATCHET_UI_PERF_SCOPE("omnibar")` and confirm steady-state delta ≈ 0 ms; regen the marker inventory.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: both heavy paths are async — JQL apply goes through `SyncWithCurrentView`→`SyncWithBackend` (already worker-posted); ticket-key fetch dispatches `FetchIssuesForKeys` via `std::async` + `wait_for(0s)` poll, **never inline `cpr`** on the render thread. In-flight = a visible spinner/disabled state in the bar. Annotate the fetch site `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms`.
- **Pillar 3 (never crash)**: apply path is in-place `cfg` mutation only — no `Views::Create`/`DeleteActive`, so no `ViewsStore` reallocation / dangling `ViewDefinition*`. Icon buttons fall back to text when the font is missing. All input buffers fixed-size (`[512]`), classifier is bounds-checked pure logic.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: every icon-only button carries a **mandatory tooltip** (no naked glyph); the omnibox is keyboard-reachable (focus + Enter to apply, Esc to dismiss the popup). Icon glyphs scale with the merged font atlas. Contrast of the new bar chrome to be checked against the active `SmatchetTheme` palette.

## Perf-review-system gates (diff touches `Source/Core/` → MANDATORY)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`.

1. **PR-fast CI** — **fires**: the changed path is JQL autocomplete + grid re-fetch; named scenario = the JQL/grid-load scenario in `scripts/dev/perf-pr-fast-set.json` (confirm exact id via `agents/core/perf-gatekeeper.md` § Curated diff → scenario map; the omnibar draw + autocomplete is the directly-exercised path).
2. **Pillar 2 static scanner** — **fires**: Slice 2c adds a new sync-capable fetch (`FetchIssuesForKeys`) reachable from `ImGui::*`. Plan: worker-thread `std::async` + poll; annotate `/* PILLAR2_WORKER_ONLY */ // est-latency: <N>ms` at the dispatch site. No new inline I/O on the render path.
3. **Dispatcher drain** — **N/A** — does not touch `MainThreadDispatcher::Drain()` (ticket-fetch result handled via per-frame `wait_for(0s)` poll, not a posted lambda; if that changes, keep posted lambdas chunked).
4. **Visible-cue bucket-E harness** — **fires**: the ticket-fetch can exceed 100 ms (esp. GitHub's per-key REST round-trip / Plane's client-side stream-filter) → add the in-bar spinner/disabled cue + a bucket-E assertion that the cue shows while a fetch is pending.
5. **Marker inventory** — **fires**: adds `SMATCHET_UI_PERF_SCOPE("omnibar")` → regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the named scenario(s) before opening PR 2.

**Override**: `perf-out-of-band` label only if an intentional regression + baseline-bump PR is queued — not expected here.

## Risks / non-goals

- **Bar placement (2b) — de-risked by the grill, but version-sensitive.** Primary mechanism A draws the bar from inside `SmatchetUI::Draw` after the menu bar and rides ImGui's one-frame-deferred work-area reservation (no host edit) — verified plausible because the menu bar already reserves this way despite the dockspace being created before `Draw` (`StandaloneAppBootstrap.cpp:563`/`main.cpp:779` → `:574`). *Residual risk*: `BeginViewportSideBar`'s deferral could under-reserve at the live ImGui version, leaving a one-frame overlap. *Mitigation*: fallback B reserves the strip at the host seam **before** `DockSpaceOverViewport` (`SmatchetImGuiHost.cpp:753` + both standalone dockspace sites) and routes to `ui-host`; the feature still spans host + UI + tracker + grid (3+ subsystems) → an `architect` framing pass is warranted only if B is taken. Validate A first.
- **Focus-race (PR #962)** — the omnibox renders *above* the panes, so it reads `d.focusedPaneId` resolved by the *previous* frame's focus-resolution block (`SmatchetGridPaneWindows.cpp:172-189`, which commits `focusedPaneId` after the pane render loop at `:146-152`). That is **intended**: the focused pane is the one the user was looking at when they typed; `focusedPaneId` is stable session state, not a previous-frame *focus flag* of the omnibox's own window. *Mitigation*: apply against `focusedPane()` (never null) and use the existing `paneDeferredActionPaneId` latch for the not-yet-focused edge — do **not** gate on a stale per-window focus bool.
- **`Save & Sync` / `Apply & Sync` are commit actions, not refreshes** — accepted: keep their text label, add a *leading* icon only. If the user prefers icon-only there too, that's a one-line follow-up. Flagged because "all refresh buttons" literally includes them but their semantics differ.
- **Per-backend ticket-fetch asymmetry** — Jira batches `key in (...)`; GitHub issues one REST call per key; Plane has **no** key format and would stream the whole project to filter client-side. *Mitigation*: v1 ticket-jump = Jira `PROJ-123` + GitHub `owner/repo#N` only; Plane input falls through to title-substring over loaded rows. Surface per-backend `Result<>` errors in the bar.
- **2a shared-state refactor** — touching the autocomplete request-id (`SmatchetAutocompleteUi.cpp:341`, the `++` increment) risks regressing the existing dashboard editor. *Mitigation*: 2a ships with the dashboard editor's bucket-E coverage green **before** 2b adds the second instance; the two-instance request-id isolation is the explicit test target.
- **Icon font absent** — `fa-solid-900.ttf` unresolved → `SmatchetAreFaIconsLoaded()` false. *Mitigation*: `SmatchetIconButton` text fallback (built-in, not optional); verify asset delivery in the build (`assets/fonts/`).
- **Non-goals**: NOT a unified command palette (user excluded commands); NOT replacing the per-pane `##GridFilter` quick-filter (kept separate); NOT server-side free-text title search; NOT mobile-mode rendering (v1 desktop only).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where physically possible.

- **Bucket A (pure-logic ctest, `test-rig`)**: `OmnibarInputClassifier.test.cpp` — JQL vs ticket-key vs title across Jira/GitHub/Plane (incl. `SMAT-1234` → key, `project = X` → JQL, `owner/repo#5` → GitHub key, Plane UUID/free-text → title). `SmatchetIconButtons.test.cpp` — fallback-selection logic (icons-loaded → icon path, else → text path) factored as a pure predicate.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: omnibar renders below the menu bar; typing JQL + Enter calls `SyncWithCurrentView` against the focused pane; typing a key + Enter triggers the (mocked) fetch + select; the in-flight spinner shows while pending (Pillar-2 cue); icon-Refresh buttons are clickable and show their tooltip; **2a regression** — dashboard editor + omnibox each keep their own in-flight user-search results (no request-id cross-stale).
- **Bash-driver scenario / screenshot / sanitizer**: bucket-C screenshot diff covering the new bar + the iconified header Refresh (this is a **visual change** → see § visual-validation note below); ASAN/UBSan lanes green on the apply + async-fetch paths.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — the new `Ui/` files compile under both GL and DX12; no GLFW/GL in any new `Source/Core/include/**` header).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint — defer to the script). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run `grill-with-docs` against this plan (sharpen "omnibox" vs "quick-filter" terminology, challenge the host-seam ordering + the per-pane vs session JqlQuery scoping) before finalising; record the outcome.
- **Visual-validation exception**: this diff touches `Smatchet*Ui*.cpp` → per `AGENTS.md` Pillar-4 note the ship-loop **pauses for user visual verify** *unless* the bucket-C/E coverage above lands. Implementation must add that coverage to keep the loop autonomous; otherwise the orchestrator pauses with the launched exe for the user's verdict.
- **Manual residue**: if any step ends up manual (e.g. WCAG-AA contrast of the new bar chrome — no automated contrast gate today), name the deferred-automation action plan + add a `docs/self-improvement/categories/tooling.md` entry. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **Command-palette fusion** — user excluded it; the omnibox does not route `>command` syntax. Follow-up plan if wanted later.
- **Server-side title search** — v1 title mode is substring over already-loaded rows only; a real free-text index (esp. Plane `sequence_id__in` server filter, noted out-of-scope at `PlaneIssueSearch.cpp:589`) is a follow-up.
- **Mobile-mode omnibar** — desktop-only v1; mobile fork (`EffectiveUiMode::Mobile` check at `SmatchetUI.cpp:316`, early `return` at `:320`) keeps its existing chrome. Follow-up.
- **Offline write-queue retry button** (`SmatchetOfflineQueueUi.cpp:730`, "Retry creates now") — **not** iconified with `ICON_FA_ARROWS_ROTATE`. It flushes the offline *write* queue (`TickOfflineCreates`/`TickOfflineFieldEdits`), a commit/retry action like `Save & Sync`, **not** a view refresh. Intentionally excluded from the Stream-A refresh-iconify sweep; a distinct icon (e.g. `ICON_FA_PAPER_PLANE`) is a follow-up if wanted.
- **Icon-only for `Save & Sync` / `Apply & Sync`** — kept text-labelled (commit semantics); icon-only is a one-line follow-up if the user prefers.
- **Multi-pane broadcast** — the omnibox drives only the focused pane, not all panes at once. No-action (matches the per-pane mental model).

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

**Stream A — icon-Refresh sweep (PR #1257, base `develop`) — SHIPPED.**
- `161ebb37` · feat(ui): icon helpers (`SmatchetIconButton` icon-only + `SmatchetIconLeadingButton` icon-leading) + 6 call sites iconified + header-only pure-label test.
- `1166df0b` · merge `origin/develop` forward (clean, no conflict; test entry survived at `tests/CMakeLists.txt:251`).
- `2544ebf7` · style(ui): tighten `SmatchetIconButtons.h` doc comments (drop bare `//` blank-run, condense).

**Stream B — global omnibox (PR #1261, merged 2026-06-16) — SHIPPED.** Squash-merged as `e6445a27`. Delivered all three slices in one PR:
- **2a** — extracted `struct JqlEditorState` (`SmatchetUiSession.h`) with two instances (`viewJqlEditor` dashboard + `omniJqlEditor`); `DrawJqlQueryEditorEmbedded(app, d, JqlEditorState&, …)` re-signatured; the `TrackerQueryAcp*` autocomplete family (`SmatchetAutocompleteUi.{h,cpp}`) threads the per-instance `JqlEditorState*` so the two editors no longer share a request-id (the in-flight-future collision the recon flagged).
- **2b** — new `SmatchetOmnibarUi.cpp` (`drawOmnibar`) drawn via **mechanism A** (`ImGui::BeginViewportSideBar(ImGuiDir_Up)` from inside `SmatchetUI::Draw` — no host/frame-loop edit); wired at `SmatchetUI.cpp:334`; `SMATCHET_UI_PERF_SCOPE("SmatchetUI::drawOmnibar")`.
- **2c** — new pure `OmnibarInputClassifier.h` (`ClassifyOmnibarInput` → `Jql | TicketKey | TitleSearch` across `Jira | Plane | GitHub`; Plane degrades to Jql/TitleSearch) + ticket-jump via `SpreadsheetState::SetActiveIssue` on the focused pane; bucket-A test `tests/Core/OmnibarInputClassifier.test.cpp`.

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

**Stream A:**
- **Files #7 — test rig: header-only split instead of listing the production `.cpp`.** The plan said register `Source/Core/src/Ui/SmatchetIconButtons.cpp` in the `SmatchetTests` source list. Instead the testable pure logic was extracted to a new header-only `Source/Core/include/Ui/SmatchetIconButtonLabel.h`, which the rig exercises directly. Rationale: the ImGui wrapper `.cpp` depends on `SmatchetAreFaIconsLoaded()` (in `SmatchetImGuiFonts.cpp`, not linked in the doctest rig) — listing it would drag ImGui/font deps into the pure-logic rig. Net coverage of the label-selection logic is unchanged (8 cases / 17 assertions). `tests/CMakeLists.txt` registers the test header-only (no extra `.cpp` link), which the `:910-928` glob-vs-list guard accepts.
- **New helper split into two functions, not one.** Plan named a single `SmatchetIconButton(icon, fallbackLabel, tooltip)`. Shipped as that **plus** `SmatchetIconLeadingButton(icon, label, tooltip)` for the two "& Sync" commit buttons (icon-leading, label retained) — the plan's § Risks already called out that those two sites differ; a dedicated helper keeps their id-derivation rule (translate-then-pin-English-`##`) distinct from the icon-only path (defer to `LabelFromSource`).

**Stream B:**
- **Bar drawn via the PRIMARY mechanism A, not the fallback.** `BeginViewportSideBar(ImGuiDir_Up)` from inside `Draw` rode ImGui's one-frame-deferred work-area reservation as the grill predicted; the host-seam fallback B (and its `ui-host`/`architect` routing) was not needed.
- **Ticket-jump v1 = select-in-focused-pane, not an async `FetchIssuesForKeys` round-trip.** The shipped 2c routes a recognised key to `SpreadsheetState::SetActiveIssue` on the focused pane (the plan's stated v1 scope: substring/loaded-row jump); the per-backend async fetch + browse-URL fallback remains the documented follow-up.
- **Plan doc was NOT moved/updated by #1261 (process miss, corrected here).** #1261 shipped the full Stream B but left this plan in `active/` with the impl-log saying "Stream B still active." This archival PR reconciles it retroactively — hence the Implementation log / Verification entries below are evidence-based (PR #1261 file list + CI), not in-session.

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

**Stream A (PR #1257):**
- **Bucket A (pure-logic doctest)** — PASS. `tests/Core/SmatchetIconButtons.test.cpp`, 8 cases / 17 assertions (icon path, two-same-glyph distinct ids, font-absent fallback, null/empty degradation for both helpers). Run via `SmatchetTests.exe --test-case="*IconButton*,*IconLeading*"` → SUCCESS.
- **Build gate (dual-target)** — PASS. `cmake --build build/ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` on the forward-merged tree: `Smatchet.exe` + `SmatchetCore_DX12.lib` both linked, 0 errors/warnings.
- **Localization** — verified preserved at all 6 sites by `code-review` agent (CLEAN, 0/0/0): ImGui ids stable + identical to the pre-change call sites across en/fr and font-loaded/absent; live fr-FR entries for "Save & Sync"/"Refresh"/"Reload" still apply.
- **pre-ship gate** — PASS (lint + doc-validation + code-review ack). comment-ratio + DRY-dup findings are WARN-only (calibration).
- **Bucket E (ImGui Test Engine) + Bucket C (screenshot)** — NOT-RUN for Stream A. Stream A is a visual change touching `Smatchet*Ui*.cpp` with no bucket-C/E coverage → **visual-validation exception applies**: ship-loop paused at the post-ship menu for user visual verify (glyph renders, tooltips on hover, text fallback when `fa-solid-900.ttf` absent). Automated bucket-C/E coverage for the icon buttons is deferred to Stream B's harness work (the omnibar bucket-E suite in § Verification covers the same draw path).

**Stream B (PR #1261):**
- **Bucket A (pure-logic doctest)** — PASS (via #1261's required `Coverage` CI, which builds + runs the doctest suite). `tests/Core/OmnibarInputClassifier.test.cpp` exercises JQL vs ticket-key vs title across Jira/GitHub/Plane.
- **Build gate (dual-target) + full required CI** — PASS. #1261 merged green on every required check (`Windows + MSVC`, light build, `Coverage`, `Perf PR-fast`, sanitizers, `Doc anchors`, `Test-delta`), so the new `Ui/` TUs compile under both GL and DX12 and the perf path is within budget.
- **Bucket E (ImGui Test Engine)** — apply-path coverage added post-archival in `tests/ui/omnibar_search_apply.test.cpp` (+ local driver `scripts/dev/test-ui-omnibar-search-apply.sh`, fixture-gated on the deterministic Jira backend). It drives the live `##SmatchetOmnibar` Enter through all three classifier branches: TitleSearch → focused-pane `gridFilterBuf`; Jql → `cfg.JqlQuery` + dashboard-editor mirror (the `applyQueryToPaneView` lock-step); TicketKey (`SMAT-1`) → loaded-row `SpreadsheetState::ActiveIssueId`. **Verified honest-green** (PR #1405): all three tests passed 3/0 across 4 consecutive runs with `tested=3` each run (genuinely executed, not skip-passed), built + run from an isolated detached worktree on Windows (MSVC + headless Mesa GL) with no edits to test/product/fixture. NOTE: an earlier revision of this section claimed residual omnibar bucket-E coverage was "tracked under `b8-bucket-e-coverage` / `testing-surface-roadmap`" — that was inaccurate (neither plan ever scoped it; b8's frozen L1–L5 set predated this feature), so this TU closes the gap directly. **Bucket C (screenshot)** of the new bar chrome remains deferred — no automated screenshot/contrast gate covers it yet.
- **Reconciliation note** — this archival PR touches docs only; no code re-run. The Stream B verdicts above are taken from PR #1261's merge-gate state (all required checks SUCCESS at merge), not a fresh in-session build.
