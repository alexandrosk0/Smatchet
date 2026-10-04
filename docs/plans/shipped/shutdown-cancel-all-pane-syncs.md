# Plan — cancel every pane's streaming sync before joining any
<!-- plan-date: 2026-10-04 -->

> **Slug**: `shutdown-cancel-all-pane-syncs`
>
> **Status**: `shipped`

## Context

`~AppController` cancelled and joined only `focusedContext().ticketSync_`. Since #2286 added a
joining `~TicketSyncService`, every other pane's busy worker is joined during `gridContexts_`
member destruction, after `impl_` is torn down, one pane at a time. A worker that has not been
flagged keeps paging while an earlier pane's join waits, so shutdown can cost one in-flight HTTP
timeout per busy pane. `RecreateLocalCacheDatabase` had the same serial cancel-then-join loop.

## Approach

Two passes: flag every service Cancelled/Superseded, then join each. Every worker unwinds
concurrently, so teardown is bounded by about one in-flight request.

## Files to modify

| File | Change |
|---|---|
| `Source/Core/include/Sync/TicketSyncService.h` / `src/Sync/TicketSyncService.cpp` | `RequestCancel()` (non-blocking) + `static CancelAndJoinAll(const std::vector<TicketSyncService*>&)` (null/duplicate-tolerant) |
| `Source/Core/include/AppController.h` / `src/AppController.cpp` | `CancelAndJoinAllPaneStreamingSyncs()` (live panes + focused context) replaces the focused-only helper in `~AppController` |
| `Source/Core/src/AppController_LocalCacheDb.cpp` | `RecreateLocalCacheDatabase` uses the same helper; its loop keeps only `ResetStaleDeletionState` |
| `Source/Core/src/AppController_Init.cpp` | comment update |
| `tests/support/FakeTrackerClient.h` | opt-in slow paged `FetchIssuesStreamed` (each page an uncancellable in-flight request) |
| `tests/Core/TicketSyncService.test.cpp` | two busy services: no page starts after `CancelAndJoinAll` begins |

## Existing utilities reused

`TicketSyncService::CancelAndJoinActiveStreamingSync` (pass 2), the DR6 all-pane loop shape in
`AppController_LocalCacheDb.cpp`, `FakeTicketSyncDeps`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no extraction or split.

## UX Pillar callouts

Pillar 2 (UI never freezes): the UI-thread join in shutdown and `RecreateLocalCacheDatabase` drops
from about N in-flight timeouts to about 1. Pillar 3 (never crash): non-focused workers are now
joined before the Lua / automation / background-task teardown, not during member destruction.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — no per-frame path changes. The touched code runs only at shutdown and on a cache-database
recreate; no perf scenario exercises either.

## Risks / non-goals

`RecreateLocalCacheDatabase` now joins every pane before `JoinBackgroundTasks` instead of only the
focused one; `JoinBackgroundTasks` only joins `backgroundWorkers_`, so it cannot restart a sync in
between. A request already on the wire still runs to its own timeout; aborting in-flight HTTP is a
non-goal.

## Verification

- [x] Full doctest suite (`ninja-test-msvc`, light features): 3365/3365.
- [x] New case 10/10 locally; a one-pass mutation of `CancelAndJoinAll` fails it every run.
- [x] `test-lint-rules.sh --diff origin/develop` clean (advisory WARNs only).
- [x] code-review agent: 0 Critical/High/Medium, 1 Low (optional helper, skipped).
- [ ] CI: full-feature + `SmatchetStandalone` / `SmatchetCore_DX12` builds.
- doc-validation suite, grill-with-docs stress-test: N/A — single-subsystem bug fix, no new domain terms.

## Out of scope (flagged, not designed)

A shared `CollectPaneTicketSyncServices()` helper for the stale-deletion reset loop (review Low).

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

## Implementation log

Shipped in #2291 (branch `claude/pensive-stonebraker-d3db80`).

## Deviations from plan

None.

## Verification (actual)

See § Verification.

## Archive (post-ship — DO IN THIS PR, never a follow-up)

Done in #2291: Status flipped to `shipped` and the file moved to `docs/plans/shipped/`.
