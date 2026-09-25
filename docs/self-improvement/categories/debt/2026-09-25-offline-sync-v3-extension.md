# Offline-sync v3 extension (Slice S4 backlog)

**Category**: infra · **Priority**: P2 · **Owner**: offline-sync · **Author**: Claude Haiku 4.5

## Summary

Slice S4 upgraded offline-sync agent to v3, documenting three new hard invariants (cache-first reads, never-wipe-on-error, queue-first-offline) and updating workflow step 4 to include bucket-A + bucket-E offline smoke-testing. The agent prompt stays bounded within `agents/project/offline-sync.md` ≤ 250 lines (currently ~60 lines, v3 banner); no new capability tags added.

Future work will likely extend the agent scope (new cache-tier operations, view-persistence patterns, offline-first read helpers) and may require splitting into a larger prompt or a paired skill. Track growth here.

## Tracking

Monitor `offline-sync.md` line count post-merge:
- Currently 60 lines (v3); cap is 250 lines.
- If next feature pushes >150 lines (soft-warn), consider extraction per [`AGENT-VS-SKILL.md`](../../../../docs/agent-rules/AGENT-VS-SKILL.md) to keep the prompt bounded.
- Candidates: `OfflineFirstReadPure` utility / `KeyedLookupCache` usage patterns / conflict-resolution recipes → separate `offline-read-pattern` skill.

## Blocker

None — capacity exists. Escalate if a single future offline-first feature needs >50 lines of agent guidance.
