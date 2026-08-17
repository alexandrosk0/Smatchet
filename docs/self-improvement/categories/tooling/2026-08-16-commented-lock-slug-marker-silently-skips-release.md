- 2026-08-16 · orchestrator · [tooling] · P2 — a `lock-slug:` marker left inside the HTML comment the PR template ships it in never releases the lock: `lock-cleanup.yml` anchors its regex to `$`, finds no match, emits a `::notice::` and exits 0, so the job goes **green** with the delete step **skipped** — the failure is invisible on the PR (an HTML comment renders as nothing) and invisible in the checks list (green), and only surfaces days later as a stale `refs/locks/<slug>` in the staleness sweep
  Details: **(a) The mechanism.**
    [`lock-cleanup.yml:58-62`](../../../../.github/workflows/lock-cleanup.yml)
    matches `^[[:space:]]*lock-slug:[[:space:]]*[a-z0-9][a-z0-9-]{0,63}[[:space:]]*$`
    — the trailing `$` after the slug means the template's
    `<!-- lock-slug: your-slug-here -->` form cannot match, because ` -->`
    follows the slug. On no match, `:64-68` prints
    `::notice::No 'lock-slug: <slug>' line found in PR body; no release.` and
    `exit 0`. The `Delete refs/locks/<slug> if present` step is gated on
    `steps.parse.outputs.slug != ''` (`:80`), so it reports **skipped**, and the
    workflow run is **green**. The regex is pinned verbatim as `EXPECTED_PAT` in
    [`tests/bats/lock_cleanup.bats:19`](../../../../tests/bats/lock_cleanup.bats),
    so the anchor is deliberate and tested — the defect is not the regex, it is
    that the *only* signal for the miss is a `notice` annotation nobody reads.
    **(b) The trap is the template itself.**
    [`.github/pull_request_template.md:50-51`](../../../../.github/pull_request_template.md)
    ships both markers pre-commented, and `:46` instructs "add the trigger line
    below somewhere in the PR body (**uncomment** + edit)". Leaving the comment
    delimiters in place is therefore the *default* state of every PR body, and
    the one keystroke that arms the release is the one nothing checks. The rule
    itself is unambiguous —
    [`ship-loops.md:162`](../../../agent-rules/ship-loops.md) requires the `open
    PR` step to write "*the exact line `lock-cleanup.yml` matches*", i.e. the
    bare form — so this is an operator error, not a doc conflict;
    it is worth a guard precisely because the correct and incorrect forms are
    visually identical in the rendered PR body.
    **(c) Observed, 2026-08-16.** Two of three PRs merged this session
    (`agent-debug-build-flag`, `windows-cdb-tooling-doc`) carried the commented
    form. Both cleanup runs were green with the delete step skipped; both locks
    survived the merge and had to be deleted by hand via
    `gh api -X DELETE repos/<owner>/<repo>/git/refs/locks/<slug>` — the same call
    the workflow would have made. `gh run view --log | grep '::notice'` does not
    surface the annotation (it returns only the `##[group] Run` echo); the notice
    is reachable only via
    `gh api repos/<owner>/<repo>/check-runs/<jobid>/annotations`. The existing
    downstream catch,
    [`lock-staleness-sweep.sh:177`](../../../../agents/scripts/core/lock-staleness-sweep.sh),
    already names this exact failure in its remediation text ("the PR was missing
    a `lock-slug: ${slug}` line in its body") — so the class is known and the
    sweep is the only thing catching it, days late.
  Concrete next action: (1) **Detect the commented form and fail loudly** — in
    the parse step, when the body contains `lock-slug:` but the anchored regex
    matched nothing, `::error::` (not `::notice::`) and exit non-zero with the
    text *"a `lock-slug:` marker is present but commented out or malformed;
    uncomment it to a bare line"*. This is the only variant that distinguishes
    "no lock on this PR" (legitimate and common — pure-docs slices) from "a lock
    that was meant to release and did not", and it is a two-line change plus a
    bats case alongside the existing `EXPECTED_PAT` assertions. Deliberately
    scope it to the *commented-marker* case; a blanket fail-on-no-slug would red
    every lockless PR. (2) **Remove the trap at the source** — either drop the
    comment delimiters from the template line (leaving a placeholder slug that
    fails the grammar check loudly) or move the marker out of an HTML comment
    entirely, so the armed and unarmed states differ visibly in the rendered
    body. Keep the informational `holds-lock:` line commented; it is
    intentionally never matched. (3) Have the ship-loop's `open PR` step
    self-verify: after creating the PR, re-read the body and assert the bare
    line matches the same anchored regex, so the miss is caught at PR-open time
    rather than at merge time. Est ~0.5d total; (1) is the cheapest and has a
    gate behind it.
  Status: open
  Last-reviewed: 2026-08-16
