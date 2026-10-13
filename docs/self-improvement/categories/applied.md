# Agent self-improvement — applied (archive)

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../../../agent-layer/docs/self-improvement/AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Closed entries. Archive moves immediately on Status → applied. Sorted by original surface date, latest first.
> **Bounded head**: this file holds the current + previous month only; older months are
> rotated into flat `applied-YYYY-MM.md` siblings by
> [`rotate-applied-md.sh`](../../../agent-layer/agents/scripts/core/rotate-applied-md.sh)
> (run automatically by `archive-backlog-entry.sh`).
>
> **Deleted-runtime banner (2026-05-21)** — applied entries below that reference the agentic-flow C++ runtime (`AgenticHandoffController`, `AgenticTriageController`, `AgentProposalStore`, `ClaudeCodeLocalRunner`, `PrCommentWatcher`, `PrCheckRunWatcher`, `HarnessRunState`, `CoderabbitCommentClassifier`, `CiFailureClassifier`, `dispatch_source` enum, sentinel-file protocol, `agent/<proposalId>` worktrees, `coderabbit-react-loop` design, `agents/handoff-implementer.md`, `agents/pr-iterator.md`) refer to code that **no longer exists** in the tree. The runtime was removed by v1 PR1 of [`../../plans/shipped/github-tracker-backend.md`](../../plans/shipped/github-tracker-backend.md) (merge sha `b1d241bc`, 2026-05-21). The future [`../../plans/shipped/smatchet-merge-watcher.md`](../../plans/shipped/smatchet-merge-watcher.md) revives a subset of the underlying needs in a different (host-daemon) shape; concepts there are NOT identity-mapped to the deleted runtime. Entries here are preserved as historical record of what was tried.

<!-- Latest first. Append on archival. -->

- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — the HTML and ADF Markdown engines each keep their own copy of the image-span and text-skip logic

Details:
md4c reports an image's alt text as ordinary text events between the image span's enter and leave
callbacks, so both engines collect it instead of emitting it. `MarkdownToHtml.cpp` and
`MarkdownToAdf.cpp` each repeat that logic:
- the `MD_SPAN_IMG` enter (push the src, raise the depth, clear the alt buffer);
- the leave tail (pop the src, clear the alt, lower the depth);
- the text-callback preamble (skip NUL chars, skip and log once on raw HTML under `MD_FLAG_NOHTML`,
  route text inside an image span into the alt buffer).

`dup_audit.py` reports these as clones, exempted by three markers in `MarkdownToHtml.cpp`. The markers
used to say that folding would couple two independent engines. It would not: both engines already
share `MarkdownConvert_Internal.h`, which holds their builder structs and `MdAttrToString`.

Concrete next action:
In `MarkdownConvert_Internal.h`:
- Add a `MdImageSpan` struct (depth, alt, src stack) and give both builders one in place of
  `imgSpanDepth` / `imgAltBuf` / `imgAltAccum` / `imgSrcStack`.
- Add inline helpers to enter an image span, pop its src, leave it, and absorb alt text, plus one for
  the NUL/raw-HTML skip that takes the engine name for its debug log.

Then move both engines onto the helpers and run the MarkdownConvert tests. Delete the three exemptions
(`revisit=2027-08-31`).

Applied 2026-10-03:
- Both builders now derive from `MdEngineState` (code-block depth, `MdImageSpan`, raw-HTML log flag).
- The engines call `EnterImageSpan` / `PopImageSrc` / `LeaveImageSpan`, `MdLinkHref` and
  `ConsumeNonEmittedMdText`, all in `MarkdownConvert_Internal.h`. Text events also build their string
  only after the line-break early returns.
- The three dated exemptions are gone. One `revisit=never` marker remains for the md4c callback
  skeleton (the end of the span switch and the start of the text callback), which both engines
  implement in the same order.
- A differential run of 16 image cases gives byte-identical HTML and ADF before and after. New
  `MarkdownToHtml: images` goldens pin the correct cases.
- The run also exposed three pre-existing alt-text bugs, filed as alexandrosk0/Smatchet#2284: empty
  `<em></em>` before an image, double-escaped entities, and nested images.

Status: applied
Last-reviewed: 2026-10-03

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

# A `SMATCHET_DEVIATION` marker wrapped across comment lines never expires

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-09-30
- **Observed on**: the 2026-10-01 deviation renewal (the markers renewed or resolved alongside this entry)
- **Status**: applied (2026-10-06 — both actions shipped elsewhere; closed by the Batch 26 historical review: #2290 added the absolute `deviation-malformed` rule (`dev_marker_malformed` in `lint-rules.d/00-common.sh`, called from `10-line-rules.sh`), which fails a marker that does not close with rule/reason/owner/revisit on one line, and no first-party marker under `Source/` is wrapped any more (checked 2026-10-06: none lacks `revisit=` on its own line).)

## What happened

`deviation-overdue` reads markers one line at a time (`DEV_RE='SMATCHET_DEVIATION\((.*)\)'` in
`agents/scripts/project/lint-rules.d/00-common.sh`). When a marker's reason wraps onto following
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
