- 2026-08-07 · claude-code · [process] · P1 — when review finds one fabricated verbatim quote, re-verify **every** quote in the changed doc; fixing only the flagged one leaves the class alive

  Twice in one session I wrote a fenced code block that quoted code existing nowhere in the
  tree. The first was caught as a Critical, and I fixed *that block*. The replacement text I
  wrote in the same edit contained a second fabricated block, caught as a Critical by the next
  review pass. Correcting the instance did nothing about the habit that produced it.

  The mechanism is specific and worth naming: a verbatim block reconstructed from memory of
  reading the file — rather than pasted from a fresh read — is plausible by construction. It
  uses the right identifiers in the right shape, so it survives every check except resolving it
  against the file. Reviewer attention and my own re-reading both slide over it.

  Rule: a finding of the form "this quote does not exist" is a **class** finding. Its fix is not
  the corrected quote — it is a sweep of every fenced block and every `file:line` citation in
  every file the diff touches, each one re-resolved by an actual read at the cited line. Cheap:
  a handful of `sed -n '<a>,<b>p'` calls. The cost of skipping it is a second Critical on the
  fix commit, which is what happened here.

  Then the sweep has to go one step further, because a third review pass on this same diff found
  a Critical the rule as stated above would have **missed**: a claim that five call sites "can
  still mint an orphan root node on a dead id" when all five already guard against exactly that.
  Every citation in that paragraph was correct; the *characterization* of what the cited code
  does was wrong — and the claim carried no line number at all, which is what let it through.
  So the sweep covers **every claim about what cited code does**, verified by reading the
  enclosing function rather than the cited line. A citation-shaped assertion with no citation is
  the highest-risk case, not the lowest.

  Corollary for authoring, not just for repair: quotes go into a doc by paste from a read
  performed for that purpose. If the block was typed rather than pasted, treat it as unverified
  until resolved, however confident it looks.

  Belongs in [`docs/agent-rules/process-rules.md`](../../../agent-rules/process-rules.md)
  § Cadence and verification, alongside the existing stale-`Edit` recovery rule — same shape
  (a stale mental model of a file standing in for the file).
