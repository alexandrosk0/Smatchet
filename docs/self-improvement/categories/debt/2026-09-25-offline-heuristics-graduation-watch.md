# Offline-heuristics graduation watch (Slice S4 backlog)

**Category**: infra · **Priority**: P2 · **Owner**: offline-sync · **Author**: Claude Haiku 4.5

## Summary

Five WARN-first heuristic rules ship with S4 (`offline-loading-only-render`, `offline-inflight-latch-unguarded`, `offline-failure-cached-as-loaded`, `offline-cache-cleared`, `offline-network-read-ungated`). Per ADR-0015 duplication gate precedent, heuristics graduate to blocking once a 5-PR calibration window closes with <10% false-positive rate.

## Tracking

Monitor CI over the next 5 PRs post-S4 merge:
- Tally WARN firings (should be low; tree is clean).
- Tally true-positives (genuine offline-first bugs caught).
- Tally false-positives (legitimate pattern flagged incorrectly).

When (true_positives + false_positives) ≥ 5 and false_positives / total < 10%, escalate graduation to the orchestrator.

## Blocker

None — calibration window is advisory, not blocking.
