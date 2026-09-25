# Offline UI bucket-E validation (Slice S4 backlog)

**Category**: test · **Priority**: P2 · **Owner**: offline-sync · **Author**: Claude Haiku 4.5

## Summary

Pillar 6 offline-first enforcement (S4 shipped) includes bucket-E visual-validation coverage (Pillar 4 + Pillar 6 contract: a network-backed view must show cached data + `DataFreshnessCue`, never loading-only). The `scripts/dev/test-ui-offline-first.sh` smoke-test harness is proactive; no bucket-C / bucket-E snapshot testing for offline-first UI patterns exists today.

Per code-review AGENTS.md: "An offline case exists — bucket A with `GlobalFakeNetwork()` or bucket E via `scripts/dev/test-ui-offline-first.sh`. Missing = **Medium** finding."

## Work

Author a bucket-E screenshot-diff test suite covering:
1. Cached ticket grid with stale freshness cue (network off, prior-fetched data, shows grid + "stale" cue).
2. Empty cache + network off (queued write, shows queue badge, no "loading").
3. Failed fetch (shows cache or fallback, not loading spinner).
4. Network restore (replays queue, grid updates, freshness cue resets).

Add to `scripts/dev/test-ui-offline-first.sh` as test cases; wire to CI bucket-E via the visual-validation gate contract.

## Blocker

Blocked on test-author availability (Slice S4 is process-only, no Bucket-E work inside; can be addressed in a follow-up slice).
