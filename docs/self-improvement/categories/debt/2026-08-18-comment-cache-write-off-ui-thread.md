# Comment-cache persistence still writes SQLite on the UI thread

- **Category**: debt · **Priority**: P2 · **Filed**: 2026-08-18 (PR #2123 CodeRabbit finding)
- **Where**: `AppController::UpdateCachedCommentsFromThread` → `UpdateTicket` → `Cache->SaveTicket`
  (`Source/Core/src/AppController_CatalogAndFieldEdit.cpp`), reached from the comments modal's
  main-thread post-back (`SmatchetCommentsModalUi.cpp`) and the Comments-cell lazy tooltip
  post-back (`SmatchetActiveProjectGridCells.cpp`).

## Problem

`UpdateTicket` runs its SQLite `SaveTicket` synchronously on the UI thread. Under cache
contention, `RunWriteTxnWithBusyRetry` can stall the frame pump (worst case ~25s + sleeps —
the #1894 ledger entry in `docs/self-improvement/historical-review-findings.md`,
batch-20-redo, HIGH, `LocalCacheManager.cpp` RunWriteTxnWithBusyRetry). This PRE-DATES
PR #2123: the replaced `UpdateCachedCommentCount` used the identical call chain. PR #2123
added a second caller (the tooltip's lazy fetch) but no new write mechanics.

## Why not fixed in PR #2123

`UpdateTicket`'s key/generation latch (`issue #1081`) is deliberately UI-thread-coupled
(capture + check + apply on the same latched context). Splitting persistence onto a worker
while keeping the swap-race guarantees is a threading-contract refactor across
AppController + LocalCacheManager — out of scope for a tooltip PR (drive-to-green: raise
larger asks, don't widen).

## Proposed shape

Bound the write by a wall-clock deadline and/or route `SaveTicket` through
`LaunchBackgroundTask` with the latched key + generation captured at dispatch, posting only
the ActiveTickets republish back to the UI thread — per the #1894 entry's proposed fix.
Applies to every `UpdateTicket` UI-thread caller, not just comments.
