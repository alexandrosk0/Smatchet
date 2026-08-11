- 2026-08-06 · claude-code · [process] · P1 — an `--admin` merge past checks that never ran leaves no trace any detector reads, and `merge-gates.sh` mislabels a CI-cannot-run head as "required-missing"

  **Evidence.** PR #1941 (preferences IA re-segmentation + global search) squash-merged
  2026-08-06T19:25:12Z as `c7fb2236` on `develop` via `gh pr merge --squash --admin`, with all
  22 branch-protection-required contexts absent from the head rollup. The head `4617a034` was
  never built at all: `gh api repos/<o>/<r>/actions/runs?head_sha=4617a034…` returned
  `total_count: 0`. `postmortem-owed.sh --list` afterwards reported "no gate escapes owed
  (last 20 merges clean)".

  Two independent holes produced this:

  1. **No defined behaviour when CI is structurally unavailable.** Actions was jammed
     repo-wide — 75 runs stuck `queued` since 18:13 UTC, no new run created repo-wide after
     19:16 UTC, and a `gh pr close && gh pr reopen` (to re-fire the `pull_request` event)
     produced 0 runs. There was no path to a green head. The ship-loop's only defined move
     is to keep polling, so the operator's choices collapse to "wait indefinitely" or
     "override" — and the override is exactly what the gates exist to prevent. The escape is
     the *absence of a third option*, not the person who took the second.

  2. **The escape class is invisible to the detector.** `postmortem-owed.sh` keys on a
     non-SUCCESS check at merge, an override label, a `Revert` commit, or an overdue
     deviation. An `--admin` merge past *absent* checks emits none of those four signals:
     there is no red check (there is no check), no label, no revert. This is the same
     detection hole already recorded in the ledger twice — `2026-07-10 · PR #1698` and
     `2026-08-05 · PR #1937` — both of which proposed a develop-tip required-green assertion
     that never landed. Third recurrence.

  A third, lower-severity contributor worth fixing in the same area: **`merge-gates.sh`
  reports a conflicted head as N `required-missing` checks.** When `mergeStateStatus` is
  `DIRTY` / `mergeable` is `CONFLICTING`, GitHub declines to build the head at all, so the
  poller sees 22 absent required contexts and prints the generic "never ran; e.g. a
  GITHUB_TOKEN bot push that did not re-trigger CI" hint. That cost a full 90-poll timeout
  before the actual cause (a conflict in `docs/plans/INDEX.md`) was found by hand. The
  actionable cause was available on poll 1 from a field the poller already fetches.

  Proposed fixes:

  1. **Detect the merge after the fact.** Extend `postmortem-owed.sh` with a fifth signal:
     for each merge commit on `develop` in the scanned window, resolve the merged PR's head
     sha and flag it when `actions/runs?head_sha=<sha>` yields `total_count == 0`, or when
     the head rollup carries fewer contexts than the branch-protection required set. This
     catches admin merges, zero-rollup merges, and the "CI never triggered" class in one
     check, and it needs no cooperation from whoever performed the merge — which is the
     property the label-keyed signals lack.
  2. **Name the real cause in the poller.** In `merge-gates.sh`, branch on
     `mergeStateStatus == DIRTY` / `mergeable == CONFLICTING` before reporting
     `required-missing`, and emit a distinct blocked reason ("head is conflicted — CI will
     not build it; merge origin/develop first"). Same for `BLOCKED` with a zero-length
     rollup. Cheap: both fields are already in the existing GraphQL response.
  3. **Give "CI is unavailable" a defined move.** Today the ship-loop has none. Minimum
     viable: when the poller observes zero runs created repo-wide inside the poll window
     (an Actions outage, not a PR problem), it should stop polling and escalate with that
     diagnosis rather than time out at 90 polls with a per-check message — per
     `AI_POLICY.md` § Escalate, don't assume, an unvalidatable state is an escalation, and
     an outage is unvalidatable by construction.

  Concrete next action: fix (1) — it is self-contained inside `postmortem-owed.sh`, closes
  a hole that has now recurred three times, and is testable in `tests/bats/`. (2) is a small
  follow-up in the same PR if the diff stays small. (3) needs a design call on what the
  ship-loop does with an escalation and should not be bundled.

  Related, distinct — do not merge these: the two prior ledger entries (2026-07-10 · #1698,
  2026-08-05 · #1937) describe the same *detector* hole reached from a different direction
  (a check green on the PR head and red on develop). A single develop-tip required-green
  assertion would close all three, and that is the argument for finally building it.

  Compensating verification actually performed on the merged head, for the record: dual-target
  build (`SmatchetStandalone` + `SmatchetCore_DX12`) EXIT=0 and
  `test-lint-rules.sh --diff origin/develop` EXIT=0 (advisory WARNs only). That is not CI and
  does not substitute for it — the next `develop` post-merge run, once Actions drains, is the
  backstop to watch.

  **Update 2026-08-11 — fixes (1) and (2) shipped; (3) is all that remains.**

  - **(1) Detect the merge after the fact — DONE.** `postmortem-owed.sh` gained the
    fifth signal as a required-context-ABSENT cross-check: every name in
    `branch_protection.required_contexts` must appear in the merged PR's rollup
    (field 5, the `|||`-joined present-context list the script already parsed for the
    expected-present allow-list), else the merge owes a postmortem. It runs on the
    snapshot path as well as the live one — a snapshot records the merge-instant RED
    set, never rollup *membership*, so absence is only ever observable from the live
    rollup and an instrumented merge would otherwise be exempt from the one signal
    snapshots cannot carry. It needs no cooperation from the merge actor, which is
    the property this entry wanted. Implemented via rollup membership rather than the
    proposed `actions/runs?head_sha=<sha>` probe: same escape class, no extra API call
    per scanned merge, and it also catches a head that *was* built but whose suite is
    missing contexts. Guarded by `POSTMORTEM_ABSENT_GRACE_SECONDS` (default 3600) —
    the infra entry `required-check-that-never-reports-is-invisible` measured a ~27-min
    check-suite creation lag, so a just-merged PR is deferred to the next sweep instead
    of false-flagged. Suite: `tests/bats/postmortem_owed.bats` § required-absent,
    including this PR's exact shape (zero rollup, no red, no label → owes).
  - **(2) Name the real cause in the poller — DONE.** `merge-gates.sh` now branches on
    `mergeStateStatus == DIRTY` before emitting the generic hint and reports "head is
    CONFLICTED … merge origin/develop first; this is NOT a CI fault and polling will
    not clear it". As predicted the field was already in the poller's GraphQL response,
    so the diagnosis is available on poll 1 rather than after a 90-poll timeout. The
    non-DIRTY branch keeps the original "never ran" hint and gained the creation-lag
    caveat. Both are still BLOCKs — only the diagnosis differs. Cases in
    `tests/bats/merge_gates.bats` § required-missing cause attribution, incl. a negative
    canary that a DIRTY head with nothing absent emits no conflict line.
    *Observed while testing, not changed:* the `mergeStateStatus` guard blocks on
    `BLOCKED|BEHIND` only, so an all-green **DIRTY** head still reaches `GATES_PASSED`
    and the conflict surfaces later as a failed merge attempt. Left as-is deliberately
    (this entry asked for diagnosis, not a new block); pinned by a test so it is
    recorded as observed rather than assumed.
  - **(3) Give "CI is unavailable" a defined move — NOT DONE, unchanged.** Still needs
    the design call on what the ship-loop does with an escalation, which this entry
    said should not be bundled. This is the entry's whole remaining scope.

  Status: open (partial — (1) + (2) shipped 2026-08-11; (3) outage-escalation design call remains)
  Last-reviewed: 2026-08-11
