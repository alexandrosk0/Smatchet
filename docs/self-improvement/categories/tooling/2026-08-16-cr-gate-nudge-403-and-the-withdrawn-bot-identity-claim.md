- 2026-08-16 · orchestrator · [tooling] · P2 — the CR finding gate's auto-nudge has never been able to POST (403, wrong permission axis); fixed in #2036, and the first end-to-end observation retracts the claim this entry originally carried — CodeRabbit DOES act on a `github-actions[bot]` trigger
  Details: **What is settled.** The nudge POST 403'd
    (`Resource not accessible by integration`) for as long as the feature has
    existed, because
    [`cr-finding-gate.yml`](../../../../.github/workflows/cr-finding-gate.yml)
    granted only `issues: write`; a comment on a PR needs
    `pull-requests: write` — the fine-grained permission is keyed on target
    type, not endpoint path, and the `/issues/{n}/comments` path is a decoy.
    Fixed in #2036; the nudge now posts. Everything below only became
    observable once it did.
    **What this entry originally claimed, and why it is withdrawn.** The first
    version concluded that CR ignores bot-authored triggers (loop prevention),
    so the gate's self-heal was inert by construction. Direct evidence on this
    same PR contradicts it. Full timeline, all 2026-08-16 on #2036:
    - `17:02:37Z` CR posts its walkthrough — this opens a rate-limit window.
    - `17:17:12Z` nudge by `github-actions[bot]` for `b3697f2a`. **No CR
      response of any kind** — the observation the withdrawn claim was built on.
    - `17:30:31Z` user trigger for `76aa640d` → `17:30:54Z` CR ack
      (invocation `4196d89b`) addressed `@alexandrosk0`; that pass then hit the
      review limit.
    - `17:30:36Z` nudge by `github-actions[bot]` for the same head, 5 s after
      the user trigger — no separate ack (plausibly coalesced).
    - `18:42:01Z` user trigger → `18:42:02Z` nudge by `github-actions[bot]` for
      `b818527b` → **two distinct CR invocations**: `9d6290ac` at `18:42:19Z`
      reading "`@github-actions`[bot] Reviewing `#2036` at `b818527b`", and a
      second, `6234e5f9` at `18:42:48Z` (an analysis-chain reply, on its face
      to the user comment).
    The load-bearing observation is `9d6290ac` on its own: CR opened an
    invocation whose addressee is the bot, named the bot's head SHA, and
    reviewed it. The blanket claim is therefore withdrawn, and so is its
    corollary that the gate's self-heal can never work. The confound worth
    stating plainly: a user trigger landed one second before the nudge, so this
    proves CR *reads and answers* a bot comment — not yet that a bot comment
    alone, with no user trigger in the same window, starts a review.
    **What is still open.** The `17:17:12Z` silence is unexplained. The
    cheapest hypothesis is quota, not identity: it landed ~15 min into the
    window opened at `17:02:37Z`. That does not fully fit either — a
    user-authored trigger inside the *same* window (`17:30:31Z`) still drew an
    ack. If that asymmetry is real, identity changes how loudly CR *declines*,
    not whether it honours a trigger. Separately, the sibling entry
    [`2026-08-16-coderabbit-trigger-identity-and-rate-limit-noop.md`](2026-08-16-coderabbit-trigger-identity-and-rate-limit-noop.md)
    records 31 min of silence on a `claude[bot]`-authored trigger with no such
    confound; that is a *different app identity* and stays an open observation.
    Its § (a) has been corrected in the same change to stop asserting the
    general "CR ignores bot-authored comments" mechanism.
  Concrete next action: (1) run the one clean experiment — on the next PR the
    gate parks, let the nudge fire with **no** user trigger inside the same
    hour, and record whether a review lands. That single observation separates
    identity from quota, and it costs nothing but the discipline to wait and
    write down the outcome. (2) Until it resolves, do NOT write a
    "never trigger CR from a bot identity" rule into
    [`merge-gates.md`](../../../agent-rules/merge-gates.md) — a false rule in a
    rule-doc costs more than an open question in the backlog, and the sibling
    entry's action item (1) is on hold for exactly this reason. (3) Independent
    of the outcome, make the gate's PENDING check description name the required
    action, so a parked PR is self-explanatory whoever has to unpark it.
  Status: open
  Last-reviewed: 2026-08-16
