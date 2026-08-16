- 2026-08-16 · orchestrator · [tooling] · P1 — absence of any CodeRabbit signal is resolved to *pass*, not *block*: 27 PRs merged with the CR gate GREEN while CR had posted **no review, no comment and no StatusContext at all** on the merged head — and a further 136 merged while the required `CR findings (0 actionable)` context was never green (82 of those with no CR signal either), so the required check is not, in practice, required
  Details: same 2026-08-16 sweep as the two sibling entries — every merged PR
    above #500 (1,416), each merged head SHA's status contexts plus CR reviews
    and comments on that SHA.
    **Class A — gate green, CR silent (27).** No `CodeRabbit` StatusContext
    exists on the merged head and no CR review or comment landed on it, yet
    `CR findings (0 actionable)` reads `success`. 14 carried `cr-out-of-band`
    (conscious overrides, working as designed) and 3 are dependabot bumps
    (#1582/#1902/#1926, exempt by policy). The remaining **10 are unlabelled and
    unexplained**: #1727, #1731, #1737, #1748, #1767, #1781, #1782, #1784,
    #1795, #1798 (2026-07-10 → 07-12, all docs/backlog reconcile PRs — low stakes
    individually, but they prove the shape).
    **Class B — merged on a never-green gate (136).** The required context was
    still `pending` at merge on 136 PRs, `failure` on 5 (#746, #780, #904, #1155,
    #1745), and absent entirely on 25 (pre-gate). Of the 136 pending, **82 also
    had no CR StatusContext**: 1 in May, 11 in June, **70 in July**, tail
    #1788–#1796. Earlier spot-checks (#1029, #1299) show the pending text
    verbatim: *"awaiting CodeRabbit review on current head"* — merged anyway.
    Mechanism, both surfaces fail open on absence:
    - Poller: `agents/scripts/core/merge-gates.sh:1029-1034` — *"Grace window
      expired without a review or SUCCESS status; treating NONE as pass"* →
      `cr_pass=true`, WARN only. A stuck integration and a never-installed
      reviewer are indistinguishable from a clean one after 10 polls
      (`MERGE_GATES_CR_GRACE_POLLS`).
    - Server side: `.github/actions/cr-finding-gate/action.yml` posts PENDING and
      leaves it there when CR never reports — correct — but Class B shows a
      pending required context did not stop 136 merges. Either the context is not
      actually in `develop`'s required set, or those merges used `--admin` /
      native merge. Branch protection is unreadable from an agent token
      (`Resource not accessible by integration`), so this needs a maintainer to
      confirm which.
    The July concentration of Class B (70 of 82) overlaps the rate-limit cluster
    in the sibling entry, i.e. the same CR-quota window produced both an
    accepted-as-green fail-open and a merged-past-pending one.
    **Class C — the self-improvement exemption (30 merged, 12 of them inside the
    unreviewed-80).** Distinct from A and B and *deliberate*
    (`merge-gates.sh:1443-1453`, mirrored in the action): a diff entirely under
    `docs/self-improvement/**` greens the gate with `self-improvement-only diff
    (N file(s)) — CR review exempt`, no review required. Recorded here not as a
    bug but as the fourth way the gate reads green on an unreviewed head, so a
    reader counting review coverage does not mistake these 30 for reviewed.
    **Reproduced by the PR that files these entries.** #2038 (this filing) opened
    2026-08-16 17:21Z; CR posted `Review skipped: manual review required for this
    OSS repository`, Cursor Bugbot replied *"couldn't run — usage limit
    reached"*, and the `CR finding gate` posted `success` at 17:23:11 on the
    Class-C path — **green in under two minutes with neither reviewer having read
    a line**. Both fallbacks being unavailable at once is the case the exemption
    was never scoped against: it was justified as belt-and-suspenders beside a
    `.coderabbit.yaml` path filter, on the assumption that a real review is
    merely redundant here, not absent. A manual `@coderabbitai review` was
    triggered on #2038 rather than merging on that green.
  Concrete next action: (1) maintainer to dump `develop`'s required-status-check
    set and confirm `CR findings (0 actionable)` is in it and that admin
    enforcement is on — Class B is either a protection-config gap or an `--admin`
    habit, and the fix differs; (2) make the poller's grace expiry terminal
    instead of passing — after the window, block with the nudge already proven to
    work (`@coderabbitai review`), and require `cr-out-of-band` + `cr-disposition`
    to merge on a silent head, matching what the size-skip and rate-limit arms
    already demand; a "CR never ran" head is exactly the case a label should have
    to attest; (3) record the CR verdict string in `merge-snapshots.jsonl` (it
    carries `gates`/`redChecks`/`overrideLabels` but nothing about review
    evidence, which is why this needed a 1,416-PR API sweep to find at all) so
    the next audit is a `jq` query; (4) add a `postmortem-owed` classifier for
    "merged with the CR context pending" — Class B is a gate escape by the repo's
    own definition and none of the 136 were filed as one. Est ~0.5 d for (2)+(3),
    (1) is a maintainer action. Cross-ref:
    `2026-08-16-cr-gate-greens-on-rate-limited-review.md`,
    the manual-review-required sibling (archived in `applied.md`, fixed by
    #2036),
    `docs/adr/0017-merge-time-snapshot-ledger.md`.
  Status: open
  Last-reviewed: 2026-08-16
