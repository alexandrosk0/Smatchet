- 2026-08-11 · claude-code · [tooling] · P2 — a required context that stops reporting on EVERY PR is now only a WARN, so `postmortem-owed.sh --blocking` exits 0 on it; the effective-date fix traded this away to stop a SessionStart wedge, and only a merge-time snapshot buys it back

  Details: the effective-date fix for
  [`required-absent-judges-history-by-todays-required-set`](../applied.md) (applied
  2026-08-11) dates each required context from the earliest merge in the scan window
  where it was observed PRESENT, and skips PRs that merged before that. A context
  observed on **no** PR in the window cannot be dated at all, and the two
  explanations point opposite ways:

  - it was promoted so recently that nothing has run it yet — benign; or
  - it never reports at all — the #1941 shape, and one of the most serious escapes
    this detector exists to catch.

  From the window alone these are **indistinguishable**: both produce exactly "the
  name is in `required_contexts` and appears in zero rollups". So the undatable case
  is reported once as a `warns` line naming the ambiguity, and `warns` never affects
  the exit code.

  **The cost, stated plainly.** Before the fix, a required workflow that silently
  stopped reporting would flag every PR in the window and hard-fail `--blocking`.
  After it, that same outage produces one advisory line and a green `--blocking`.
  That is a real reduction in detection strength on the detector's headline case,
  and it was accepted only because the alternative re-creates the wedge the fix
  exists to remove: with `POSTMORTEM_BLOCKING_GRACE=0`, flagging per-PR means a
  routine branch-protection change hard-fails `--blocking` at SessionStart for up to
  `SCAN_N` PRs at once, wedging every new session on phantom escapes.

  Two mitigations already limit the blast radius, which is why this is P2 and not P1:

  - An **empty rollup** is exempt from the dating entirely and still flags. The
    reported escapes (#1941, #1972-#1974) are all empty-rollup merges, so the
    historical cases remain caught. The gap is narrower than "absence is unchecked":
    it is specifically *one* context missing from otherwise-populated rollups, across
    the whole window.
  - The WARN is emitted on every sweep, so the signal is never lost — only
    downgraded from blocking to advisory.

  Found by the pre-first-push adversarial review of the fix itself, which correctly
  called it out as "deliberate and documented, but the headline case of the detector
  added in the immediately preceding commit". Recording it rather than leaving the
  reasoning only in a code comment, because a deliberate trade-off that lives only in
  a comment is indistinguishable from an oversight six months later.

  Concrete next action: the fix is the OTHER candidate from the original entry —
  **persist the required set at merge time**. The merge-snapshot ledger
  (`merge-snapshots.jsonl`) already writes a per-merge record and is the natural
  home: add the branch-protection required-context list to the snapshot the watcher
  captures. Then "was this context required when this PR merged?" is a lookup rather
  than an inference, the effective-date heuristic and the undatable case both
  disappear, and a context that stops reporting can be flagged per-PR again without
  any risk of the promotion wedge. Note this only helps merges made AFTER the
  snapshot gains the field, so the window-derived dating has to stay as the fallback
  for older merges — the two coexist rather than one replacing the other.

  Status: open
  Last-reviewed: 2026-08-11
