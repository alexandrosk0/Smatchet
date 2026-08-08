- 2026-08-07 · claude-code · [tooling] · P1 — `postmortem-owed.sh`'s first dedup probe matches `PR #N` **anywhere** in the ledger, so a prose mention of a PR inside an unrelated entry permanently suppresses that PR's own gate-escape nudge

  Observed after PR #1962 merged with `cr-out-of-band` +
  `cr-disposition:cr-rate-limited` — a real override escape (CodeRabbit's
  account quota was exhausted, so the diff was never reviewed). The escape owes
  a postmortem by
  [`AGENTS.md` § Self-improvement loop](../../../../AGENTS.md), but
  `bash agents/scripts/core/postmortem-owed.sh --list` reports
  `no gate escapes owed a postmortem (last 20 merges clean)`. Four consecutive
  invocations agree, so this is a deterministic miss, not a flake.

  Mechanism. `has_entry()`
  ([`postmortem-owed.sh:240-251`](../../../../agents/scripts/core/postmortem-owed.sh))
  runs two probes and the trigger-1 loop skips the PR when either fires:

  ```bash
  grep -qE "PR #$1([^0-9]|$)|commit $1([^0-9A-Fa-f]|$)" "$LEDGER" && return 0
  grep -qE "^#+ .*PR #[0-9].*[,[:space:]/]#$1([^0-9]|$)" "$LEDGER"
  ```

  The **second** probe is correctly scoped to a heading line (`^#+ …`) — its
  comment even states the intent: *"scoped to `^#+ …` so a #N mention in prose
  body can't false-suppress a real owe"*. The **first** probe is not scoped at
  all. It scans the whole file, so any sentence anywhere that happens to write
  `PR #1962` satisfies it. Measured against `origin/develop`'s ledger:

  ```
  grep -cE "PR #1962([^0-9]|$)"        → 2     # prose, inside an unrelated entry
  grep -cE "^#+ .*PR #1962([^0-9]|$)"  → 0     # no entry is actually filed
  ```

  Both hits are body prose in the 2026-08-05 `#1937` postmortem
  ([`postmortems.md:2171,2184`](../../postmortems.md)) — written by the very
  work that produced #1962, citing it as the determinism fix its instance
  ratchet rests on. Citing a PR is the normal way these entries are written, so
  the failure mode is not exotic: **any PR named in an existing entry's prose is
  silently exempted from ever being nudged again.** The suppression is
  permanent and silent — the detector's output is indistinguishable from a
  genuinely clean window, which is the same "mask discards the verdict" shape as
  [`2026-08-06-bucket-c-golden-mask-hides-stale-goldens.md`](2026-08-06-bucket-c-golden-mask-hides-stale-goldens.md).

  Blast radius: this is the *detector for gate escapes*. A hole here doesn't
  leak one defect, it suppresses the mechanism that converts escapes into new
  gates. Entries accumulate cross-references over time, so the exempted set only
  grows.

  Proposed gate — **dedup on a structured field, never on free prose.**

  1. **Scope probe 1 the same way probe 2 already is.** Require the `PR #N`
     match to land on an entry heading (`^#+ .*PR #N([^0-9]|$)`), matching the
     documented entry shape `## <date> · PR #N[, #M …] · <trigger>`. This is a
     one-line change and immediately un-suppresses #1962. The `commit <sha>`
     alternation should be split out and kept whole-file — `has_sha_entry`
     already documents bare-sha matching as deliberate for triggers 3+4.
  2. **Add a bats regression case.** `tests/bats/` should assert that a ledger
     containing only a *prose* `PR #N` mention still reports #N as owed, and
     that a real `## … PR #N …` heading dedupes it. This is the property both
     probes are trying to express and neither one tests.
  3. **Consider a machine-readable key.** Longer-term, have each entry carry an
     explicit `### Escaped PRs: #A, #B` field and dedup on that alone, so
     heading prose style can drift without re-opening the hole. Optional — (1)
     plus (2) closes the class.

  Item 1 is the fix; item 2 is what keeps it fixed. Do not apply from here —
  suggestion-only per the skill's finder/applier split.

  Secondary observation, low confidence, recorded rather than actioned: one
  earlier `--list` invocation in the same session emitted six owed escapes
  (#1979, #1974, #1971, #1964, #1968, #1954) while six others reported clean.
  Re-running four times after the fact was stable-clean, and the underlying
  `gh pr list` query returned 20 rows on three consecutive checks, so the
  transient was not a `gh` failure. The trigger-1 loop ends in
  `2>/dev/null || true`, which would turn any upstream failure into a silent
  "clean" — worth a `set -o pipefail` + explicit row-count assertion if it
  recurs, but it is not reproducible today and is **not** the cause of the
  #1962 miss (that one is fully explained above).
