# Plan — Lightweight ticket-change monitor (notify on remote changes)

> **Slug**: `ticket-change-monitor` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules (plan-doc family, perf-gate section, scope-reduction sweep) and the Sync + Tracker leaf `AGENTS.md` files (strict lint zones).

## Context

User request: *"Monitor backend in a lightweight way for changes so you notify the user when any of the tickets get changed."* Today ticket sync is **pull-only on demand** — a refresh happens on manual sync or pane-open (`TicketSyncService::SyncWithBackend`), and there is **no passive auto-refresh** (the only interval-driven loop in core is the connectivity probe, `AppController_Connectivity.cpp:127`). So a teammate changing a ticket's status/assignee is invisible until the user manually refreshes.

After this lands: with the app focused and the tracker reachable, Smatchet periodically asks the backend *"what changed recently?"* for the open panes' tracked set, and raises an in-app **toast** when a salient field changes. Backend-agnostic (Jira / Plane / GitHub), opt-out via Preferences, **on by default**.

Confirmed scope decisions (user, this session): **periodic poll + diff** · scope = **active view / open panes** · channel = **in-app toast** · **on by default** with a toggle · default interval **120 s** · change filter = **salient fields only** (status/state, assignee, priority, summary/title, due date, sprint/iteration, labels/components) · toast = **always summarized** (one toast per batch; per-ticket detail — field, from→to, author — only when exactly one ticket changed), 5 s; **clicking any toast opens a new Notification Center window** (a chronological log of every toast) and **clicking a notification row focuses the changed ticket in the grid** · detection via a **server-side `updated`-filter probe** (Jira `updated >= -Nm` **+ `expand=changelog`**, GitHub `since=`, Plane `updated_at__gte`) so an idle poll is ≈ one near-empty request, **not** a full-view download (the changelog rides only on the few changed issues) · ship the **`tickets.monitor` command** in this PR.

## Approach

Reuse, don't rebuild — and **push the change-filter to the server** so polling is cheap.

1. **Trigger** — mirror the connectivity-probe interval pattern (`AppController_Connectivity.cpp:122-155`). Add a per-pane `nextChangePollAt` time-point on `GridLiveContext`; in the per-frame `AppController::TickAllContexts()` (`AppController.cpp:701-734`), when the interval elapses *and* the gate passes (enabled · backend reachable · window focused · no sync already active), dispatch a **lightweight change probe** on a worker thread. This is **not** the full streaming sync — off the UI thread, same discipline as a manual refresh, but a much smaller request.

2. **Server-side change query (the key to "lightweight")** — the probe calls a new `ITrackerIssueReader::FetchIssuesChangedSince(cfg, views, window, salientFields)` that pushes the time filter to the backend and requests **only the salient fields**:
   - **Jira**: `(<view JQL>) AND updated >= -Nm` on `/rest/api/3/search/jql` **with `expand=changelog`** (relative units ⇒ timezone-proof). The `updated` filter already returns only the few changed issues, so the changelog rides along just for those — each carries its recent field history (exact from→to + author) and idle polls stay empty. (The codebase already uses `expand=changelog` on this endpoint, `JiraIssueSearch.cpp:320`; if the bulk endpoint ever caps histories, fall back to per-issue `/issue/{key}/changelog` for the changed keys only.)
   - **GitHub**: the existing `…/issues?state=all&since=<ISO>` builder (`GitHubClientHelpers.cpp:155`) / the `updated:>=` GraphQL qualifier.
   - **Plane**: `updated_at__gte=<ISO>` query param (`PlaneIssueSearch.cpp`).
   - **Idle poll ⇒ near-empty response** (zero issues), so steady-state cost is one small request — never a full-view download. A backend without a native filter falls back to the existing full fetch (default impl), so the feature still works everywhere.

3. **Identify the *salient* change, then notify** — an `updated` bump can be a comment/watcher edit, so we pinpoint what actually changed. **Primary (history-based): parse the change history** — for Jira, the `expand=changelog` histories newer than the `since` anchor, filtered to salient fields — yielding the exact field, **from→to values, and the author** without needing the old cached value. **Fallback: cache field-diff** (compare the returned issue's salient fields against its cached row) for any backend/field without usable history. Either way we **patch the cache rows so the grid stays consistent**. Self-edits don't notify — the change author is matched against the current user (more robust than cache-diff) and tickets with a pending offline edit are skipped. On enable/startup the `since` anchor is the monitor-start time, so there's no backfill burst. The change list flows to a notifier (sibling of `SmatchetGridNotifications.cpp`) that pushes one **always-summarized**, deduped toast (per-ticket detail only for a lone change), 5 s. Every toast is also recorded in the toast manager's history with a per-entry **row action** (for ticket toasts: focus that ticket's pane + row); the **transient toast's own click opens the Notification Center** (see point 5).

4. **Membership reconcile (removals)** — the change probe sees *presences*, never *absences*: an issue that was deleted, or whose edit moved it *out* of the view (status→Done when the view hides Done; reassigned away), simply stops matching `(<view JQL>)`. So each cycle is paired with a **keys-only** fetch of the view (Jira `…&fields=*none` → id+key only; GitHub/Plane key-only) — tiny even for a full 100-issue view. `removed = pane's cached keys − fetched keys`; each vanished key gets one direct `GET /issue/{key}` to classify (**404 → deleted**, **200 → left the view**), then its cache row + grid entry are dropped and a toast fires (`PROJ-123 deleted` / `PROJ-123 left your view`). New keys in the set corroborate the change probe's additions. (Per user choice the reconcile runs every change cycle.)

5. **Notification Center (general toast history)** — a new window lists *every* toast the app raises (not just ticket changes), **newest-first**, each row showing timestamp · type · title · message. The toast manager gains a **bounded in-session history** (each entry keeps its row action). **Clicking any transient toast opens this window**; **clicking a row invokes that entry's action** — ticket entries → `FocusTicketInGrid`, others are informational. Also openable from the **View menu** + a `notifications` command. Session-only + bounded (~200); cross-restart persistence is a follow-up.

Trade-off named: polling (not webhooks/SSE) is deliberate — backend-agnostic across all three trackers with zero new server-side machinery. "Lightweight" is preserved primarily by the **server-side `updated` filter** (idle ⇒ near-empty), plus gating (focused + reachable + not-already-syncing), the 120 s default, and active-pane-only scope. Each cycle is **two small requests** — the change probe (empty when idle) + the keys-only membership reconcile (small) — plus one `GET /issue/{key}` only when a ticket disappears. Because the probe is *separate from* the full streaming sync, it raises **no sync spinner/toast** (no "refresh flash" every cycle). Real-time push is a future ADR, not this plan.

## Files to modify

*Grep-checked: `rg -l 'TicketChangeDiff|JqlChangedSince|FetchIssuesChangedSince|ShouldPollForChanges|nextChangePollAt|TicketChangeMonitor' Source/` returns nothing today — all new symbols below are genuinely new. GitHub's `since=` builder already exists (`GitHubClientHelpers.cpp:155`).*

**Pure logic (Sync — strict zone)**
1. `Source/Core/include/Sync/TicketChangeDiffPure.h` *(new)* — `TicketChangeSummary{ kind (Modified/Added/LeftView/Deleted), issueId, changedFieldLabel, fromValue, toValue, author }` (from→to + author from changelog when available; `FormatTicketChangeToast` wording branches on `kind`), the **salient-field set** (`IsSalientChangeField(canonicalId)` over status/state, assignee, priority, summary/title, due date, sprint/iteration, labels/components), `std::vector<TicketChangeSummary> DiffChangedTickets(prev, next)` (salient deltas only), `std::string FormatTicketChangeToast(changes, /*cap=*/1)` (lone change → detailed; ≥2 → "N issues changed: …").
2. `Source/Core/src/Sync/TicketChangeDiffPure.cpp` *(new)* — impl.
3. `Source/Core/include/Sync/JqlChangedSincePure.h` (+ `.cpp`) *(new)* — pure query-fragment builders: `WrapJqlChangedWithin(baseJql, minutes)` → `(<base>) AND updated >= -Nm`; `IsoSinceFromWindow(nowUnix, window)` (wraps `IsoZuluFromUnixSec`) for GitHub/Plane. Unit-testable, no network. Also `Source/Core/include/Sync/MembershipDiffPure.h` *(new)* — `std::vector<std::string> RemovedKeys(cachedKeys, fetchedKeys)` (order-stable set difference) for the removal reconcile.

**Backend probe (Tracker — strict zone)**
4. `Source/Core/include/ITrackerIssueReader.h` *(add)* — `virtual Result<std::vector<CachedTicket>, TrackerError> FetchIssuesChangedSince(const TrackerConfig&, const ViewsStore&, std::chrono::seconds window, const std::vector<std::string>& salientFields)` with a **default that falls back to the full `FetchIssues`** (heavy but correct) so unsupporting backends still work. Also add `FetchIssueKeysForView(cfg, views)` → `Result<std::vector<std::string>, TrackerError>` (keys only, for the membership reconcile; default = full `FetchIssues` + key extraction) and `ProbeIssueExists(cfg, issueKey)` → `Result<bool, TrackerError>` (one `GET /issue/{key}`: true = exists/left-view, false = deleted) for the disappearance classification.
5. `Source/Core/src/Tracker/JiraIssueSearch.cpp` (+ a pure `JiraChangelogDeltaPure` helper) — implement: JQL via `WrapJqlChangedWithin`, `fields=<salient>`, **`expand=changelog`**; parse histories newer than `since`, filter to salient fields → `TicketChangeSummary` (from→to + author); reuse `ProcessJiraSearchPage` + `AppendCachedTicketFromJiraSearchIssue`. Plus `FetchIssueKeysForView` (`…&fields=*none`, token-paginated; minimal-field fallback if `*none` unsupported) and `ProbeIssueExists` (`GET /rest/api/3/issue/{key}` → 200/404).
6. `Source/Core/src/Tracker/GitHubIssueSearch.cpp` (+ `GitHubClientHelpers.cpp`) — implement via the existing `since=` URL builder (REST) / `updated:>=` qualifier (GraphQL search), salient fields. Plus keys-only membership (issue numbers via the `state`-scoped list) + `ProbeIssueExists` (`GET …/issues/{n}`).
7. `Source/Core/src/Tracker/PlaneIssueSearch.cpp` — implement with an `updated_at__gte` param (mirrors the existing `per_page` param pattern). Plus keys-only membership + `ProbeIssueExists` (work-item GET).

**Trigger + monitor flow (AppController + per-pane state)**
8. `Source/Core/include/GridLiveContext.h` *(struct @ :69)* — add `std::chrono::steady_clock::time_point nextChangePollAt{};`, a `changeSinceAnchor` (wall-clock for ISO `since`), and `bool changeBaselineEstablished = false;` (UI-thread-only, same discipline as `syncRetryAfter`).
9. `Source/Core/include/PaneSyncKickPolicy.h` — pure gate `bool ShouldPollForChanges(now, enabled, reachable, focused, syncActive, nextPollAt)` (sibling to `ShouldKickInitialSync`).
10. `Source/Core/src/AppController.cpp` — `TickChangeMonitors()` (called from/after `TickAllContexts`): per context, gate → dispatch worker `Backend->FetchIssuesChangedSince(...)`; on the result (UI thread) build change/added summaries (changelog-primary, cache-diff fallback), patch the changed rows. In the **same cycle** dispatch `FetchIssueKeysForView`, compute `RemovedKeys` vs the pane's cached keys (from its `ActiveTickets` snapshot — no new field), classify each vanished key via `ProbeIssueExists`, drop the row + grid entry, and append Deleted/LeftView summaries. Hand the combined list to the notifier; stamp `nextChangePollAt = now + interval` and advance the `since` anchor.
11. `Source/Core/src/AppController.cpp` (+ thin UI helper) — `FocusTicketInGrid(issueId)`: select the owning pane + row, invoked by a **Notification Center row** action (reuses the grid selection model; **no network**).

**Notification surface (Ui)**
12. `Source/Core/src/SmatchetTicketChangeNotifications.cpp` *(new, sibling of `SmatchetGridNotifications.cpp`)* — change list → one always-summarized toast via `SmatchetToastManager::Instance().Push("Tickets", FormatTicketChangeToast(changes, 1), ToastType::Info, 5000, rowAction)`, deduped on the sorted id-set; the history entry's `rowAction` → `FocusTicketInGrid(primaryChangedId)` (the transient toast's own click opens the Notification Center, handled generically by the manager).
13. `Source/Core/include/Ui/SmatchetToast.h` + `Source/Core/src/Ui/SmatchetToast.cpp` — (a) record a **bounded in-session history** (`ToastHistoryEntry{ CreatedAt, Title, Message, Type, RowAction }`, ring-capped ~200) on every `Push`; (b) a `Push(...)` overload taking an optional `RowAction`; (c) transient-toast **click → request-open the Notification Center** (hit-test rect; keyboard/auto-dismiss preserved); (d) expose `History()` / `ClearHistory()` + an `open-center` request flag the UI polls. Backward-compatible — existing call sites unchanged.
13a. `Source/Core/src/Ui/SmatchetNotificationCenterUi.cpp` (+ `.h`) *(new)* — ImGui window listing `SmatchetToastManager::Instance().History()` **newest-first** (timestamp · type icon · title · message); **row click → `entry.RowAction`**; a *Clear all* button. Visibility on `UiDrawSession`; drawn from `SmatchetUI::Draw`; opened by the manager's open-center flag, a **View-menu** item (`SmatchetUI_MainMenu.cpp`), and the `notifications` command.

**Settings + focus signal (Config / Ui / Standalone)**
14. `Source/Core/include/Config/ConfigManager.h` — `bool TicketChangeMonitorEnabled = true;` + `int TicketChangeMonitorIntervalSec = 120;` on `TrackerConfig`.
15. `Source/Core/src/Config/ConfigManager.cpp` — descriptor rows in `kBoolFields` / `kIntFields`; post-load clamp interval to `[30, 3600]`.
16. `Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp` *(~:400, by the Update-check toggles)* — `Checkbox` + `SliderInt` (interval greyed when off) + `MarkPrefsDirty(d)`; localized labels.
17. `Source/Core/src/AppController.cpp` + `AppController.h` + `Source/Standalone/main.cpp` — `SetWindowFocused(bool)` / `windowFocused_` read by the gate; Standalone sets it from `glfwGetWindowAttrib(window, GLFW_FOCUSED)` (Android already has `hasFocus`; targets without a signal default to `true` → no regression).

**Command surface (in scope, this PR)**
18. `Source/Core/src/Commands/Builtin/BuiltinCommands_*.cpp` — register `tickets.monitor on|off|status` (reads/writes the pref + persists via `ConfigManager::Save`) and `notifications` (open the Notification Center); both surface across CLI / Palette / MCP / Lua via the unified registry. Add the `CLI_GUIDE.md` entries.

**Tests**
19. `tests/Core/TicketChangeDiffPure.test.cpp`, `tests/Core/JqlChangedSincePure.test.cpp` (JQL `(X) AND updated >= -Nm` + ISO `since` formatting), a `ShouldPollForChanges` truth-table test, `tests/Core/TicketsMonitorCommand.test.cpp`, and a per-backend changed-since URL/JQL-composition test (via the existing fake fixtures); register in `tests/CMakeLists.txt`.

## Existing utilities reused

- `AppController::TickAllContexts()` — `Source/Core/src/AppController.cpp:701` — per-frame, budget-bounded per-pane tick; the trigger's host loop.
- Connectivity-probe interval pattern + reachability state — `AppController_Connectivity.cpp:122-155` (`lastTrackerConnectivityState_ == AuthenticatedReachable`) — gating precedent.
- `GitHubClientHelpers` `…/issues?state=all&since=<ISO>` builder — `GitHubClientHelpers.cpp:155` — GitHub change-probe already exists.
- `ProcessJiraSearchPage` / `AppendCachedTicketFromJiraSearchIssue` / `tracker_jql::QuoteLiteral` — `JiraIssueSearch.cpp:80,104,208` — reused for the Jira probe + safe JQL.
- `IsoZuluFromUnixSec` (GitHub helpers) — ISO `since` formatting for GitHub/Plane.
- `CachedTicket` (`id` + `fieldValues` + `fieldRichValues`, `GetFieldValueRef`) — `CachedTicketTypes.h:13-43` — diff input.
- `SmatchetToastManager::Instance().Push(...)` + dedup pattern — `SmatchetGridNotifications.cpp:37-51`.
- `smatchet::ShouldKickInitialSync` (`PaneSyncKickPolicy.h:22`) — header-pure gate precedent.
- `TrackerConfig` + `kBoolFields`/`kIntFields` + `MarkPrefsDirty` — config/prefs add pattern (mirror `UpdateCheckEnabled`).
- `ITrackerIssueReader::FetchIssuesForKeys` (`ITrackerIssueReader.h:56`) — optional full-field refresh of just-changed keys if salient-only patching proves insufficient.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: per-frame cost is one `steady_clock` compare per open pane; each cycle's two requests (change probe — empty when idle — + keys-only reconcile) are off-thread and small; the diff/reconcile run only over the changed/removed keys. No measurable steady-state hit.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no new UI-thread sync I/O — the probe runs on a worker thread; toast push + cache patch are O(changed).
- **Pillar 3 (never crash)**: new logic is pure helpers + a default-fallback virtual; no raw `new`/`delete`; backend-agnostic (no `Jira*`/`Plane*`/`GitHub*` leak into the shared `ITrackerIssueReader` header — the time filter is built inside each concrete client).
- **Pillar 4 (accessibility)**: reuses the existing toast renderer (font-scaling/contrast); the Notification Center is a standard keyboard-navigable ImGui window (also openable via the View menu + `notifications` command), and the focus action is reachable there + via the grid — nothing is mouse-only.

## Perf-review-system gates (diff touches `Source/Core/`)

1. **PR-fast CI** — scenario most directly exercising the changed path: the streaming-sync / concurrent-sync path (`Source/Core/src/Commands/Scenarios/ConcurrentSyncScenario.cpp`); confirm against `agents/core/perf-gatekeeper.md` § Curated diff → scenario map + `scripts/dev/perf-pr-fast-set.json`. The probe is strictly lighter than a full sync.
2. **Pillar 2 static scanner** — **N/A**: no new sync-I/O reachable from `ImGui::*`; the probe is dispatched to a worker.
3. **Dispatcher drain** — **N/A**.
4. **Visible-cue bucket-E harness** — **N/A**: no new >100 ms UI-thread stall.
5. **Marker inventory** — optional `SMATCHET_UI_PERF_SCOPE("AppController::TickChangeMonitors")`; if added, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline against the named scenario before opening the PR.

## Risks / non-goals

- **Heavy polling download** (the concern that reshaped this plan) → **eliminated** by the server-side `updated` filter: idle poll returns no issues; only changed issues (salient fields) come back. Fallback to full fetch only for a backend without a native filter.
- **`updated` minute-granularity / clock skew** (Jira) → relative `updated >= -Nm` (timezone-proof) with the window slightly larger than the interval (e.g. 120 s interval → `-3m`), plus per-issue last-seen-`updated` dedup so overlapping windows don't double-notify.
- **`updated` bumps from non-salient edits** (comments/watchers) → salient-field diff vs cache gates the toast; non-salient bumps are silent.
- **Toast storms** → salient-only filtering + always-summarized output (cap=1) + id-set dedup + first-poll baseline suppression.
- **Toast click + history are new surfaces** (`SmatchetToastManager` has no callback/history today) → additive: an optional `RowAction`, a bounded history ring, and a generic open-center click; existing toast call sites unchanged; focus reuses the grid selection model (no network).
- **Notification Center scope** → session-only, bounded (~200) in-memory history (O(1) per push); cross-restart persistence is a follow-up (no new storage now).
- **Grid freshness** → changed rows are patched on their salient fields; non-salient columns of a changed row may lag until the next full sync (accepted; optional `FetchIssuesForKeys` top-up if needed).
- **Removal reconcile cost** → a second small request per cycle (keys-only, `fields=*none` = id+key) + one `GET /issue/{key}` only on a vanished key. On GitHub issues are rarely deleted (closed/transferred surfaces as left-view) — reflected in the toast wording.
- **Non-goals**: explicit per-ticket watch-list; OS-native desktop notifications; webhook/SSE push; polling hidden panes.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `TicketChangeDiffPure.test.cpp` — salient-field delta detection (status/assignee/priority/summary/due-date/sprint/labels fire; updated-timestamp / watcher-count churn does **not**), always-summarize formatting (cap=1); `JqlChangedSincePure.test.cpp` — `(X) AND updated >= -Nm` composition (+ base-JQL parenthesisation/escaping) and ISO `since` formatting; `JiraChangelogDeltaPure.test.cpp` — salient histories newer than `since`, from→to + author extraction, self-author suppression; `MembershipDiffPure.test.cpp` — `RemovedKeys` set-difference (order-stable, dupes); `ShouldPollForChanges` truth-table (each gate independently blocks); `TicketsMonitorCommand.test.cpp` — `on|off|status`; per-backend changed-since + keys-only URL/JQL composition and `ProbeIssueExists` 404→deleted / 200→left-view classification via fakes; `ToastHistoryRingPure.test.cpp` — bounded cap + newest-first ordering of the toast history.
- **Bucket E (ImGui Test Engine)**: clicking a toast opens the Notification Center, and a center row click focuses the ticket's pane/row (assert selection moved). (Manual smoke: enable monitor, mutate a ticket via a second client → toast → click → center list → row click focuses it; check the slim probe in the network-usage log.)
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; sharpen "salient field", "since anchor / window", "tracked set" against `Source/Core/src/Sync/CONTEXT.md` + the Tracker glossary; record outcome.
- **Manual residue**: the cross-client "real change" smoke is manual-only (no live creds in CI) — add a `docs/self-improvement/categories/test.md` note proposing a deterministic fixture-backend change-injection follow-up.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray "watch/monitor/notify" references and reconcile.

- **Explicit watch-list (star/pin tickets)** — follow-up if active-pane scope is too noisy.
- **OS desktop notifications (backgrounded app)** — follow-up; needs per-platform toast plumbing.
- **Webhook / SSE real-time push** — future ADR (backend-specific; defeats the backend-agnostic poll).
- **Cross-restart notification persistence** — the Notification Center is session-only for now; persisting history across restarts is a follow-up (needs SQLite/config storage).

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
