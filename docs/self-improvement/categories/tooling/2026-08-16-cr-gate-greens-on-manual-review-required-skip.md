- 2026-08-16 · orchestrator · [tooling] · P1 — the `CR findings` gate treats CodeRabbit's `Review skipped: manual review required for this OSS repository` status as "CR reviewed and found nothing", so on this repo it goes GREEN on an entirely unreviewed head — and that is the DEFAULT state of every new PR, not an edge case
  Details: [`cr-finding-gate/action.yml`](../../../../.github/actions/cr-finding-gate/action.yml)
    disambiguates a head with no CR review node via CR's own `CodeRabbit`
    StatusContext. It already special-cases ONE not-a-review description —
    `grep -qiE 'rate.?limit|limit reached'` — and correctly resolves that to
    PENDING plus a full-review nudge. Everything else falls through to
    `SUCCESS) post success "CodeRabbit completed with no review on head
    (skipped/clean)"; exit 0`. The in-file comment states the intent: *SUCCESS ->
    CR is done and skipped the review (trivial / workflow / docs change)*.
    But `Review skipped: manual review required for this OSS repository` does
    NOT mean that. It means the opposite: CR has **not** looked and is waiting to
    be asked. CodeRabbit requires a manual `@coderabbitai review` on repositories
    with fewer than 10 stars, so this status is posted on **every** PR here at
    creation time. The gate is therefore green-by-default on unreviewed code, and
    only turns honest if a real review later lands.
    Observed live on PR #2028: CR posted the skip status at 03:46:17, the gate
    posted `success` at 03:46:30, and the PR then sat for **11.5 hours** with
    `mergeable_state: clean`, all 36 CI checks green, and the CR gate green —
    with zero review having occurred. The only thing that stopped an unreviewed
    merge was the orchestrator manually applying the repo learning ("a skipped /
    rate-limited stamp is NOT review evidence"). A `smatchet-merge-watcher`
    registration, a `governance.auto_merge: on` grant, or any operator trusting
    the checks would have merged it. #2023 and #2025 showed the same green.
    This is the exact fail-open the rate-limit branch was added to close
    (its comment: *"the branch below would translate that into 'completed with no
    review on head (skipped/clean)' and pass an entirely unreviewed commit"*) —
    the same sentence describes this case verbatim, only with a different
    description string. The scoping decision ("an unrecognised description must
    keep its existing pass behaviour instead of hanging every PR") was a
    deliberate fail-open for UNKNOWN markers; `manual review required` is no
    longer unknown.
    **Merge-history sweep 2026-08-16** (added after filing): this is no longer a
    near-miss. A sweep of all 1,416 merged PRs above #500 — reading each merged
    head SHA's status contexts plus CR reviews/comments on that SHA — found
    **4 PRs already merged** with this exact status green and CR never having
    looked at all: #2014, #2024, #2027, #2031 (2026-08-15 → 08-16), none carrying
    an override label. (#2030 carries the same green status but CR did leave a
    walkthrough comment on its head, so it is a fifth instance of the *status*
    and not of the never-reviewed outcome — counted separately on purpose.)
    #2024 is not docs: `fix(sync): stop multi-pane full syncs
    from deleting each other's cached tickets`, 19 code files, merged with zero
    CR review evidence on the head. The 11.5h window on #2028 was the case that
    got caught; these five are the ones that did not. Two sibling fail-opens in
    the same gate, from the same sweep, are filed separately:
    `2026-08-16-cr-gate-greens-on-rate-limited-review.md` (48 merges) and
    `2026-08-16-cr-gate-greens-with-no-cr-status-on-head.md` (27 merges green +
    136 merged on a never-green gate).
  Concrete next action: extend the not-a-review description match from
    `rate.?limit|limit reached` to also cover `manual review required` /
    `review skipped` (keeping the deliberate fail-open for genuinely unrecognised
    descriptions), so the head resolves to PENDING and `maybe_nudge_full_review`
    fires — which is already the right recovery and is proven to work (a manual
    `@coderabbitai review` on #2028 produced a clean review in ~3 min). Guard
    against the sibling risk the existing comment names: a docs-only PR whose
    files are all path-excluded must still pass, and that case is already handled
    up front by the `selfImpOnly` head-accurate file-list check, so widening this
    match does not re-wedge it. Add a `merge_gates`/`cr_finding_gate` bats case
    per description string (rate-limited, manual-review-required, genuinely
    unknown) so the vocabulary cannot silently regress — the sibling entry
    2026-08-16-coderabbit-trigger-identity-and-rate-limit-noop records the same
    class of brittleness in the auto-nudge's own regexes. Est ~0.5d.
  Status: open
  Last-reviewed: 2026-08-16
