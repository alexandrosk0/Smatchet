- 2026-08-07 · claude-code · [process] · P2 — a doc-correction sweep must grep the *distinguishing phrase*, not the subsystem name, or it silently misses files

  Correcting a stale claim across the docs tree ("the duplication gate is WARN-first" → "it is
  blocking") I swept for the subsystem tokens — `duplication`, `dup_audit` — and declared the
  drift bounded to two files. A reviewer then found a third, [`docs/CONTEXT.md`](../../../CONTEXT.md),
  which states the claim without naming the gate at all: it says only *"DRY is WARN-first today
  per ADR-0015"*. The subsystem grep could not have found it.

  The rule: sweep on the phrase that makes the claim **wrong** (`WARN-first`), not on the thing
  the claim is **about**. The wrong phrase is what needs to change, so it is the complete
  enumerator by construction; the subsystem name is only a proxy, and any doc that refers to
  the subsystem obliquely escapes it.

  Cost is real but bounded: `WARN-first` matches roughly **100 lines across 40 markdown files**
  on this worktree, most legitimately describing *other* gates still in calibration. Triage is a
  scan, not a rewrite, and it is the price of the sweep being complete rather than plausible.
  (Approximate deliberately: this entry contains the phrase several times, so an exact count
  self-invalidates on its own next revision — a hazard for any doc that counts a token it uses.)

  Two scoping notes learned by running it: frozen docs (`docs/plans/shipped/**`, `evaluation/**`)
  legitimately record what was true when written and should be **excluded by default** rather
  than "fixed" — a shipped plan describing the gate as WARN-first at the time is accurate
  history. And a stale-claim sweep is a *whole-file* scan, so a hit inside a fenced code block
  or a quoted historical excerpt is a false positive to skip, not a line to edit.

  Belongs as a line in [`docs/agent-rules/process-rules.md`](../../../agent-rules/process-rules.md)
  § Cadence and verification, next to the existing "use `test-markdown-links.sh` as the enumerator, not grep"
  note — same failure shape: a hand-rolled proxy standing in for a complete enumerator.
