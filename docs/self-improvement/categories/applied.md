# Agent self-improvement — applied (archive)

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Closed entries. Archive moves immediately on Status → applied. Sorted by original surface date, latest first.
> **Bounded head**: this file holds the current + previous month only; older months are
> rotated into flat `applied-YYYY-MM.md` siblings by
> [`rotate-applied-md.sh`](../../../agents/scripts/core/rotate-applied-md.sh)
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
  offline-first sweep (docs/plans/offline-first.md).

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
  it is applied locally. Found by the offline-first sweep (docs/plans/offline-first.md).

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
  projects. Found by the offline-first sweep (docs/plans/offline-first.md).

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
