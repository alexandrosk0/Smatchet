- 2026-08-16 · orchestrator · [tooling] · P1 — a `Review rate limited` CodeRabbit status is accepted as review evidence somewhere in the merge path: **48 PRs merged with the `CR findings (0 actionable)` context GREEN on a head CR never reviewed**, 38 of them code PRs that the documented CODE-PR pause should have blocked, and only 4 of the 48 carrying any override label
  Details: found by a full-history sweep (2026-08-16) of every merged PR above
    #500 — 1,416 PRs — reading each merged head SHA's status contexts plus CR's
    reviews and comments on that same SHA (review evidence matched by
    `commit_id == merged head sha`, so a review of an earlier head does not
    count). 80 of the 1,416 merged with the CR gate context reporting `success`
    and **no CR review node on the merged head at all**; 48 of those carry CR's
    own `CodeRabbit` StatusContext with `state=success, description="Review rate
    limited"` — CR stating in its own words that it did not review.
    Merged 2026-07-22 (#1909) → 2026-08-11 (#1994). Only **4** (#1962, #1964,
    #1965, #1977) carried `cr-out-of-band`, so 44 were not conscious overrides —
    the operator was told the gate was green. 19 of the 48 got a rate-limit
    notice comment on the head and nothing else; the rest got no CR output
    whatsoever. Code-bearing, not docs: classified against the poller's own
    `pure_docs` regex (`^(docs/|backlog/|agents/scripts/|.*[.]md$)`), **38 of the
    48 are NOT pure-docs** — #1962 (bucket-C screenshot determinism, 14 code
    files), #1989/#1994 (tracker-type resolution), #1992 (icon-upload latch),
    #1984 (dock writes against a dead pane), #1957–#1960 (the PowerShell→bash
    toolchain port), #1964/#1977 (the gate scripts themselves).
    Two paths could produce this and the evidence does not separate them
    post-hoc, so both need closing:
    (a) **Server-side gate.** `.github/actions/cr-finding-gate/action.yml`
    resolved any CR `SUCCESS` description to `post success "CodeRabbit completed
    with no review on head (skipped/clean)"` until the `rate.?limit|limit
    reached` branch (now action.yml:395) landed in #1996 on **2026-08-11
    12:40Z**. 37 of the 38 code merges precede that timestamp. **#1994 does
    not** — it merged 2026-08-11 21:45Z, nine hours after the fix, still green
    on a rate-limited head; that single case needs a run-log check before the
    server side is called closed.
    (b) **Poller.** `agents/scripts/core/merge-gates.sh` documents the right
    behaviour at :1340-1352 — `cr_rate_limited` + non-pure-docs + `cr_state=NONE`
    → `cr_pass=false` (`cr-rate-limit-code-pr-auto-pause`) — but that block is
    computed at :1339, *after* the `NONE` arm at :955-1036 has already set
    `cr_pass=true` via the grace branches (:1013-1022 "status=SUCCESS but no
    inline evidence after grace … assume status-only", :1029-1034
    "grace-expired"). The later assignment does win, so the pause is reachable —
    but any merge that did not run through this poller (native merge, GitHub
    auto-merge on required contexts, `--admin`) never sees it at all, and the
    required-context path is exactly (a).
    Sibling note on the same block: the pure-docs auto-downgrade (:1341-1345)
    counts `agents/scripts/**` as docs — "markdown is never compiled" is not true
    of the shell that runs the merge gates. #1938 took that path.
  Concrete next action: (1) verify #1994 against the #1996 action revision — if
    the fixed action greened a `Review rate limited` head, the description match
    is not reached on that code path and the bug is still live server-side;
    (2) hoist the rate-limit verdict in `merge-gates.sh` so `cr_rate_limited` is
    evaluated *inside* the `NONE` arm, before the grace branches assign
    `cr_pass=true`, rather than relying on a later re-assignment — the grace
    branches are the generic fail-open and should never see a head CR has
    explicitly declined; (3) narrow the pure-docs auto-downgrade to exclude
    `agents/scripts/**` (gate shell is executable, and a rate-limited review of a
    gate change is precisely the review worth waiting for); (4) add
    `cr_finding_gate` bats cases per CR description string — `Review rate
    limited`, `Review skipped: manual review required…`, `Review skipped`,
    `Review completed`, and a genuinely unknown string — asserting
    PENDING/PENDING/SUCCESS/SUCCESS/SUCCESS, so the vocabulary cannot regress
    silently again. Est ~0.5 d. Cross-ref: sibling
    `2026-08-16-cr-gate-greens-on-manual-review-required-skip.md` (same fail-open,
    different description string, 5 merged PRs);
    `2026-08-16-cr-gate-greens-with-no-cr-status-on-head.md` (third string: none
    at all); #1996; #2004.
  Status: open
  Last-reviewed: 2026-08-16
