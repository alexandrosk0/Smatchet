- 2026-08-16 · orchestrator · [tooling] · P2 — the CR finding gate's auto-nudge posts as `github-actions[bot]`, and CodeRabbit ignores bot-authored triggers, so the gate's self-heal is inert: the comment lands, nobody is listening, and the PR stays parked
  Details: Two independent failures had to be fixed before this one was even
    observable, which is why it went unnoticed for as long as the nudge feature
    has existed:
    1. The nudge POST 403'd (`Resource not accessible by integration`) because
       the workflow granted only `issues: write`; a PR comment needs
       `pull-requests: write` (the fine-grained permission is keyed on target
       type, not endpoint path). Fixed in #2036 — the nudge now posts.
    2. With it finally posting, the end-to-end path could be observed for the
       first time — and CR does not answer it.
    Evidence, all on PR #2036: the nudge landed at 17:17:12Z as
    `github-actions[bot]` carrying `cr-first-review-nudge:b3697f2a…`. Twelve
    minutes later CR had posted nothing — no ack, no review node, no notice —
    and its newest word was still the 17:16:30Z "Review available on request",
    so this was NOT a rate-limit or in-progress window. Same signature as the
    sibling entry 2026-08-16-coderabbit-trigger-identity-and-rate-limit-noop: a
    `@coderabbitai review` posted as `claude[bot]` sat 31 minutes in silence
    while all 8 triggers CR ever acted on were authored by the user
    `alexandrosk0` — and a user-identity trigger on the same PR drew an ack in
    ~35 seconds. CR ignores bot comments (loop prevention); the nudge's author
    is a bot; therefore the nudge cannot trigger a review, only inform a human
    who happens to read the thread.
    Why it matters more now: #2036 closed the `manual review required`
    fail-open, so the gate correctly parks on PENDING instead of greening an
    unreviewed head. The nudge is the mechanism that was supposed to un-park it.
    An inert nudge turns a silent fail-open into a silent wedge — safer, but
    still requiring a human every time.
  Concrete next action: pick one, cheapest first.
    (a) Post the nudge under a NON-bot identity — a PAT stored as a repo secret
        (e.g. `CR_NUDGE_TOKEN`) used only for this POST. Restores real
        self-heal; costs a secret to rotate, and the token's owner becomes the
        apparent commenter.
    (b) Accept the nudge as a human-visible prompt only, and stop implying it
        self-heals: rename it in logs/comments to say a review is REQUIRED, and
        make the gate's PENDING description name the required action
        (`post @coderabbitai review — CR ignores bot triggers`). Honest, zero
        new surface, but every PR still needs a human or the orchestrator.
    (c) Verify whether CR's ignore-list is bot-authorship or specifically the
        `[bot]` suffix / app identity, by testing one nudge from a different app
        identity before investing in (a). Cheap experiment, decides (a) vs (b).
    Whichever is chosen, add a bats/doc assertion that the chosen mechanism is
    the one actually wired, so the next reader does not re-derive this from a
    silent thread.
  Status: open
  Last-reviewed: 2026-08-16
