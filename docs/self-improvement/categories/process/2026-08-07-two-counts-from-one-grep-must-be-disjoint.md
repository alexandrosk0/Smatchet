- 2026-08-07 · claude-code · [process] · P2 — when a claim reads "N sites do X, M do not-X" off a single grep, the two populations are usually **nested, not disjoint**; subtract before writing the numbers down

  Caught as a High on the [PR #1966](https://github.com/alexandrosk0/Smatchet/pull/1966)
  plan-doc addendum, and traced back into the already-pushed
  [PR #1984](https://github.com/alexandrosk0/Smatchet/pull/1984) entry it summarised
  ([`categories/test/2026-08-07-booted-app-or-skip-fails-open.md`](../test/2026-08-07-booted-app-or-skip-fails-open.md)).
  That entry stated the bucket-E guard "fails **open** at 56 call sites" and, four paragraphs
  later, that "**6** sites already fail **closed**". Both numbers came from the same
  `grep -c "AppController\* app = SmatchetActiveUiTestAppController();"` match set — the six
  fail-closed sites are *inside* the 56, because 56 counts the **assignment** shape, which every
  site shares regardless of what it does on the next line. The correct fail-open count is 50.
  The entry contradicted itself in its own text (6 + 56 ≠ 56) and neither I nor two earlier
  review passes read the two paragraphs against each other.

  This is a distinct failure mode from the fabricated-quote class
  ([`2026-08-07-fabricated-quote-is-a-class-not-an-instance.md`](../applied.md)).
  There the citation was invented; here every grep was real, its output was pasted correctly,
  and the arithmetic was never done. A measured number with a wrong population reads exactly
  like a measured number with the right one — there is no surface tell, which is why it survived
  further than the fabricated quotes did.

  Two mechanical checks, both cheap:

  1. **Assert disjointness explicitly.** When one grep shape underlies both counts, the second
     population is a *filter* of the first. Either re-grep the complement (`grep -L`, or grep the
     shape and subtract the exception files' own counts) or state the relationship in the text —
     "50 of its 56" rather than "56 … and separately 6".
  2. **Read the paragraphs against each other before shipping.** The contradiction here was
     internal to one file and visible without leaving it. A doc that quotes two counts of the
     same population owes a sentence saying how they relate.

  Same shape, different unit, in the addendum that summarised it: a sentence whose subject was
  "commit A **and** commit B" carried counts derived from B alone. Check: when a claim names
  multiple commits, run the union — `git diff --name-only <first>~1 <last> -- <path>` — rather
  than reading one commit's `--stat`.

  Belongs in [`docs/agent-rules/process-rules.md`](../../../agent-rules/process-rules.md)
  § Cadence and verification, next to the other verify-the-claim-not-the-tool rules.
