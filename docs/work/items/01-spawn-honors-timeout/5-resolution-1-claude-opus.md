# 01-spawn-honors-timeout — Review resolution (round 1, post)

Addresser: claude-opus. Findings gathered from `5-review-1-claude-opus.md` (3) and
`5-review-1-claude-sonnet.md` (1); sonnet §1 and opus §3 dedup to one finding (R3). All findings
verified against the working tree before binding.

---

## R1 — Bucket-E CI comment states the pre-fix behaviour and steers maintainers to the wrong knob

- **Raised by:** 5-review-1-claude-opus §1
- **Disposition:** fix
- **Resolution:** Rewrote the comment at `.github/workflows/build-and-test.yml:1249-1259` — it now
  states the budget is frames-derived via `ScenarioWaitMs` *only without an explicit `--timeout`*,
  that since #1943 `--timeout=<ms>` overrides it on the `--spawn` path too and is the direct dial
  for more wall-clock, and the "filed separately" sentence is gone. Bucket-E keeps `--frames=5400`:
  changing the lane's runtime knobs is out of scope for this fix PR, and the comment now tells the
  next maintainer which dial to reach for. Verified in the diff.

## R2 — `SMATCHET_SPAWN_TIMEOUT_MS` documented as `--timeout`'s default in three places, honoured in none of the budget paths

- **Raised by:** 5-review-1-claude-opus §2
- **Disposition:** defer (→ Spawned.md)
- **Resolution:** Verified: `ParseArgs` (CliArgs.cpp:287-299) never consults the env var; the only
  reader (CliHelpAndAttach.cpp:304) feeds the attach path's HTTP read timeout. Pre-existing defect,
  distinct surface from this fix. Entered under `Spawned.md § Bugs` with both candidate remedies
  (default `out.timeoutMs` from the env var in `ParseArgs`, or delete the doc claim).

## R3 — Nothing pins the call-site wiring; issue #1943's suggested integration test was not added

- **Raised by:** 5-review-1-claude-opus §3; 5-review-1-claude-sonnet §1
- **Disposition:** defer (→ Spawned.md)
- **Resolution:** Both reviewers agree the shared helper is the strongest guard available today and
  the residual risk is small; they differ only on the ledger section (Deferred vs Backlog). Bound as
  **Deferred** — the ask has a natural trigger (next touch of bucket-E or the spawn smoke lane),
  which is what distinguishes a deferral from undated backlog. Entered under `Spawned.md § Deferred`
  citing both reviews and the in-repo smoke-test pattern to follow. Per opus's rider, the
  overstated "cannot diverge again" claim is softened in the PR body (commit `bc3b1cde`'s message is
  immutable short of a rewrite; the PR body is the surface reviewers and the merge record read).
