# Agent self-improvement — applied (archive partition 2026-09)

> Rotated slice of [`applied.md`](applied.md) (see its header for format /
> categories / workflow). Entries whose original surface date falls in
> 2026-09, sorted latest first. Append-only via
> `agents/scripts/core/rotate-applied-md.sh`; do not file new work here.
>
> **Deleted-runtime banner (2026-05-21)** — entries below that reference the
> agentic-flow C++ runtime (`AgenticHandoffController`, `AgenticTriageController`,
> `AgentProposalStore`, `ClaudeCodeLocalRunner`, `PrCommentWatcher`,
> `PrCheckRunWatcher`, `HarnessRunState`, `CoderabbitCommentClassifier`,
> `CiFailureClassifier`, the `dispatch_source` enum, the sentinel-file protocol,
> `agent/<proposalId>` worktrees, the `coderabbit-react-loop` design,
> `agents/handoff-implementer.md`, `agents/pr-iterator.md`) refer to code
> removed 2026-05-21 (v1 PR1 of `../../plans/shipped/github-tracker-backend.md`,
> merge sha `b1d241bc`). Preserved as historical record of what was tried.

<!-- Latest first. Appended by rotate-applied-md.sh only. -->

# A `SMATCHET_DEVIATION` marker wrapped across comment lines never expires

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-09-30
- **Observed on**: the 2026-10-01 deviation renewal (the markers renewed or resolved alongside this entry)
- **Status**: open

## What happened

`deviation-overdue` reads markers one line at a time (`DEV_RE='SMATCHET_DEVIATION\((.*)\)'` in
`agent-layer/agents/scripts/project/lint-rules.d/00-common.sh`). When a marker's reason wraps onto following
comment lines, the `revisit=` field sits on a continuation line the rule never parses, so the marker
never expires: the fail-open direction. Eleven wrapped markers dated 2026-09-30 / 2026-10-01 were
past due without any gate noticing; the renewal rewrote them as single-line markers. Wrapped markers
with later dates remain in `Source/` (for example the backend-client headers under
`Source/Core/include/Tracker/`, `OllamaClient.cpp`, `OpenAiClient.cpp`). `dup_audit.py` has the same
per-line reading, so a wrapped marker may also fail to suppress the clone it was written for; its
`_ineffective_dup_deviation` diagnostic already names that cause.

Most of these markers were wrapped by clang-format before `CommentPragmas: '^ *SMATCHET_DEVIATION'`
protected them.

## Concrete next action

1. Rewrite the remaining wrapped markers as single-line markers, with the explanation kept as plain
   comment prose above them.
2. Make the grammar fail closed: in `scan_file_rules`, a comment line that contains
   `SMATCHET_DEVIATION(` but no closing `)` emits `deviation-overdue` ("marker must be one line"),
   like an empty `revisit=`. Add a `--selftest` case and a bats case.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — deviation-malformed (absolute-0) fails a SMATCHET_DEVIATION marker that is not one whole line with rule/reason/owner/revisit (lint-rules.d/10-line-rules.sh + 00-common.sh helpers, fixture tests/fixtures/lint_rules/deviation-malformed.cpp), so a wrapped marker can no longer dodge expiry.
  Status: applied (2026-10-04)

# A `SMATCHET_DEVIATION` marker wrapped across comment lines never expires

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-09-30
- **Observed on**: the 2026-10-01 deviation renewal (the markers renewed or resolved alongside this entry)
- **Status**: applied (2026-10-06 — both actions shipped elsewhere; closed by the Batch 26 historical review: #2290 added the absolute `deviation-malformed` rule (`dev_marker_malformed` in `lint-rules.d/00-common.sh`, called from `10-line-rules.sh`), which fails a marker that does not close with rule/reason/owner/revisit on one line, and no first-party marker under `Source/` is wrapped any more (checked 2026-10-06: none lacks `revisit=` on its own line).)

## What happened

`deviation-overdue` reads markers one line at a time (`DEV_RE='SMATCHET_DEVIATION\((.*)\)'` in
`agent-layer/agents/scripts/project/lint-rules.d/00-common.sh`). When a marker's reason wraps onto following
comment lines, the `revisit=` field sits on a continuation line the rule never parses, so the marker
never expires: the fail-open direction. Eleven wrapped markers dated 2026-09-30 / 2026-10-01 were
past due without any gate noticing; the renewal rewrote them as single-line markers. Wrapped markers
with later dates remain in `Source/` (for example the backend-client headers under
`Source/Core/include/Tracker/`, `OllamaClient.cpp`, `OpenAiClient.cpp`). `dup_audit.py` has the same
per-line reading, so a wrapped marker may also fail to suppress the clone it was written for; its
`_ineffective_dup_deviation` diagnostic already names that cause.

Most of these markers were wrapped by clang-format before `CommentPragmas: '^ *SMATCHET_DEVIATION'`
protected them.

## Concrete next action

1. Rewrite the remaining wrapped markers as single-line markers, with the explanation kept as plain
   comment prose above them.
2. Make the grammar fail closed: in `scan_file_rules`, a comment line that contains
   `SMATCHET_DEVIATION(` but no closing `)` emits `deviation-overdue` ("marker must be one line"),
   like an empty `revisit=`. Add a `--selftest` case and a bats case.

- 2026-09-29 · offline-first calibration (S13) · [debt] · P3 — the bulk-import hydration prefetch retries a failed fetch every frame

  Details: `Ui/SmatchetBulkTicketsUi.cpp` calls `app.PrefetchIssueTicketsForKeys(keysNeedingHydration)`
  on every frame the Bulk Import panel draws. `AppController::PrefetchIssueTicketsFrom` only
  de-duplicates keys that are still in flight. When a fetch fails, its keys are released and the next
  frame launches the same fetch again, with no backoff. While the tracker is reachable but failing
  (5xx, rate limit), that is a continuous stream of doomed requests for as long as the panel is open.
  Since S13 the prefetch is skipped while the tracker is known offline, so the offline case is covered.
  Found by the offline-first calibration review (`offline-network-read-ungated` hit on
  `AppController_TicketPrefetch.cpp`).

  Concrete next action: route the prefetch through a `KeyedLookupCache` keyed by
  (backend key, issue key), or keep a per-key retry-after next to `bulkImportPrefetchKeysInFlight_`.
  After a failure the key then waits out `kLookupRetryAfterSeconds`, and `OnConnectivityRecovered`
  clears the wait.
  Status: applied (offline-first follow-ups) — a failed prefetch holds its (backend key, issue key) entries back for `kLookupRetryAfterSeconds`; a connectivity recovery clears every backoff
  Last-reviewed: 2026-10-05

- 2026-09-24 · offline-first sweep · [debt] · P2 — the offline-queue counts run a SQLite SELECT on the UI thread every frame

  Details: `Ui/SmatchetUI.cpp` sets `d.cachedPendingFieldEditCount` from
  `app.GetPendingFieldEdits().size()` once per frame. `OfflineQueueService::GetPendingFieldEdits`
  loads every row through `LocalCacheManager::LoadPendingFieldEdits`, only to count them. The status
  bar (`Ui/SmatchetStatusBarUi.cpp`) does the same for creates: `app.GetPendingCreateCount()` loads
  every pending create through `LoadPendingCreates()` and returns `.size()`. Both reads are synchronous
  SQLite I/O on the render path (Quality Pillar 2). They are cheap while the queue is empty, but they
  grow with the queue, which is exactly the offline case the queue exists for. Found by the
  offline-first sweep (docs/plans/shipped/offline-first.md).

  Concrete next action: keep both counts in `OfflineQueueService` as atomics. Update them on the worker
  after every enqueue, replay, archive and restore (the same places that already touch the tables).
  The UI then reads the atomics and never queries SQLite in a frame.
  Status: applied (offline-first follow-ups) — OfflineQueueService and PendingActionQueueService publish an in-memory snapshot of their tables (`Sync/PublishedSnapshot.h`), rebuilt on a worker after every change; the status bar and the Offline Queue panel read it and never query SQLite in a frame
  Last-reviewed: 2026-10-05

- 2026-09-24 · offline-first sweep · [debt] · P2 — `AppController::UpdateTicket` writes the ticket to SQLite on the UI thread

  Details: `AppController::UpdateTicket` (`AppController_CatalogAndFieldEdit.cpp`) calls
  `Cache->SaveTicket(capturedKey, ticket)` inline, and its callers apply field-edit results on the
  UI thread (`ApplyFieldEditResult` through the grid pipeline's main-thread post-back). The write runs
  inside `RunWriteTxnWithBusyRetry`, whose busy-retry deadline can hold the frame, so a contended
  cache stalls the UI (Quality Pillar 2). Every queued offline edit also passes through this path when
  it is applied locally. Found by the offline-first sweep (docs/plans/shipped/offline-first.md).

  Concrete next action: post the `SaveTicket` call to a worker. Keep the key-and-generation latch
  that precedes it (issue #1081), and pass the latched key into the worker so the write still lands
  under the captured backend. The in-memory ticket update and `RefreshLocalData()` stay on the UI
  thread.
  Status: applied (offline-first follow-ups) — UpdateTicket patches the pane's row in memory on the UI thread and queues the SQLite write plus the grid re-read on a worker (`SerialTaskQueue.h`), in call order; only the latest queued save for a pane re-reads the grid
  Last-reviewed: 2026-10-05

- 2026-09-24 · offline-first sweep · [debt] · P2 — the project picker reads and parses the catalog-cache index every frame while its combo is open

  Details: `Ui/SmatchetProjectPicker.cpp` `DrawRecentSection` calls
  `FieldCatalogCache::ListCachedProjects()` on every frame the picker combo is open. That call reads
  the field-catalog cache from disk and parses its JSON, so an open picker does synchronous file I/O
  plus a parse on the render path (Quality Pillar 2). The cost grows with the number of cached
  projects. Found by the offline-first sweep (docs/plans/shipped/offline-first.md).

  Concrete next action: load the list once when the popup opens, on a worker, and keep it in the
  picker's state until the popup closes. Show the previous list, or an empty list, while that load
  runs.
  Status: applied (offline-first follow-ups) — the picker reads the recently used projects on a worker once per popup open (`SmatchetProjectPicker::StartRecentProjectsLoad`) and draws the rows it last read
  Last-reviewed: 2026-10-05

# An orphaned plan-lock has no direct write path for a session-scoped token — release it through a closing PR's `lock-slug:` line instead

- **Category**: process
- **Priority**: P3
- **Date**: 2026-09-10
- **Observed on**: Issue #2182 (`refs/locks/fa-fetch-raw-host`), alongside #2183/`pillar2-shutdown-flush` (PR #2201's session) and #2198's `parent-issue-hierarchy` (same session)
- **Status**: applied (this PR)

  Concrete next action: route the prefetch through a `KeyedLookupCache` keyed by
  (backend key, issue key), or keep a per-key retry-after next to `bulkImportPrefetchKeysInFlight_`.
  After a failure the key then waits out `kLookupRetryAfterSeconds`, and `OnConnectivityRecovered`
  clears the wait.
  Status: applied (offline-first follow-ups) — a failed prefetch holds its (backend key, issue key) entries back for `kLookupRetryAfterSeconds`; a connectivity recovery clears every backoff
  Last-reviewed: 2026-10-05

# An orphaned plan-lock has no direct write path for a session-scoped token — release it through a closing PR's `lock-slug:` line instead

- **Category**: process
- **Priority**: P3
- **Date**: 2026-09-10
- **Observed on**: Issue #2182 (`refs/locks/fa-fetch-raw-host`), alongside #2183/`pillar2-shutdown-flush` (PR #2201's session) and #2198's `parent-issue-hierarchy` (same session)
- **Status**: applied (PR #2211 released the lock; the recipe was codified in `docs/agent-rules/ship-loops.md` (Release wiring) by the Batch 26 historical review, 2026-10-06)

## What happened

`refs/locks/fa-fetch-raw-host` was claimed 2026-08-17 by branch
`fix/fa-fetch-raw-host` for a one-line fix to
`.github/actions/fetch-fontawesome/action.yml`. The fix landed the next day —
folded into unrelated PR #2119 rather than shipped from the locked branch —
so `fix/fa-fetch-raw-host` was deleted with no PR ever pointing back at the
`fa-fetch-raw-host` slug. `lock-staleness.yml` flagged it 23 days later as
Issue #2182.

## Why the obvious fixes don't apply

- `lock-cleanup.yml` only fires on `pull_request: closed` and only releases a
  ref named by a `lock-slug:` line in *that* PR's body. A lock whose branch
  never became a PR — or whose fix shipped under an unrelated PR, as here —
  is orphaned forever by that path alone.
- `lock-release.sh` pushes a ref delete straight to `refs/locks/*`, which
  needs git credentials with write access to that namespace. A
  session-scoped `GITHUB_TOKEN` / CCR credential does not have it —
  confirmed by a repeatable HTTP 403 chasing the same problem for
  `parent-issue-hierarchy` (#2198's lock), released only because the repo
  owner ran the push locally with `SMATCHET_ALLOW_MERGED_PR_PUSH=1`.

## The fix that generalizes

`lock-cleanup.yml` releases on **any** PR close, merged or abandoned — so a
PR that carries `lock-slug: <slug>` in its body releases the ref the moment
it closes, whether or not its own diff touches the locked write-set at all.
PR #2195 used exactly this to release `pillar2-shutdown-flush` for #2183;
this PR does the same for `fa-fetch-raw-host` — the diff is this note, and
the ref is released by the `lock-slug:` line in the PR body, not by the
note's content.

Any future orphaned-lock Issue where the branch is gone and no PR names the
slug can be closed the same way: open (and merge or close) a PR whose body
carries `lock-slug: <slug>`. No push access to `refs/locks/*` required.
  Resolution: verified-in-tree 2026-10-04 (backlog-sweep-2026-10) — lock-staleness-sweep.sh's stale-lock Issue body offers the lock-slug PR path for agents without ref-write access; the sweep also gains a workflow_dispatch release path in this sweep.
  Status: applied (2026-10-04)

- 2026-09-07 · code-review · [debt] · P2 — worker-side missing-parent fetch gates on the ACTIVE view; the grid projection gates per pane

  Details: `TicketSyncService::FetchMissingParentsIntoQueue` decides whether to skip the keyed parent fetch
  by reading `ConfigManager::FindActiveViewOrFirst(viewsCopy.Views, viewsCopy.ActiveViewId)`'s `HideParents`
  and the global `TrackerConfig::LoadParentIssues` — one decision for the whole sync. But the grid's row
  projection (`SmatchetActiveProjectGridTable.cpp`, `HierarchyOptionsForView(activeViewForGrid)`) reads
  the hierarchy options **per pane** (ADR-0018 multi-pane views). A background pane whose own view wants
  Story group / does not hide parents gets no parent top-up whenever the globally-active view happens to
  hide parents — the two switches read different scopes for the same feature.
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md (PR #2198).

  Concrete next action: either (a) make the worker-side gate check every open pane's view (fetch parents
  if ANY pane wants them), or (b) document the active-view-only scope as an intentional simplification in
  docs/guides/parent-hierarchy.md and drop the cost only when literally no pane could use the result.
  Enumerator: `g_ui.gridPanes` (or the pane-view resolver used by `HierarchyOptionsForView` callers) is the
  per-pane view source of truth to reconcile against.
  Resolution: not a defect, closed 2026-10-04 (backlog-sweep-2026-10) — every sync is pane-scoped: pane-kicked syncs re-point views.ActiveViewId to the pane's own view before SyncPaneWithBackend, and the focused pane syncs against the active view, so FetchMissingParentsIntoQueue resolves each pane's own HideParents.
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04

# Re-running a body-reading gate replays a frozen event payload — and the rollup dedup turns that into a fresh red

- **Category**: process
- **Priority**: P2
- **Date**: 2026-09-07
- **Observed on**: PR #2180 (`claude/agent-layer-a1-seam`) — two `gh run rerun` invocations against run `34139417600`, the second landing a terminal `FAILURE` at `17:03:05Z` where the head had previously carried only a stale `CANCELLED`
- **Status**: open

## What happened

`Intent section` was red on #2180's head. The reflex remedy — the one `merge-gates.md` documents —
is `gh run rerun <id>`. It was issued twice. Neither run could ever have passed, and the second one
made the gate strictly worse than before it ran.

The re-trigger that actually worked was a **PR body edit**, which produced run `34147012343`,
`SUCCESS` at `17:17:52Z`.

## The mechanics, source-read

1. `.github/workflows/doc-validation.yml:536` passes the body into the check as
   `PR_BODY: ${{ github.event.pull_request.body }}` — the body **as it stood in the event payload
   that started the run**, not the body as it stands now. `PR_HEAD_SHA` at line 537 comes from the
   same frozen payload.
2. `gh run rerun` re-executes a run against its **original** payload. So a rerun of a `synchronize`
   run re-reads the body from the moment of that push. If the body was wrong then — a missing
   `## Intent`, or a `head=` that no longer matches after a later push — it is still wrong on every
   rerun, forever. The condition being waited on is immutable, so the retry can never converge.
3. The workflow already knows this and says so, at `doc-validation.yml:73-79`: `edited` is in the
   trigger list precisely so "a PR that adds or fixes its `## Intent` section via a body edit
   re-runs the `Intent section` job and self-heals the stale-red — no wasted empty-commit push".
   The comment documents the cure. `merge-gates.md`'s 405-recovery recipe has also warned against
   the reflex since #2120 (2026-08-18): "do not re-run a job whose result depends on the PR body
   (`Intent section`)". The halt-code 8 row, which is where an agent reading a BLOCK lands first,
   does not.
4. The rerun is not merely futile — it is **actively regressive**. `merge-gates.sh:30-32`:
   > Rollup dedup: required CheckRuns with the same `.name` are deduped to the entry with the latest
   > `.startedAt` so stale FAILUREs from rerun jobs don't falsely block.
   Latest-`startedAt`-wins is the right rule for its stated purpose, but it cuts both ways: a rerun
   always carries the newest `startedAt`, so a rerun that fails **displaces whatever was there
   before**. On #2180 the pre-rerun state was a `CANCELLED` entry that blocked nothing; the post-rerun
   state was a terminal `FAILURE` that blocked everything. The remedy manufactured the block it was
   invoked to clear.

## The class, and how small it is

Exactly **one** PR-gating workflow in this repo reads a frozen payload field as its subject:

```
grep -rln 'github.event.pull_request.body\|github.event.pull_request.title' .github/workflows/
  .github/workflows/doc-validation.yml     # pull_request: [opened, synchronize, reopened, edited]
  .github/workflows/lock-cleanup.yml       # pull_request: [closed] — not a gate
```

That is the whole population today. The bound is what makes a gate cheap; the fact that the
population is one is also why the trap has never been written down.

## Why the existing docs point the wrong way

[`merge-gates.md`](../../../agent-layer/docs/agent-rules/merge-gates.md) is where an agent looks when a check is red,
and it prescribes `gh run rerun` in two places:

- the halt-code 8 row (*Cancelled-while-pending*, in the halt-prompt return-code table): "Rerun the
  named run(s) (`gh run rerun <id>` from the BLOCK output), then re-poll". **Unscoped**: nothing there
  says "unless the workflow's subject is the event payload".
- the 405-recovery recipe: `gh run rerun <run-id>  # the run whose job is CANCELLED, not the newer
  one`, followed by "No push, no force, no PR-body re-pin". This one IS scoped: a few lines below it
  says not to re-run a body-reading job (`Intent section`) and to edit the body instead (#2120).

Both reruns are correct **for the concurrency-collapse case they were written for**, where the run
never executed and the payload is irrelevant. Only the halt-code 8 row is missing the carve-out.

## Concrete next action

1. **Scope the rerun remedy in the halt-code 8 row.** The 405-recovery recipe already carries the
   carve-out (#2120); add the same one line to the halt-code 8 row of `merge-gates.md`'s halt-prompt
   return-code table: a rerun cannot fix a check whose input is the event payload
   (`github.event.pull_request.*`); for those, edit the PR body to fire the `edited` trigger.
   Enumerator: the `grep -rln` above — keep the carve-out keyed on that command, not on a hardcoded
   workflow name, so it stays true when the population grows.
2. **Make the workflow say it at the point of temptation.** The `doc-validation.yml:73-79` comment
   explains why `edited` exists; extend it with the inverse — that `gh run rerun` replays the stale
   body and can never pass. An agent reading the failing workflow should not have to find the
   remedy doc to learn the remedy is wrong here.
3. **Assert the property rather than the instance**, in `tests/bats/workflow_job_mask.bats` (the
   existing workflow-YAML-shape suite): every `pull_request`-triggered workflow that references
   `github.event.pull_request.body` or `.title` MUST list `edited` in its `types:`. Without `edited`
   such a gate has *no* re-trigger at all short of a new commit — the failure mode one config edit
   away from today's, and the one a name-based carve-out would not catch.
4. **Record the dedup's second edge.** The `merge-gates.sh:30-32` comment justifies latest-wins in
   one direction only ("stale FAILUREs … don't falsely block"). Note the other: a rerun's fresh
   FAILURE displaces an older SUCCESS or a harmless CANCELLED. The rule is still right; a reader
   deciding whether to rerun needs to know it is not free.

Triggered-follow-up: when=pr-count:base=develop;since=2026-09-07;n=20; action=re-check whether the rerun carve-out landed in merge-gates.md, whether any new workflow reads `github.event.pull_request.body` without an `edited` trigger, and whether another rerun-induced FAILURE displaced a green; baseline=1 rerun-manufactured FAILURE on PR #2180 and 1 payload-reading gate repo-wide, 2026-09-07; fired=2026-10-05 (carve-out landed in merge-gates.md halt-code-8 row + 405 recipe; tests/bats/workflow_event_payload.bats finds no payload-reading workflow without `edited`; rerun-displaced-green incidence not checked — needs run history)
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — (1) carve-out in merge-gates.md at the halt-code-8 row and the 405 recipe; (2) the inverse warning in doc-validation.yml's edited comment; (3) tests/bats/workflow_event_payload.bats asserts every pull_request workflow reading the body/title lists edited (closed-only exempt); (4) the dedup's second edge is documented in merge-gates.sh.
  Status: applied (2026-10-04)

- 2026-09-07 · code-review · [tooling] · P2 — `SmatchetLocalization::T(key, fallback)` returns the call-site fallback for en-US, not the table's English column — editing one without the other is a silent no-op

  Details: `TranslateEntryLocked` (`Source/Core/src/SmatchetLocalization.cpp`) returns
  `fallback ? fallback : entry.English` for the active en-US language — the `kEntries` table's English
  column is used only as an *overrides lookup key* (`overrides.find(entry.English)`), never rendered
  directly. A diff that edits only `kEntries`'s English string (e.g. renaming a label) changes nothing
  user-visible in English; the call-site `T("key", "...")` literal is what actually renders. This bit twice
  in one session on the same PR (#2198): the author renamed "Story group" -> "Parent group" by editing only
  the table row, and a review pass initially read the table as the source of truth too before tracing the
  function. The mismatch also silently broke the feature's only bucket-E UI-test coverage — the test's item
  ref matched the (unwritten) new label, so `ClickSortByCheckbox` never resolved it.
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md.

  Concrete next action: document the fallback-wins-for-en-US contract in `docs/agent-rules/cpp-rules.md`
  § Conventions (near any existing localization guidance), and add a cheap delta-gated check: for every
  changed `kEntries` row `{"<key>", "<english>", ...}` in `SmatchetLocalization.cpp`, grep the repo for
  `T("<key>"` call sites and flag if none of their literal fallback strings match the new `<english>` value.
  Enumerator: the `kEntries` array in `Source/Core/src/SmatchetLocalization.cpp`; rule id sketch
  `localization-english-fallback-drift`.
  Resolution: applied 2026-10-05 (backlog-sweep-2026-10, PR #2296) — docs/agent-rules/cpp-rules.md § Localization states the fallback-wins rule; agents/scripts/project/test-localization-drift.sh (WARN-first; --diff/--all/--strict/--selftest; C-escape aware) compares call-site fallbacks with the table's English column. Whole-tree: 380 call sites, 3 drifts, all real (toast.reverted_layout, cmdpalette.destructive_hint, whisper.preferences.testConnection.failure) — product text, so filed as GitHub Issue #2305 rather than changed here (visual-validation rule).
  Status: applied (2026-10-04; was: open)
  Last-reviewed: 2026-10-04
