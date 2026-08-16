- 2026-08-16 · orchestrator · [process] · P2 — a new WARN-first gate shipped without measuring its false-positive ratio on the whole tree first; the rule as written was 8/12 false, which would have trained readers to ignore it
  Details: PR #2028 added a code-span repo-path check to
    [`test-markdown-links.sh`](../../../../agents/scripts/core/test-markdown-links.sh).
    It was authored, unit-tested, bats-tested and negative-tested against the
    delta scope — all green — and only a discretionary whole-tree `--all` run
    revealed the real signal quality: **12 warnings, of which 8 were false**. A
    backlog entry's proposal block names files it exists to CREATE ("Concrete
    next action: add a `scripts/dev/install-security-tools.sh`"), and those paths
    are the single most common repo-path code span in the backlog. The rule was
    narrowed (a structural, label-delimited proposal-block exemption) before
    landing, ending at 3 warnings — all genuine, all fixed.
    Residual, found by this very entry dogfooding the rule: the exemption is
    keyed on the entry's own proposal block, so QUOTING someone else's proposal
    inside a `Details:` block still warns (the sentence above does exactly that —
    the install-security-tools path it quotes is illustrative and is not on
    `develop`). That is the documented escape working as intended — the rule says
    *fix it, or note in prose that it is not on develop yet*, and this is the
    note — but it is a real residual worth watching: if quoted-proposal warnings
    become common, the exemption should widen from "inside a proposal block" to
    "any path in a sentence that names it as something to create".
    Why this matters beyond the one rule: a WARN-first gate's entire value is
    that a human reads the warning. A majority-false rule is worse than no rule,
    because it teaches the reader to skip the whole category — the same failure
    the `applied.md` exemption in that PR was independently added to avoid (~100
    archive rows that would have drowned the live signal). Delta-scoped tests
    cannot catch this: they only ever see the handful of lines a change touches,
    which is precisely the sample where a new rule looks clean.
    The near-miss was caught by discretion, not by process. Nothing in the
    ship-loop asks for the measurement, so the next WARN gate is one distracted
    author away from landing at 8/12.
  Concrete next action: add a line to
    [`process-rules.md`](../../../agent-rules/process-rules.md) § Cadence and
    verification — *a new or widened WARN-first rule must be run whole-tree
    (`--all` / equivalent) before push, and the PR body must state the warning
    count and the true/false split; a rule whose warnings are majority-false gets
    narrowed or scoped before it lands, never after.* Pairs naturally with the
    existing "gate, don't trust" principle: a gate nobody reads is not a gate.
    Cheap and mechanical — the measurement is one command, and stating it in the
    body makes the reviewer a second check on the calibration.
  Status: open
  Last-reviewed: 2026-08-16
