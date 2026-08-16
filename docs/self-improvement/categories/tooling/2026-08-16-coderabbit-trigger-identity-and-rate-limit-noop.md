- 2026-08-16 · orchestrator · [tooling] · P2 — two silent ways an `@coderabbitai review` trigger does nothing: posted under a BOT identity it is dropped with no ack, and posted after a RATE-LIMITED pass on the same head it is a no-op because the head is already "seen" — both are indistinguishable from ordinary throttling, and each cost a full check-in cycle on PR #2023
  Details: **(a) Identity.** In the remote environment `$GITHUB_TOKEN` posts as
    `claude[bot]`, while `mcp__github__add_issue_comment` posts as the user.
    Evidence from #2023: all 8 triggers CR ever acted on were authored by
    `alexandrosk0`; the one posted via `curl` + `$GITHUB_TOKEN` sat for 31
    minutes with no ack, no rate-limit notice, and no review. There is no
    negative signal — the comment posts 201 and is simply never read — so the
    natural reading is "still throttled", and the wait is unbounded.
    **Correction (same day, from #2036):** this originally read "CodeRabbit
    ignores bot-authored comments (loop prevention)". That mechanism is wrong
    as stated — CR demonstrably acted on a `github-actions[bot]` trigger,
    acking it by name and reviewing the head. What survives is the narrower
    observation above: a `claude[bot]`-authored trigger drew nothing for 31
    min. Treat that as an unexplained result for that one app identity, not a
    policy, and read
    [`2026-08-16-cr-gate-nudge-403-and-the-withdrawn-bot-identity-claim.md`](2026-08-16-cr-gate-nudge-403-and-the-withdrawn-bot-identity-claim.md)
    for the full timeline and the experiment that would settle it. Note this does NOT retract the
    `curl -X PATCH` advice in the sibling entry
    [`2026-08-16-verdict-head-hex-hand-copied-into-pr-body.md`](2026-08-16-verdict-head-hex-hand-copied-into-pr-body.md):
    PR-*body* edits are identity-neutral and the token is the cheap path there.
    The split is the rule — bot token for body edits, user identity for anything
    CR must READ.
    **(b) Rate-limited head is consumed.** CR is incremental and "does not
    re-review already reviewed commits". A pass that reaches the head and THEN
    hits the limit still marks it seen, so the follow-up `@coderabbitai review`
    returns "Reviews are available now" and produces no review node — the
    `CR findings` gate stays correctly pending forever. The escape is
    `@coderabbitai full review`, which is exactly what the repo's own
    `cr-finding-gate` auto-nudge posts for this state; it worked first try
    (review node on head, `Merge Risk … up to 6b545`, 0 actionable).
    **(c) Gate-coverage gap found while diagnosing (b).** The auto-nudge did not
    fire here. Its clean-pass guard greps `no actionable (comments|findings)`,
    but CR's targeted verification reply said only "No findings", so `clean_ts`
    never advanced past `busy_ts` and the nudge stayed suppressed in precisely
    its target scenario. The rate-limit notice wording match has the same
    brittleness: it looks for `next review available`, while the follow-up said
    "Reviews are available now".
  Concrete next action: (1) add the already-settled rule to the CR rate-limit
    playbook in [`merge-gates.md`](../../../agent-rules/merge-gates.md) §
    CodeRabbit rate-limit playbook — *after any rate-limited pass on the
    current head, escalate to `full review` rather than repeating `review`,
    since a plain re-trigger is a no-op on an already-seen commit*. The
    identity half of this action item (*"never post under the bot token"*) is
    **on hold** pending the experiment in the sibling entry above — do not
    write it into a rule-doc until a positive observation supports it. Add the
    diagnostic tell: **a trigger that
    draws no CR response AND no limit notice within ~15 min is an authorship or
    already-seen problem, not throttling.** (2) Widen the `cr-finding-gate`
    clean-pass regex to also accept a bare `no findings` (and the busy regex to
    accept `reviews are available`), with a bats case per wording — the guard
    is only as good as its vocabulary, and it silently under-fires when CR
    rephrases. Est ~0.5d total; (2) is the part with a gate behind it.
  Status: open
  Last-reviewed: 2026-08-16
