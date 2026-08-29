# IPlugin per-plugin-type accessor — dormant watch (fires on a second gated virtual)

- **Category**: debt · **Priority**: P3 (watch, no queued work) · **Filed**: 2026-08-29 (migrated
  from `backlog/BACKLOG_CODE_REVIEW.md` N13 on ledger retirement; original finding 2026-05-16)
- **Where**: `Source/Core/include/IPlugin.h` — the `#if SMATCHET_WITH_MCP`-gated
  `TryGetMcpStatusSnapshot` virtual.

## Problem

`IPlugin` carries exactly one plugin-type-specific accessor, gated behind
`SMATCHET_WITH_MCP`. One is acceptable; the ledger's concern was accretion — a second
gated per-plugin-type virtual would signal the interface growing type-switches instead of a
capability-tag system. Reconciliation passes on 2026-07-05 and 2026-08-18 both confirmed
the trigger has not fired (~400+ PRs): still exactly one gated virtual, no second accessor.

## Trigger / proposed shape

No action while the count stays at one. If a second plugin-type accessor lands on
`IPlugin`, replace the pattern with a capability-tag / query-interface mechanism instead of
stacking gated virtuals. Until then this entry only preserves the watch that previously
lived in the retired ledger.
