- 2026-06-27 · gate-escape-postmortem · [infra] · P1 — `Perf PR-fast` (meant-to-block, not GH-required) merged CANCELLED via a human native-merge, wedging every `--spawn` PR on develop
  Details: PR #1566 (`32392e32`, human direct-merge by alexandrosk0) shipped two
  breaking changes to the `--spawn` perf harness while its own `Perf PR-fast
  (windows-2022)` check was terminal **CANCELLED** (the token-401 idle-to-timeout
  signature; single rollup entry, no SUCCESS twin). Two compounding gate holes let
  it ship silently: **(1) prevention** — `Perf PR-fast` is in the custom poller's
  meant-to-block allow-list but is NOT a `branch_protection.required_contexts`
  entry, and a native GitHub merge (human direct-merge, or native auto-merge) does
  not consult `merge-gates.sh`; only the ~5 required contexts gate it, so a
  CANCELLED-but-not-required check sails through. **(2) detection** —
  `postmortem-owed.sh` trigger-1 deliberately excludes CANCELLED as a "supersede"
  (lines 314-326), and a human native-merge writes no `merge-snapshots.jsonl` line
  (line 388 lossless path), so the escape is invisible on BOTH the snapshot and the
  CANCELLED-excluding live path — `postmortem-owed --list` reads "clean." Blast
  radius: every `--spawn` gate on develop broke at once (#1571/#1572 stuck 250+
  watcher cycles).
  Concrete next action: PRIMARY (prevention) — promote `Perf PR-fast (windows-2022)`
  to a `branch_protection.required_contexts` entry on develop so native merges
  cannot bypass it. PRECONDITION (must land first or it wedges non-perf PRs): the
  check runs only on perf-relevant diffs (`Detect perf-relevant changes` path
  filter), so it must emit a terminal neutral/success status on non-perf diffs
  before it can be required — otherwise a required context that never posts wedges
  every docs/non-perf PR (the same conditional-skip trap noted in
  postmortem-owed.sh lines 314-318). COMPANION (detection, no precondition) — refine
  `postmortem-owed.sh` trigger-1 so a meant-to-block context whose collapsed
  latest-run conclusion is CANCELLED (with no later SUCCESS run for the same
  context — the existing group_by-name/max-startedAt dedup already drops the
  concurrency twin) counts as an escape, closing the silent-miss that let #1566
  through undetected.
  Status: open
  Last-reviewed: 2026-06-27
