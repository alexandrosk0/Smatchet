- 2026-06-28 · gate-escape-postmortem · [infra] · P1 — deterministic `Bucket-E Jira fixture-backend (Mesa GL, hard)` lane is advisory-by-prefix in the merge-gate allow-list; a real `--spawn` regression rode past 3 PRs undetected
  Details: The poller's meant-to-block allow-list (`merge-gates.sh:163`,
  `MERGE_GATES_BLOCK_ALLOWLIST_RE`) blocks exactly one Mesa bucket lane —
  `Bucket launch-smoke (Mesa GL)`. The broad `Bucket-` token was deliberately
  removed 2026-06-15 (the poller-jam from flaky Mesa screenshot-diff / ImGui-Test
  lanes that can't boot the software-GL exe — `bucket-mesa-exe-boot` P1, infra.md;
  remedy for those = the `bucket-out-of-band` downgrade label, 2026-06-14 #1218
  postmortem). But that blanket removal ALSO silenced
  `Bucket-E Jira fixture-backend (Mesa GL, hard)` — a **deterministic, boot-capable**
  lane: it boots fine (every OTHER Mesa bucket — launch-smoke, Bucket-C screenshot
  diff, Bucket-E UI tests, both sanitizers — passes on the same runner), and the
  driver itself declares its failures "a fixture-backend regression, not a render
  flake." #1566's `PathConfinement` hardening (`32392e32`) turned it red (the
  spawned child rejects the parent-absolutized `--outLog`, same collision class as
  the perf-harness `--outPath` break in the 2026-06-27 sibling postmortem); #1574
  fixed the perf-harness `--outPath` instance but missed this `ui_test.run --spawn
  --outLog` one, so the lane stayed red on develop and **#1574 (introducer), #1576,
  #1577** all squash-merged past it because it was advisory — `postmortem-owed
  --list` self-reported "clean" since an off-allow-list non-required red is never
  recorded as an escape. Product fix tracked as GitHub Issue #1579.
  Concrete next action: PRIMARY (prevention) — add the literal
  `Bucket-E Jira fixture-backend (Mesa GL, hard)` job name to
  `MERGE_GATES_BLOCK_ALLOWLIST_RE` (`merge-gates.sh:163`), alongside the
  already-blocking `Bucket launch-smoke (Mesa GL)`. Unlike the genuinely-advisory
  flaky lanes (`Bucket-E UI tests`, `Bucket-C screenshot diff` — kept advisory by
  the #1218 `bucket-out-of-band` remedy), this lane is deterministic + boot-capable,
  so blocking it does NOT re-introduce the stochastic-flake jam the 2026-06-15
  `Bucket-` removal was protecting against. **Sequencing precondition: land only
  AFTER the #1579 product fix makes the lane green on develop** — adding it while
  red would block every PR. COMPANION (test) — a `merge_gates.bats` case asserting
  the deterministic fixture-backend lane IS on the allow-list while the flaky
  bucket-C/E render lanes are NOT, so a future blanket `Bucket-` edit can't silently
  re-advisory it.
  Status: open
  Last-reviewed: 2026-06-28
