# Bucket-C screenshot goldens are non-deterministic against a machine with cached backend config

- **Date**: 2026-07-14
- **Author**: orchestrator
- **Category**: tooling
- **Priority**: P2

## What

While bootstrapping the 4 new `user-info-*` bucket-C goldens (PR-10), the 3
pre-existing goldens (`dock-gap-sentinel`, `command-palette-fuzzy`,
`code-syntax-coloring`) failed `scripts/dev/test-screenshot-diff.sh` at **L∞
240-245** against their committed PNGs — and their *self*-diff (two fresh
captures on the same machine, back-to-back) was **L∞ 105-126**, i.e. they are
non-deterministic *against themselves*. Root cause: the `--spawn` exe boots
against this machine's **cached Jira/backend config** (`SMATCHET_USER_DATA`),
so those scenarios render live grid rows + "Loading…" async + wall-clock-timed
sync toasts that differ frame-to-frame. The committed goldens were captured on
a different machine/config, so a fresh local `test-screenshot-diff.sh` can
never pass for them. The advisory (continue-on-error) CI lane masks this.

The 4 new `user-info-*` scenarios pass **L∞ 0** self-diff because they were
authored with explicit determinism isolation (per-scenario empty
`SMATCHET_USER_DATA` dir in the driver + `cfg.UiMode` pin + `DismissAllLive()`
toast clear + empty-email/cleared-git fast-fail + `WhisperSetupCompleted` pin
to drop the first-run consent banner). The 3 legacy scenarios have none of this.

## Why it matters

A golden that can't pass a fresh local capture is untrustworthy: a dev can't
tell an intentional UI change from ambient-config noise, and the advisory lane
green-washes it. The determinism recipe now proven for `user-info-*` is the fix.

## Concrete next action

1. Apply the same per-scenario `SMATCHET_USER_DATA` empty-dir isolation the
   `user-info-*` driver branch uses to the 3 legacy scenarios in
   `scripts/dev/test-screenshot-diff.sh` (widen the isolation `case` from
   `user-info-*` to all screenshot scenarios), then re-bootstrap + commit their
   goldens from an isolated run so they become machine-independent.
2. Once all bucket-C goldens pass a fresh isolated self-diff, consider graduating
   the CI lane from advisory → blocking (it currently masks this class).
3. Document the determinism recipe (isolate config + pin UiMode + clear toasts +
   fast-fail network legs + suppress first-run banners) in a test-authoring note
   so the next golden author doesn't rediscover it (cost several capture rounds
   here). Cross-ref: PR-10 `user-info-*` scenarios (`UserInfoScreenshotScenario.h`).

## Status

open (verified this session; the driver-isolation widen + legacy-golden
re-bootstrap is the implementable fix)
