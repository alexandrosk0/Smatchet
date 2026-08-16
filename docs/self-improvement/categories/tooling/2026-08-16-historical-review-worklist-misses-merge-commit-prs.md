- 2026-08-16 · orchestrator · [tooling] · P1 — `historical-review-worklist-misses-merge-commit-prs`: the historical-review work-list is built by scraping `(#N)` off develop squash subjects, so a PR that landed as a **true merge commit** is invisible to it — Batch 20 claimed a contiguous #1–#1940 frontier while silently omitting 7 merged PRs
  Details: The sweep's resume recipe
  ([`historical-review-findings.md`](../../historical-review-findings.md) § Sweep status)
  discovers each batch's work-list from `gh pr list`, but in a `gh`-less environment
  (every remote session — see the Batch 13 header) the documented fallback is to
  scrape the develop log for squash subjects ending in `(#N)`. That scrape is only
  correct under the repo's stated squash-merge invariant, and the invariant does not
  actually hold: PRs merged with a **merge commit** carry the subject
  `Merge pull request #N from <branch>`, which has no trailing `(#N)`, and their
  constituent commits carry no PR reference at all. Such a PR is not "skipped" with a
  warning — it never enters the work-list, so it cannot appear in the batch's
  reviewed / clean / superseded counts, and the batch reports a complete frontier.
  <br><br>
  Measured on the #1878–#1940 range while re-running Batch 20 at full agent grade:
  GitHub's merged list for `base=develop` returns **60** PRs; the `(#N)` scrape
  returns **53**. The 7 missing are **#1883, #1919, #1920, #1921, #1923, #1927,
  #1932** — every one a merge commit, and (not coincidentally) every one part of the
  release-publishing pipeline (`.github/workflows/release.yml` driving
  `scripts/publish/release-github.sh`), which is signing/publishing code that has
  therefore never been survivor-reviewed. Batch 20's header nonetheless states "The
  frontier is now #1–#1940 contiguous", and the § Sweep status coverage claim inherits
  that error.
  <br><br>
  This is a **recurrence, not a first occurrence**, which is what raises it above a
  one-off correction. The Batch 13 header already records the same class — "4 PRs with
  edited/non-standard squash subjects the `(#N)`-suffix scrape misses: #1439/#1577/#1593/#1597"
  — caught that time only because the run cross-validated against GitHub's merged list
  by hand. Batch 20 did not repeat the cross-validation and the misses went unrecorded.
  The knowledge exists in the ledger as a batch-local anecdote; nothing in the tooling
  enforces it, so whether a batch is honest about its own coverage depends on whether
  that particular run happened to remember. Note also the second-order defect: blame
  attributes lines to a merge commit's **constituents**, never to the merge commit
  itself, so even a work-list that correctly contained #1883 would extract
  `FULLY SUPERSEDED` (an empty, falsely-clean review surface) if it passed the merge
  sha to the extractor. The #1593 "per-constituent special" in Batch 16 is the existing
  precedent for the right handling; it too is recorded only as prose.
  Concrete next action: make coverage a computed property of the sweep instead of a
  claim in its header. (1) Add a `--worklist <lo> <hi>` mode to
  [`historical-review-survivors.sh`](../../../../agents/scripts/core/historical-review-survivors.sh)
  (or a sibling `historical-review-worklist.sh`) that emits `{pr, sha}` units for a PR
  range and **fails loudly** when the two enumerators disagree. The gate's enumerator
  is the GitHub merged set — `gh pr list --state merged --base develop --json number,mergeCommit`,
  or in a `gh`-less session the `list_pull_requests(base=develop, state=closed)` MCP
  call filtered on non-null `merged_at`; the candidate set is the `(#N)` develop-log
  scrape. Replaying the motivating bug against that enumerator: for `lo=1878 hi=1940`
  the GitHub set contains rows `1883, 1919, 1920, 1921, 1923, 1927, 1932` that the
  scrape set does not, so the gate trips on exactly the 7 PRs Batch 20 lost — and on
  the Batch 13 set (#1439/#1577/#1593/#1597) for its own range. (2) When a work-list
  entry's resolved sha is a merge commit (`git rev-list --parents -n1 <sha>` reports
  2+ parents), expand it to one unit per constituent (`<first-parent>..<sha> --no-merges`)
  rather than emitting the merge sha, promoting the #1593 special into the default
  path. (3) Have the sweep return its own coverage triple (requested / reviewed /
  unreachable) so a batch header quotes a computed number rather than asserting one,
  and correct Batch 20's contiguity claim in the ledger. Est ~2–3 h for (1)+(2),
  ~1 h for (3). Cross-ref: Batch 13 header (the #1439/#1577/#1593/#1597 precedent);
  Batch 16 § the #1593 per-constituent special; Batch 20 header (the incorrect
  contiguity claim); the #1987 review-ref bug, the other case where the extractor
  degraded silently rather than failing loudly.
  Status: partially applied (2026-08-16 — shipped: parts (1) + (2). New gate
    [`historical-review-worklist.sh`](../../../../agents/scripts/core/historical-review-worklist.sh)
    builds the work-list from the GitHub merged set cross-validated against the
    develop-log scrape, **refuses to emit a scrape-only list** when no authority is
    available (`gh` absent and no `--merged-list`), reports any PR the scrape missed,
    fails loudly on a merged PR with no resolvable commit, and expands merge shas to
    per-constituent units. Validated by replaying the motivating bug against the real
    range: `--range 1878 1940` reports `authoritative 60 / scrape 53 / MISSED 1883
    1919 1920 1921 1923 1927 1932 / coverage 60/60 -> 64 units`, independently
    reconstructing the hand-built Batch 20-REDO work-list. `--selftest` carries 3 e2e
    fixtures, two of them asserts-failure (no-authority must exit 2; an unresolvable
    merged PR must exit 2). Wired into § Sweep status resume instructions and used to
    build Batch 22. **Remaining: part (3)** — have the sweep workflow itself return
    the coverage triple so a batch header quotes the computed number instead of
    re-asserting one; today the gate prints it to stderr and the orchestrator
    transcribes it.)
  Last-reviewed: 2026-08-16
