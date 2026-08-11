- 2026-08-11 · claude-code · [tooling] · P2 — `required-absent` judges every historical merge against TODAY's required-context set, so promoting a context retroactively flags PRs that merged before it existed and can hard-fail `--blocking` at SessionStart

  Details: `postmortem-owed.sh` reads the required set once
  (`REQ_CTX_JSON`, from `project.config.json`) and applies it uniformly to every PR
  in the scan window. A context that became required *after* a PR merged is absent
  from that PR's rollup **by design** — nothing was wrong with the merge — but the
  cross-check cannot tell that apart from a genuine escape and reports
  `required-absent`.

  Consequence: the first sweep after any required-context promotion or rename flags
  up to `SCAN_N` historical PRs at once. With `POSTMORTEM_BLOCKING_GRACE=0` that
  hard-fails `--blocking`, which runs at SessionStart — so a routine branch-protection
  change can wedge every new session until someone notices the flags are phantom.

  Found by an explicit self-review of the diff in PR #1996 and independently
  confirmed by CodeRabbit on the same PR. Both landed on the same two candidate
  fixes:

  - **Record the required set at merge time.** Most correct, most work: the sweep
    would need a per-PR snapshot of what was required when it merged, which nothing
    currently persists.
  - **Apply each context only from its effective date.** Cheaper and self-contained:
    derive a per-context "earliest observed present" from the scan window itself and
    skip any PR merged before it. Needs the row stream buffered — the loop is
    currently single-pass over a process substitution — so it is a real restructure
    of `postmortem-owed.sh:573-719`, not a patch.

  Deliberately **not** fixed in #1996. It fails LOUD and rarely (only on a
  required-set change), which is the opposite of the silent false-green class that
  PR exists to close; picking between the two designs is a judgement call rather
  than a defect fix, and doing it badly would put noise into the one gate that is
  supposed to be trustworthy. The sibling defect on the same detector — a POST-merge
  re-run reading as `required-never-terminal` — WAS fixed there, because it fires on
  ordinary PRs and the fix is local (ignore runs that started after `mergedAt`).

  Escape hatch until fixed: `POSTMORTEM_ABSENT_GRACE_SECONDS` does not help (the
  merges are old, so the grace has long elapsed). Either raise
  `POSTMORTEM_BLOCKING_GRACE` or narrow `SCAN_N` past the promotion date for one
  sweep.
