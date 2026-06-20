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

**Backend parity**: the monitor is backend-agnostic — the same change-probe / attributed-history / membership-reconcile / existence-check design is implemented on Jira, Plane, GitHub, and the **upcoming** Linear backend (plan `linear-tracker-backend`), mirroring Jira's behaviour everywhere possible with a universal cache-diff fallback (see § Per-backend parity).

## Grill outcomes (`grill-with-docs`, 2026-06-20)

Recorded before implementation. These **supersede** any conflicting wording in the body below (the body is corrected inline where it diverged).

**Slice plan (3 PRs — ships incrementally, not one mega-PR):**
- **S1 — change-detection engine**: pure helpers (`TicketChangeDiffPure`, `JqlChangedSincePure`, `MembershipDiffPure`, `JiraChangelogDeltaPure`) + the three `ITrackerIssueReader` virtuals on all backends + trigger/monitor flow in `AppController` + config + the `tickets.monitor` command + bucket-A tests. **Surfaces changes via the existing `SmatchetToastManager::Push`** (plain toasts) — a working monitor with no toast-subsystem changes.
- **S2 — toast-history infra**: `SmatchetToastManager` gains the bounded history ring + the `RowAction` `Push` overload + the open-center click flag (back-compat; existing call sites untouched).
- **S3 — Notification Center**: `SmatchetNotificationCenterUi` window + View-menu item + `notifications` command + bucket-E. Reason: S1 carries the load-bearing value without being gated on the toast rewrite, and the diff stays under the per-PR file ceiling / CR quota.

**Resolved design decisions:**
1. **Role boundary** — change-detection lives on **`ITrackerIssueReader`** (the three defaulted virtuals), reusing the **pure** changelog parsers (`JiraChangelogDeltaPure` + the `*ActivityFeed*Pure` helpers). It is **deliberately NOT** routed through the existing sixth role `ITrackerActivity`: that role is `accountId`/day-window scoped (`FetchUserActivity`), the wrong axis for "which tickets in *this view* changed since anchor T." A 7th role was rejected — these are issue *reads*, which `ITrackerIssueReader` owns. (Future-reader note: yes, `ITrackerActivity` parses changelogs too — but only its *pure* parsers are reused, not the user-scoped role object.)
2. **Reconcile baseline + scope** — the membership baseline is the pane's **in-memory `ActiveTickets` key set**, and the monitor runs **on non-evicted (recently-visible) panes only** (`paneFrameClock_ - lastVisibleFrame` recent). The cache (`tickets_v2`) is **NOT** a valid baseline — it is keyed `PRIMARY KEY(backend_key, id)` with **no view dimension** (`LocalCacheManager.cpp:132`), so diffing against it would flag every *other* view's tickets. A pane whose `ActiveTickets` was LRU-evicted has an empty baseline (→ spurious all-added/all-removed) so it is simply skipped — consistent with the "no polling hidden panes" non-goal.
3. **Disappearance → cache row** — **404 (deleted backend-side)** `DELETE`s the `tickets_v2` + `ticket_field_values_v2` rows + "deleted" toast; **200 (left this view's filter)** **keeps** the cache row (it is per-backend, shared — another view may still match) and only drops the key from *this* pane's `ActiveTickets` + "left view" toast.
4. **Salient fields** — defined as a fixed set of **canonical roles** (Status, Assignee, Priority, Summary, DueDate, Sprint, Labels, Components), each **resolved per-backend to its concrete field id(s)** via the field catalog (`SchemaSystem`) + the per-backend mappers. There is **no `canonicalId` concept in code** — that term was a phantom; the diff compares `CachedTicket.fieldValues` at the resolved ids. Bare `TrackerFieldFamily` is too coarse (every `SelectSingle`/`UserSingle`/`Date` would count).
5. **Since anchor** — **absolute** per-pane `lastChangePollAt` (in-memory on `GridLiveContext`, session-only — no schema change) queried as `updated >= (anchor − 1 interval)` so a skipped/slept poll never drops a change; first poll establishes the baseline **silently**; dedup via a per-pane `seen` set keyed by `(issueKey, changelogEntryId)` (or `field+from+to+ts` when no changelog id).

## Approach

Reuse, don't rebuild — and **push the change-filter to the server** so polling is cheap.

1. **Trigger** — mirror the connectivity-probe interval pattern (`AppController_Connectivity.cpp:122-155`). Add a per-pane `nextChangePollAt` time-point on `GridLiveContext`; in the per-frame `AppController::TickAllContexts()` (`AppController.cpp:701-734`), when the interval elapses *and* the gate passes (enabled · backend reachable · window focused · **pane recently visible / not LRU-evicted** · no sync already active), dispatch a **lightweight change probe** on a worker thread. This is **not** the full streaming sync — off the UI thread, same discipline as a manual refresh, but a much smaller request.

2. **Server-side change query (the key to "lightweight")** — the probe calls a new `ITrackerIssueReader::FetchIssuesChangedSince(cfg, views, window, salientFields)` that pushes the time filter to the backend and requests **only the salient fields**:
   - **Jira**: `(<view JQL>) AND updated >= -Nm` on `/rest/api/3/search/jql` **with `expand=changelog`** (relative units ⇒ timezone-proof). The `updated` filter already returns only the few changed issues, so the changelog rides along just for those — each carries its recent field history (exact from→to + author) and idle polls stay empty. (The codebase already uses `expand=changelog` on this endpoint, `JiraIssueSearch.cpp:320`; if the bulk endpoint ever caps histories, fall back to per-issue `/issue/{key}/changelog` for the changed keys only.)
   - **GitHub**: the existing `…/issues?state=all&since=<ISO>` builder (`GitHubClientHelpers.cpp:155`) / the `updated:>=` GraphQL qualifier.
   - **Plane**: `updated_at__gte=<ISO>` query param (`PlaneIssueSearch.cpp`).
   - **Linear** (upcoming backend, plan `linear-tracker-backend`): GraphQL `issues(filter: { updatedAt: { gt: $since }, …view… })` — native field selection keeps it minimal, and one query can also pull `history` for from→to + actor.
   - **Idle poll ⇒ near-empty response** (zero issues), so steady-state cost is one small request — never a full-view download. A backend without a native filter falls back to the existing full fetch (default impl), so the feature still works everywhere.

3. **Identify the *salient* change, then notify** — an `updated` bump can be a comment/watcher edit, so we pinpoint what actually changed. **Primary (history-based): parse each backend's change history** — Jira `expand=changelog`, Plane issue-activities, Linear issue `history`, GitHub timeline events — for entries newer than the `since` anchor, filtered to salient fields, yielding the exact field, **from→to values, and the author** without needing the old cached value. **Fallback: cache field-diff** (compare the returned issue's salient fields against its cached row) for any backend/field without usable history. Either way we **patch the cache rows so the grid stays consistent**. Self-edits don't notify — the change author is matched against the current user (more robust than cache-diff) and tickets with a pending offline edit are skipped. On enable/startup the `since` anchor is the monitor-start time, so there's no backfill burst. The change list flows to a notifier (sibling of `SmatchetGridNotifications.cpp`) that pushes one **always-summarized**, deduped toast (per-ticket detail only for a lone change), 5 s. Every toast is also recorded in the toast manager's history with a per-entry **row action** (for ticket toasts: focus that ticket's pane + row); the **transient toast's own click opens the Notification Center** (see point 5).

4. **Membership reconcile (removals)** — the change probe sees *presences*, never *absences*: an issue that was deleted, or whose edit moved it *out* of the view (status→Done when the view hides Done; reassigned away), simply stops matching `(<view JQL>)`. So each cycle is paired with a **keys-only** fetch of the view (Jira `…&fields=*none` → id+key only; GitHub/Plane key-only) — tiny even for a full 100-issue view. `removed = pane's cached keys − fetched keys` (baseline = the pane's in-memory `ActiveTickets` key set — see § Grill outcomes #2; **only run on non-evicted, recently-visible panes**, never against the per-backend `tickets_v2` cache). Each vanished key gets one direct `GET /issue/{key}` to classify, with **distinct** cache handling (§ Grill outcomes #3): **404 → deleted** ⇒ `DELETE` the `tickets_v2` + field rows + grid entry, toast `PROJ-123 deleted`; **200 → left the view** ⇒ **keep** the (shared, per-backend) cache row, drop the key only from *this* pane's `ActiveTickets` + grid, toast `PROJ-123 left your view`. New keys in the set corroborate the change probe's additions. (Per user choice the reconcile runs every change cycle.)

5. **Notification Center (general toast history)** — a new window lists *every* toast the app raises (not just ticket changes), **newest-first**, each row showing timestamp · type · title · message. The toast manager gains a **bounded in-session history** (each entry keeps its row action). **Clicking any transient toast opens this window**; **clicking a row invokes that entry's action** — ticket entries → `FocusTicketInGrid`, others are informational. Also openable from the **View menu** + a `notifications` command. Session-only + bounded (~200); cross-restart persistence is a follow-up.

Trade-off named: polling (not webhooks/SSE) is deliberate — backend-agnostic across all three trackers with zero new server-side machinery. "Lightweight" is preserved primarily by the **server-side `updated` filter** (idle ⇒ near-empty), plus gating (focused + reachable + not-already-syncing), the 120 s default, and active-pane-only scope. Each cycle is **two small requests** — the change probe (empty when idle) + the keys-only membership reconcile (small) — plus one `GET /issue/{key}` only when a ticket disappears. Because the probe is *separate from* the full streaming sync, it raises **no sync spinner/toast** (no "refresh flash" every cycle). Real-time push is a future ADR, not this plan.

## Per-backend parity (mirror Jira on all backends)

The monitor is built on the backend-agnostic `ITrackerIssueReader`, so each capability below is implemented per concrete client and behaves the same across trackers, with a universal **cache field-diff fallback** wherever a backend can't do a step natively. Linear is the **upcoming** 4th backend (plan `linear-tracker-backend`); these methods join that backend's reader surface when it lands.

| Capability | Jira | Plane | GitHub | Linear (upcoming) |
|---|---|---|---|---|
| Server-side "changed since" | `(JQL) AND updated >= -Nm` | `updated_at__gte=` | `?since=` REST / `updated:>=` GraphQL | `filter:{ updatedAt:{ gt } }` |
| Attributed from→to + author | `expand=changelog` ✓ | issue **activities** (field/old/new/actor) ✓ | timeline **events** — partial (state/assignee/label) → cache-diff fills the rest | issue **history** (fromState/toState/actor) ✓ |
| Keys-only membership reconcile | `fields=*none` | minimal-field list | `number`-only (GraphQL) / list (REST) | `issues{ nodes{ identifier } }` |
| Existence check (deleted vs left) | `GET /issue/{key}` 200/404 | work-item GET | `GET /issues/{n}` 200/404 | `issue(id){ id }` null |

Only **GitHub** is partial (timeline events cover state/assignee/label transitions but not arbitrary field from→to) — there the cache-diff fallback supplies the rest. The per-backend history parsers already exist in the `*ActivityFeed*` TUs (`JiraActivityFeed` / `PlaneActivityFeedPure` / `GitHubActivityFeed`) and are the reuse point for from→to extraction; Linear adds a small `history`-node parser when its backend is built.

## Files to modify

*Grep-checked: `rg -l 'TicketChangeDiff|JqlChangedSince|FetchIssuesChangedSince|ShouldPollForChanges|nextChangePollAt|TicketChangeMonitor' Source/` returns nothing today — all new symbols below are genuinely new. GitHub's `since=` builder already exists (`GitHubClientHelpers.cpp:155`).*

**Pure logic (Sync — strict zone)**
1. `Source/Core/include/Sync/TicketChangeDiffPure.h` *(new)* — `TicketChangeSummary{ kind (Modified/Added/LeftView/Deleted), issueId, changedFieldLabel, fromValue, toValue, author }` (from→to + author from changelog when available; `FormatTicketChangeToast` wording branches on `kind`), the **salient-field set** — a fixed list of canonical **roles** (Status, Assignee, Priority, Summary, DueDate, Sprint, Labels, Components) each resolved per-backend to its concrete field id(s) via the field catalog (`SchemaSystem`) + per-backend mappers; `IsSalientChangeField(fieldId, resolvedSalientIds)` tests membership over the resolved id set (NOT a `canonicalId` — no such concept exists; see § Grill outcomes #4), `std::vector<TicketChangeSummary> DiffChangedTickets(prev, next)` (salient deltas only), `std::string FormatTicketChangeToast(changes, /*cap=*/1)` (lone change → detailed; ≥2 → "N issues changed: …").
2. `Source/Core/src/Sync/TicketChangeDiffPure.cpp` *(new)* — impl.
3. `Source/Core/include/Sync/JqlChangedSincePure.h` (+ `.cpp`) *(new)* — pure query-fragment builders: `WrapJqlChangedWithin(baseJql, minutes)` → `(<base>) AND updated >= -Nm`; `IsoSinceFromWindow(nowUnix, window)` (wraps `IsoZuluFromUnixSec`) for GitHub/Plane. Unit-testable, no network. Also `Source/Core/include/Sync/MembershipDiffPure.h` *(new)* — `std::vector<std::string> RemovedKeys(cachedKeys, fetchedKeys)` (order-stable set difference) for the removal reconcile.

**Backend probe (Tracker — strict zone)**
4. `Source/Core/include/ITrackerIssueReader.h` *(add)* — `virtual Result<std::vector<CachedTicket>, TrackerError> FetchIssuesChangedSince(const TrackerConfig&, const ViewsStore&, std::chrono::seconds window, const std::vector<std::string>& salientFields)` with a **default that falls back to the full `FetchIssues`** (heavy but correct) so unsupporting backends still work. Also add `FetchIssueKeysForView(cfg, views)` → `Result<std::vector<std::string>, TrackerError>` (keys only, for the membership reconcile; default = full `FetchIssues` + key extraction) and `ProbeIssueExists(cfg, issueKey)` → `Result<bool, TrackerError>` (one `GET /issue/{key}`: true = exists/left-view, false = deleted) for the disappearance classification.
5. `Source/Core/src/Tracker/JiraIssueSearch.cpp` (+ a pure `JiraChangelogDeltaPure` helper) — implement: JQL via `WrapJqlChangedWithin`, `fields=<salient>`, **`expand=changelog`**; parse histories newer than `since`, filter to salient fields → `TicketChangeSummary` (from→to + author); reuse `ProcessJiraSearchPage` + `AppendCachedTicketFromJiraSearchIssue`. Plus `FetchIssueKeysForView` (`…&fields=*none`, token-paginated; minimal-field fallback if `*none` unsupported) and `ProbeIssueExists` (`GET /rest/api/3/issue/{key}` → 200/404).
6. `Source/Core/src/Tracker/GitHubIssueSearch.cpp` (+ `GitHubClientHelpers.cpp`) — implement via the existing `since=` URL builder (REST) / `updated:>=` qualifier (GraphQL search), salient fields. Plus keys-only membership (issue numbers via the `state`-scoped list) + `ProbeIssueExists` (`GET …/issues/{n}`).
7. `Source/Core/src/Tracker/PlaneIssueSearch.cpp` — implement with an `updated_at__gte` param (mirrors the existing `per_page` param pattern). Plus keys-only membership + `ProbeIssueExists` (work-item GET).
7a. **Linear** (upcoming backend, plan `linear-tracker-backend`) — when `LinearClient` lands it implements the same three reader methods over GraphQL: `FetchIssuesChangedSince` = `issues(filter:{ updatedAt:{ gt }, …view… }){ nodes{ …salient… history{ nodes{ … } } } }`; `FetchIssueKeysForView` = `issues(filter){ nodes{ identifier } }`; `ProbeIssueExists` = `issue(id){ id }` (null ⇒ deleted). Add this as a parity item in that plan; **no code here until the backend exists** — the `ITrackerIssueReader` default fallback covers the gap meanwhile.

**Trigger + monitor flow (AppController + per-pane state)**
8. `Source/Core/include/GridLiveContext.h` *(struct @ :69)* — add `std::chrono::steady_clock::time_point nextChangePollAt{};`, a `changeSinceAnchor` (wall-clock for ISO `since`), and `bool changeBaselineEstablished = false;` (UI-thread-only, same discipline as `syncRetryAfter`).
9. `Source/Core/include/PaneSyncKickPolicy.h` — pure gate `bool ShouldPollForChanges(now, enabled, reachable, focused, paneRecentlyVisible, syncActive, nextPollAt)` (sibling to `ShouldKickInitialSync`). The **`paneRecentlyVisible`** term (`paneFrameClock_ - lastVisibleFrame <= K`) is mandatory — an LRU-evicted pane has an empty `ActiveTickets` baseline and must be skipped (§ Grill outcomes #2), not merely gated on app-window focus.
10. `Source/Core/src/AppController.cpp` — `TickChangeMonitors()` (called from/after `TickAllContexts`): per context, gate (incl. `paneRecentlyVisible` — **skip evicted panes**) → dispatch worker `Backend->FetchIssuesChangedSince(...)`; on the result (UI thread) build change/added summaries (changelog-primary, cache-diff fallback), patch the changed rows. In the **same cycle** dispatch `FetchIssueKeysForView`, compute `RemovedKeys` vs the pane's cached keys (from its `ActiveTickets` snapshot — no new field), classify each vanished key via `ProbeIssueExists`, then **branch on the verdict** (§ Grill outcomes #3): 404 → `DELETE` cache row + grid entry (Deleted summary); 200 → keep the shared cache row, drop only this pane's `ActiveTickets`/grid entry (LeftView summary). Hand the combined list to the notifier; stamp `nextChangePollAt = now + interval` and advance the `since` anchor.
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
- Per-backend history parsers `JiraActivityFeed` / `PlaneActivityFeedPure` / `GitHubActivityFeed` (`Source/Core/src/Tracker/*ActivityFeed*`) — reuse for from→to + author extraction (the attributed-change source per § Per-backend parity); Linear adds a `history`-node parser with its backend.

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
- **Plan stress-test — `grill-with-docs`**: ✅ done 2026-06-20 — "salient field", "since anchor", "tracked set" sharpened against the **Tracker** glossary (`Source/Core/src/Tracker/CONTEXT.md`; the earlier `Source/Core/src/Sync/CONTEXT.md` ref was a phantom — that file does not exist) and added there; outcome recorded in § Grill outcomes above.
- **Manual residue**: the cross-client "real change" smoke is manual-only (no live creds in CI) — add a `docs/self-improvement/categories/test.md` note proposing a deterministic fixture-backend change-injection follow-up.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray "watch/monitor/notify" references and reconcile.

- **Explicit watch-list (star/pin tickets)** — follow-up if active-pane scope is too noisy.
- **OS desktop notifications (backgrounded app)** — follow-up; needs per-platform toast plumbing.
- **Webhook / SSE real-time push** — future ADR (backend-specific; defeats the backend-agnostic poll).
- **Cross-restart notification persistence** — the Notification Center is session-only for now; persisting history across restarts is a follow-up (needs SQLite/config storage).

## Implementation log
*(populated per-slice as sub-PRs land)*

### S1a — backend-agnostic pure diff/query helpers (2026-06-20)
The S1 change-detection engine ships as three sub-PRs (S1a/S1b/S1c) rather than the single S1 PR in § Approach, so the fully-decoupled pure layer lands and bakes first and each diff stays inside the CR file ceiling. S1a is that pure layer — no backend, no pane, no AppController wiring:

- `Source/Core/include/Sync/TicketChangeDiffPure.{h,cpp}` *(new)* — `SalientFieldRole`, `TicketChangeKind`, `TicketChangeSummary`, `IsSalientChangeField`, `DiffChangedTickets`, `FormatTicketChangeToast`.
- `Source/Core/include/Sync/JqlChangedSincePure.{h,cpp}` *(new)* — `WrapJqlChangedWithin`, `IsoSinceFromWindow`.
- `Source/Core/include/Sync/MembershipDiffPure.h` *(new, header-only)* — `RemovedKeys` + `AddedKeys`.
- `tests/Core/{TicketChangeDiffPure,JqlChangedSincePure,MembershipDiffPure}.test.cpp` *(new)* — 35 doctest cases / 71 assertions (bucket A), all green; registered in `tests/CMakeLists.txt`.

S1b (backend reader virtuals + `JiraChangelogDeltaPure`) and S1c (AppController trigger + config + commands) follow.

### S1b — backend reader surface + Jira changelog-delta parser (2026-06-20)
The change-detection reader surface and the Jira-private attributed-delta parser, still decoupled from any pane / AppController wiring. The concrete Jira network override (real `expand=changelog` HTTP) is deliberately deferred to a later slice — it needs live `JiraIssueSearch` plumbing plus fixture tests and would break this slice's purity + file ceiling:

- `Source/Core/include/ITrackerIssueReader.h` *(modified)* — three defaulted virtuals + `<chrono>`: `FetchIssuesChangedSince(cfg, views, window, salientFields)`, `FetchIssueKeysForView(cfg, views)`, `ProbeIssueExists(cfg, issueKey)`. Each ships a safe default over the existing `FetchIssues` surface so every backend inherits working behaviour with no concrete override (build stays green); `ProbeIssueExists` defaults to `Ok(true)` (conservative — assume the issue still exists, so a non-probing backend reconciles to LeftView, never a destructive Deleted).
- `Source/Core/src/Tracker/JiraChangelogDeltaPure.{h,cpp}` *(new)* — `DeltasFromIssueJson(issue, sinceIsoMinute, roster, selfAccountId)` maps one issue's `expand=changelog` JSON to attributed salient `TicketChangeSummary` deltas (kind=Modified, roster-label, display-preferred from→to, author), each carrying `changelogEntryId` + `createdAt` for the monitor's per-pane `(issueKey, changelogEntryId)` seen-set. Pure: only nlohmann/json + the header-only `TicketChangeDiffPure` structs — no JiraClient/cpr, so it links in the pure test block.
- `tests/Core/JiraChangelogDeltaPure.test.cpp` *(new)* — 5 doctest cases / 24 assertions (bucket A), all green; registered in `tests/CMakeLists.txt`.

S1c (AppController trigger + config + commands) follows. The concrete Jira `FetchIssuesChangedSince` override that calls `DeltasFromIssueJson` over live `expand=changelog` HTTP is a deferred sub-slice after S1c.

## Deviations from plan
*(populated per-slice as sub-PRs land)*

### S1a
- **S1 split into S1a/S1b/S1c sub-PRs.** § Approach nominally ships the change-detection engine as one S1 PR; it is split so the fully-decoupled pure helpers land first and each diff stays under the CR file ceiling. S1a = pure layer only.
- **`SalientFieldRole{fieldId,label}` roster replaces the plan's flat `resolvedSalientIds`.** § Files-to-modify #1 typed the salient set as a bare id list with `IsSalientChangeField(fieldId, resolvedSalientIds)`. S1a carries `{fieldId, label}` pairs so the toast shows the canonical role label ("Status") rather than the raw backend id ("customfield_10020"); `IsSalientChangeField` / `DiffChangedTickets` take the roster. No `canonicalId` namespace is introduced — the label is display-only, so § Grill outcomes #4 still holds.
- **`DiffChangedTickets` takes the roster argument.** The plan signature `DiffChangedTickets(prev, next)` omitted the salient set; the impl needs it to know which fields are salient and their labels, so the shipped signature is `DiffChangedTickets(prev, next, roster)`.
- **`MembershipDiffPure` adds `AddedKeys` alongside `RemovedKeys`.** § Files-to-modify #3 listed only `RemovedKeys`; the symmetric `AddedKeys` feeds the `TicketChangeKind::Added` summaries § Approach already calls for.
- **`IsoSinceFromWindow` is self-contained** (Hinnant civil-from-days) rather than wrapping `IsoZuluFromUnixSec` as § Files-to-modify #3 suggested, keeping the pure helper free of any GitHub-client dependency so it builds in the bare test rig. A later slice may swap to the shared formatter.

### S1b
- **Detection rides `ITrackerIssueReader`, not a new role.** § Grill outcomes #1 — three defaulted virtuals on the existing reader interface reuse the pure changelog parser; no `ITrackerActivity` dependency and no 7th backend role.
- **Concrete Jira `expand=changelog` HTTP override deferred to a post-S1c sub-slice.** § Files-to-modify #5 nominally lands the Jira network override alongside `JiraChangelogDeltaPure` in S1b. It is split out: the parser is pure + bucket-A tested now, while the live `JiraIssueSearch` wiring (which needs fixture/integration tests and pulls cpr into the slice) lands after S1c. The defaulted virtual keeps every backend building until then.
- **`JiraChangelogDelta` wraps `TicketChangeSummary` rather than emitting it bare.** The parser returns `{summary, changelogEntryId, createdAt}` so the seen-set dedup key `(issueKey, changelogEntryId)` and the since-window compare have the changelog metadata the backend-agnostic `TicketChangeSummary` does not carry.
- **`ProbeIssueExists(cfg, issueKey)` default is `Ok(true)`.** § Grill outcomes #3 distinguishes 404→Deleted from 200→LeftView; a backend that cannot probe defaults to "still exists" so reconcile picks the non-destructive LeftView path and never deletes cache rows on a probe it could not actually perform.

## Verification (actual)
*(populated per-slice as sub-PRs land)*

### S1a
- **Bucket A**: 35 doctest cases / 71 assertions across the three new `tests/Core/*.test.cpp`, all green (`ninja-test-msvc`). clang-format clean; lint gate `agents/scripts/project/test-lint-rules.sh --diff origin/develop` PASS (no new strict-zone or comment-noise violations).
- **Dual-target build**: deferred to CI's MSVC + DX12 lanes. S1a is pure C++14 (no platform / UI / HTTP / ImGui headers) and compiled clean in the test rig; the cold standalone+DX12 local build was skipped per the build/test-cadence rule for a pure-helper slice — CI's dual-target lanes gate the PR.

### S1b
- **Bucket A**: 5 doctest cases / 24 assertions in `tests/Core/JiraChangelogDeltaPure.test.cpp` — salient-roster filter (non-salient items dropped), display-preferred from→to + raw fallback + `field`/`fieldId` match, self-author suppression, minute-prefix since-window, malformed/missing input — all green (`ninja-test-msvc`, `--source-file=*JiraChangelogDeltaPure*`). Lint gate `agents/scripts/project/test-lint-rules.sh --diff origin/develop` PASS (all 9 checks; no new strict-zone or comment-noise violations).
- **Dual-target build**: deferred to CI's MSVC + DX12 lanes, same rationale as S1a. The three defaulted virtuals are header-only over the existing `FetchIssues` surface and `JiraChangelogDeltaPure` is pure (nlohmann/json + header-only structs only); both compiled clean in the test rig.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
