- 2026-08-16 · orchestrator · [process] · P2 — a backlog entry asserted a third-party system's *policy* ("CodeRabbit ignores bot-authored triggers") on the strength of one non-response; a positive observation 85 minutes later contradicted it, on the same PR, before the entry had even merged
  Details: The sequence is worth keeping because nothing in it looks careless
    at the time. A nudge posted at `17:17:12Z` drew no CR response. Twelve
    minutes of nothing, plus a sibling entry recording a similar silence under
    a different bot identity, plus a plausible mechanism ready to hand (loop
    prevention — bots must not trigger bots), and the inference wrote itself. I
    filed it as a P2 with a settled-sounding title and three remediation
    options, one of which cost a repo secret. At `18:42:19Z` CR replied
    "`@github-actions`[bot] Reviewing `#2036` …" to a nudge from exactly that
    identity. The mechanism was never real.
    The defect is not the wrong guess — it is the *grade of claim*. Two
    genuinely different things got written in the same voice:
    - `17:17:12Z` produced no response — an **observation**, cheap and
      permanent; and
    - CR ignores bot-authored comments — a **mechanism**, which absence of a
      response cannot establish, because every competing explanation (quota,
      coalescing, a dropped webhook, an outage) produces the identical
      non-event.
    A non-response is compatible with every hypothesis, so it discriminates
    between none of them. Only a *positive* observation — a system doing
    something under condition A that it does not under condition B — licenses a
    mechanism. Silence licenses "unexplained".
    The cost is real even when caught. A wrong mechanism in the backlog is a
    wrong mechanism a later session will implement: the entry's cheapest-first
    remediation was to buy a PAT and store it as a repo secret, which would
    have added a rotating credential to the repo to solve a problem that did
    not exist. It also seeded a false sentence into a sibling entry's action
    item, which was queued for a rule-doc. Fabricated mechanisms propagate the
    same way fabricated quotes do, and the existing class-sweep rule in
    [`process-rules.md`](../../../agent-rules/process-rules.md) already covers
    the sweep once one is found — what is missing is the guard that stops it
    being written in the first place.
  Concrete next action: add one line to
    [`process-rules.md`](../../../agent-rules/process-rules.md) § Self-improvement
    entries — *an entry may state what was observed at any time; it may state
    WHY a third-party system behaved that way only from a positive observation.
    An inference drawn from a non-response is labelled `Hypothesis:` in the
    entry and MUST NOT appear in the entry's title, priority rationale, or any
    remediation that spends money, adds a credential, or lands in a rule-doc.*
    Mechanical to apply, and it would have downgraded this exact entry to
    "unexplained silence, experiment pending" — which is what it always was.
    Pairs with the class-sweep rule already there: that one cleans up after a
    false claim, this one keeps it out of the title.
  Status: open
  Last-reviewed: 2026-08-16
